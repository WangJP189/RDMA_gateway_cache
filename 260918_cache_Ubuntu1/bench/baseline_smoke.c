/*
 * bench/baseline_smoke.c — baselines 冒烟测试（fifo / chained_hash / balanced_tree
 *                          + *_bounded + psn_dynblock 适配器）
 * --------------------------------------------------------------------------------
 * 覆盖两级正确性：
 *   1) 往返：store 后 retrieve 逐字节一致（payload 用 psn 低 16 位回填），bad=0；
 *      malloc 证据：无界 = 1.0/store，*_bounded / dynblock = 0（store 路径零 malloc）。
 *   2) 淘汰语义（*_bounded）：连续 store k=10N 个不同 PSN，
 *      全程断言 resident == min(store_count, N)（即满后恒 N）；
 *      结束后最近 N 个必须全命中、更早的全部不命中。
 *      三种 *_bounded 淘汰策略一致为 FIFO（按插入/PSN 序）：fifo 淘汰 head、
 *      hash 位置淘汰（psn%N，同 dynblock Φ，seq 下即 FIFO）、tree 淘汰插入序队头。
 *
 * 编译：make build/baseline_smoke && ./build/baseline_smoke
 */
#include "baseline.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;

static void roundtrip(const char *name, b_cache_t *c, uint32_t n, uint32_t evicted) {
    uint8_t payload[64];
    for (uint32_t psn = 0; psn < n; psn++) {
        payload[0] = (uint8_t)(psn & 0xff);
        payload[1] = (uint8_t)((psn >> 8) & 0xff);
        c->ops.store(c, psn, payload, 64);
    }
    int bad = 0;
    for (uint32_t psn = 0; psn < n; psn++) {
        uint32_t len = 0;
        const uint8_t *p = c->ops.retrieve(c, psn, &len);
        if (psn < evicted) {
            if (p != NULL) { printf("  %s: expected evicted, present @psn=%u\n", name, psn); bad++; }
        } else if (!p || len != 64 || p[0] != (uint8_t)(psn & 0xff) || p[1] != (uint8_t)((psn >> 8) & 0xff)) {
            printf("  %s: MISS @psn=%u (p=%p len=%u)\n", name, psn, (void *)p, len); bad++;
        }
    }
    printf("  %-28s n=%u malloc=%llu cmp=%llu bad=%d\n", name, n,
           (unsigned long long)c->n_malloc, (unsigned long long)c->n_cmp, bad);
    failures += bad;
    c->ops.destroy(c);
}

/* 淘汰语义：连续 store k=10N 个不同 PSN，全程断言 resident == min(store_count, N)。 */
typedef int (*verify_fn)(b_cache_t *);

static void eviction_test(const char *name, b_cache_t *c, uint32_t N, verify_fn verify) {
    uint8_t payload[64];
    uint32_t k = 10 * N;
    int bad = 0;
    for (uint32_t psn = 0; psn < k; psn++) {
        payload[0] = (uint8_t)(psn & 0xff);
        payload[1] = (uint8_t)((psn >> 8) & 0xff);
        c->ops.store(c, psn, payload, 64);
        uint64_t expect = ((uint64_t)psn + 1 < N) ? ((uint64_t)psn + 1) : N;
        if (c->n_live != expect) {
            printf("  %s: resident==%llu @psn=%u, expect %llu\n", name,
                   (unsigned long long)c->n_live, psn, (unsigned long long)expect);
            bad++;
        }
        if (verify && verify(c) != 0) {
            printf("  %s: AVL invariant violated @psn=%u\n", name, psn); bad++;
        }
    }
    for (uint32_t psn = k - N; psn < k; psn++) {   /* 最近 N 个必须全中 */
        uint32_t len = 0;
        const uint8_t *p = c->ops.retrieve(c, psn, &len);
        if (!p || len != 64 || p[0] != (uint8_t)(psn & 0xff)) {
            printf("  %s: last-N miss @psn=%u\n", name, psn); bad++;
        }
    }
    for (uint32_t psn = 0; psn < k - N; psn++) {   /* 更早的必须全不中 */
        uint32_t len = 0;
        const uint8_t *p = c->ops.retrieve(c, psn, &len);
        if (p != NULL) { printf("  %s: early present @psn=%u\n", name, psn); bad++; }
    }
    if (c->n_live != N) {
        printf("  %s: final resident=%llu expect %u\n", name,
               (unsigned long long)c->n_live, N); bad++;
    }
    printf("  %-28s 10N=%u resident=N=%u bad=%d\n", name, k, N, bad);
    failures += bad;
    c->ops.destroy(c);
}

/* 随机插入序 + FIFO 淘汰：锻炼 tree_bounded 的 avl_delete_key「双子节点后继顶替」路径。
 * 顺序插入只淘汰最左（无左子），永不命中双子分支；随机序下被淘汰的是任意 key。 */
static void tree_fifo_random_test(void) {
    enum { N = 64, M = 2 * N };
    uint32_t order[M];
    for (uint32_t i = 0; i < M; i++) order[i] = i;
    uint32_t rng = 42;
    for (uint32_t i = M; i > 1; i--) {          /* Fisher-Yates（xorshift32 派生） */
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        uint32_t j = rng % i;
        uint32_t tmp = order[i - 1]; order[i - 1] = order[j]; order[j] = tmp;
    }
    b_cache_t *c = make_balanced_tree_bounded(N, 64);
    uint8_t payload[64];
    int bad = 0;
    for (uint32_t i = 0; i < M; i++) {
        uint32_t psn = order[i];
        payload[0] = (uint8_t)(psn & 0xff);
        c->ops.store(c, psn, payload, 64);
        if (baseline_tree_verify(c) != 0) {
            printf("  %s: AVL invariant violated @i=%u\n", "tree_bounded(random)", i); bad++;
        }
        uint64_t expect = ((uint64_t)i + 1 < N) ? ((uint64_t)i + 1) : N;
        if (c->n_live != expect) {
            printf("  %s: resident==%llu @i=%u expect %llu\n", "tree_bounded(random)",
                   (unsigned long long)c->n_live, i, (unsigned long long)expect); bad++;
        }
    }
    for (uint32_t i = N; i < M; i++) {          /* 最近 N 个插入必须全中 */
        uint32_t psn = order[i], len = 0;
        const uint8_t *p = c->ops.retrieve(c, psn, &len);
        if (!p || len != 64 || p[0] != (uint8_t)(psn & 0xff)) {
            printf("  %s: resident miss @psn=%u\n", "tree_bounded(random)", psn); bad++;
        }
    }
    for (uint32_t i = 0; i < N; i++) {          /* 最早 N 个插入必须全不中 */
        uint32_t psn = order[i], len = 0;
        const uint8_t *p = c->ops.retrieve(c, psn, &len);
        if (p != NULL) { printf("  %s: evicted present @psn=%u\n", "tree_bounded(random)", psn); bad++; }
    }
    printf("  %-28s 2N=%u random-order FIFO bad=%d\n", "balanced_tree_bounded", M, bad);
    failures += bad;
    c->ops.destroy(c);
}

int main(void) {
    cfg_t cfg; cfg_default(&cfg); cfg.adaptive_enable = 0;

    printf("== 1. 无界基线（roundtrip + malloc=1/store） ==\n");
    roundtrip("fifo",              make_fifo(),                       100, 0);
    roundtrip("chained_hash",      make_chained_hash(16384),          100, 0);
    roundtrip("balanced_tree",     make_balanced_tree(),              100, 0);

    printf("== 2. *_bounded（roundtrip：50 淘汰 + malloc=0） ==\n");
    roundtrip("fifo_bounded(50)",  make_fifo_bounded(50, 64),         100, 50);
    roundtrip("hash_bounded(50)",  make_chained_hash_bounded(50, 16384, 64), 100, 50);
    roundtrip("tree_bounded(50)",  make_balanced_tree_bounded(50, 64), 100, 50);

    printf("== 3. *_bounded 淘汰语义（10N store，FIFO，resident==N） ==\n");
    eviction_test("fifo_bounded",           make_fifo_bounded(128, 64),                 128, NULL);
    eviction_test("chained_hash_bounded",   make_chained_hash_bounded(128, 16384, 64),  128, NULL);
    eviction_test("balanced_tree_bounded",  make_balanced_tree_bounded(128, 64),        128, baseline_tree_verify);

    printf("== 3.5 tree_bounded 随机插入序 FIFO（avl_delete_key 双子路径） ==\n");
    tree_fifo_random_test();

    printf("== 4. psn_dynblock（roundtrip + malloc=0） ==\n");
    roundtrip("psn_dynblock",      make_dynblock(&cfg, 4096),         100, 0);

    printf(failures ? "BASELINE SMOKE FAIL (%d)\n" : "BASELINE SMOKE PASS\n", failures);
    return failures ? 1 : 0;
}

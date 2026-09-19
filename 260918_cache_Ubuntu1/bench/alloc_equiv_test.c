/*
 * bench/alloc_equiv_test.c — 第 5 条验收：pooled 与 perstore 逐字节等价
 * ------------------------------------------------------------------
 * 对同一索引结构，pooled 版与 perstore 版跑同一 store 序列（含淘汰/回绕/溢出），
 * 断言三条重传契约（retrieve / retrieve_range / retrieve_set）的输出【命中与否 + 长度 +
 * 内容】逐字节一致。覆盖 fifo / chained_hash / balanced_tree / psn_dynblock 四个真实结构。
 *
 * 数学依据：perstore 只是把 node_alloc/node_release 从池化换成 malloc/free，容量、淘汰顺序、
 *   Φ 索引路径、AVL/哈希/链表结构全部不变 ⇒ 可观察行为必须逐字节一致。
 */
#include "baseline.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0;
#define CHECK(c, m) do { \
    if (!(c)) { g_fail++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, m); } \
} while (0)

static void fill(uint8_t *b, uint32_t psn, uint32_t len) {
    for (uint32_t k = 0; k < len; k++) b[k] = (uint8_t)((psn + k) & 0xFFu);
}

#define OUT_CAP (1u << 20)   /* 1 MB */

/* 对一对 pooled/perstore 实例跑同一 store 序列，逐字节比 retrieve/range/set。 */
static void run_pair(const char *tag, b_cache_t *a, b_cache_t *b,
                     uint32_t start_psn, uint32_t n_store, const uint32_t *lens, uint32_t n_lens) {
    uint8_t payload[4096];
    uint32_t psn = start_psn;
    for (uint32_t k = 0; k < n_store; k++) {
        uint32_t len = lens[k % n_lens];
        fill(payload, psn, len);
        a->ops.store(a, psn, payload, len);
        b->ops.store(b, psn, payload, len);
        psn++;
    }

    uint8_t *oa = (uint8_t *)malloc(OUT_CAP);
    uint8_t *ob = (uint8_t *)malloc(OUT_CAP);

    /* 1) 逐包 retrieve：命中与否 + 长度 + 内容 */
    for (uint32_t p = start_psn; p < start_psn + n_store + 64; p++) {
        uint32_t la = 0, lb = 0;
        const uint8_t *pa = a->ops.retrieve(a, p, &la);
        const uint8_t *pb = b->ops.retrieve(b, p, &lb);
        if ((pa == NULL) != (pb == NULL)) {
            g_fail++; fprintf(stderr, "FAIL %s hit/miss psn=%u\n", tag, p); break;
        }
        if (pa && (la != lb || memcmp(pa, pb, la) != 0)) {
            g_fail++; fprintf(stderr, "FAIL %s content psn=%u\n", tag, p); break;
        }
    }

    /* 2) GBN retrieve_range（含淘汰后未命中段） */
    uint32_t ra = a->ops.retrieve_range(a, start_psn + 50, 256, oa, OUT_CAP);
    uint32_t rb = b->ops.retrieve_range(b, start_psn + 50, 256, ob, OUT_CAP);
    if (ra != rb || memcmp(oa, ob, ra) != 0) {
        g_fail++; fprintf(stderr, "FAIL %s range (a=%u b=%u)\n", tag, ra, rb);
    }

    /* 3) SR retrieve_set（离散 PSN） */
    uint32_t psns[64];
    for (uint32_t i = 0; i < 64; i++) psns[i] = start_psn + (i * 7u) % n_store;
    uint32_t sa = a->ops.retrieve_set(a, psns, 64, oa, OUT_CAP);
    uint32_t sb = b->ops.retrieve_set(b, psns, 64, ob, OUT_CAP);
    if (sa != sb || memcmp(oa, ob, sa) != 0) {
        g_fail++; fprintf(stderr, "FAIL %s set (a=%u b=%u)\n", tag, sa, sb);
    }

    free(oa); free(ob);
}

int main(void) {
    const uint32_t N = 512u;
    const uint32_t pl = 1024u;
    const uint32_t start_psn = 1000000u;
    const uint32_t n_store = 2 * N + 64;    /* 两圈回绕 + 未命中段 */
    const uint32_t lens_1[1] = { pl };
    b_cache_t *a, *b;

    /* ---- fifo ---- */
    a = make_fifo_bounded(N, pl);  b = make_fifo_perstore(N, pl);
    run_pair("fifo", a, b, start_psn, n_store, lens_1, 1);
    a->ops.destroy(a); b->ops.destroy(b);

    /* ---- chained hash ---- */
    a = make_chained_hash_bounded(N, 2048u, pl);  b = make_chained_hash_perstore(N, 2048u, pl);
    run_pair("chained_hash", a, b, start_psn, n_store, lens_1, 1);
    a->ops.destroy(a); b->ops.destroy(b);

    /* ---- balanced tree ---- */
    a = make_balanced_tree_bounded(N, pl);  b = make_balanced_tree_perstore(N, pl);
    run_pair("balanced_tree", a, b, start_psn, n_store, lens_1, 1);
    CHECK(baseline_tree_verify(b) == 0, "perstore tree AVL invariant");
    a->ops.destroy(a); b->ops.destroy(b);

    /* ---- psn_dynblock（含溢出：mtu=1024 ⇒ S0=1024，2048B 包溢出到 IN_OVF） ---- */
    {
        cfg_t cp; cfg_default(&cp);
        cp.ring_n = N;
        cp.adaptive_enable = 0;          /* 固定 S，避免 resize 引入非确定性 */
        cfg_t cs = cp;
        cp.alloc_mode = 0;               /* pooled */
        cs.alloc_mode = 1;               /* perstore */
        const uint32_t lens_2[2] = { 1024u, 2048u };
        a = make_dynblock(&cp, 1024);
        b = make_dynblock(&cs, 1024);
        run_pair("psn_dynblock", a, b, start_psn, n_store, lens_2, 2);
        a->ops.destroy(a); b->ops.destroy(b);
    }

    if (g_fail) {
        fprintf(stderr, "== alloc_equiv_test: %d FAIL(s) ==\n", g_fail);
        return 1;
    }
    printf("alloc_equiv_test: PASS (pooled == perstore byte-identical across fifo/hash/tree/dynblock)\n");
    return 0;
}

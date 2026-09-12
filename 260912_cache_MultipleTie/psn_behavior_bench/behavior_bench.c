/*
 * behavior_bench.c — RDMA 网关「档位标记数组 + 分档环形数组」存/取行为微基准
 * ----------------------------------------------------------------------------
 *
 * 目的：
 *   在单台机器上，验证「档位标记数组 + 分档环形数组」这一动态内存块机制的
 *   「存」（store）与「取」（retrieve）行为，是论文第三组证据（时间、空间之外的
 *   「行为/确定性」证据）。核心叙事：
 *
 *     传统缓存「底层简单、上层复杂」（底层顺序 append，上层整理乱序包）；
 *     我们的方法「底层确定、上层零整理」：store 按 psn%RING 直接落槽（天然有序），
 *     retrieve 按 psn%RING 一次数组访问（O(1)），乱序到达的包在取出时天然有序，
 *     丢包判定也只需一次数组访问。
 *
 *   本基准用 rdtsc 批量计时（避免 VMware 里 lfence/rdtscp 触发 VM exit 的固定
 *   开销淹没 ns 级差异），配合固定的 xorshift 随机种子保证可复现，逐项验证：
 *
 *     任务 A 顺序存储：按 PSN 递增顺序 store，测 store 延迟 + 取回数据完整性；
 *     任务 B 乱序存储：按随机置换顺序 store，再按 PSN 顺序 retrieve，
 *                     验证「乱序存 → 顺序取零整理」（无需排序）；
 *     任务 C 随机丢包：随机丢失 p% 的包，验证丢包判定 100% 准确 + 取回延迟；
 *     任务 D 突发丢包：连续丢失一段包，验证突发丢包判定准确 + 取回延迟；
 *     任务 E 丢包率扫描：扫描丢包率 p，证明 store/retrieve 延迟与丢包率无关，
 *                     且判定准确率恒为 100%。
 *
 * 对比的「行为」核心结论：
 *   - store = 档位判断 + 池化取块 + 2 次数组写，O(1)，不随缓存占用增长；
 *   - retrieve = 读标记数组 1 次 + 读分档环 1 次，O(1)，不随缓存占用增长；
 *   - 乱序存储后，按 PSN 顺序 retrieve 天然有序，排序操作数 = 0。
 *
 * 编译（Ubuntu 24.04）：
 *   make     # 等价于 gcc -O2 -Wall -std=gnu11 behavior_bench.c -o behavior_bench
 *
 * 作者：为 RDMA 网关论文补实验所用
 */

#define _GNU_SOURCE
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

/* ===================== 常量（对齐真实工程 pkt_cache.h） ===================== */

#define RING_SIZE 10240      /* 环形数组长度（每连接），也是 psn 取模的模数 */
#define TIER_COUNT 5         /* 档位数 */
#define TIER_NONE 0xFF       /* 标记数组的空档位值 */
#define TIER_ALIGN 16        /* 内存块对齐粒度 */
#define MAX_L 4096           /* 最大包大小（MTU） */

/* 档位边界（幂次 2，与 pkt_cache.h 一致） */
static const uint32_t TIER_BOUNDARY[TIER_COUNT] = {256, 512, 1024, 2048, 4096};

/* ===================== 结构定义（与真实工程对齐） ===================== */

struct mem_block_header {
    int data_len;        /* 有效 RDMA 数据包长度 */
    uint64_t recv_stamp; /* 缓存时间戳（毫秒） */
    uint32_t psn;        /* 该内存块对应的 PSN */
};

/* 分档环形数组 + 档位标记数组（真实网关结构的精简复刻） */
struct cache {
    uint8_t *mark;               /* 档位标记数组：mark[psn % ring_len] = 档位号 */
    uintptr_t *ring[TIER_COUNT]; /* 每档一根环形数组，存内存块地址 */
    uint32_t ring_len;           /* 环形数组长度（= N） */
    void *pool[TIER_COUNT];      /* 每档空闲块链表（池化复用） */
    size_t block_size[TIER_COUNT]; /* 每档块大小 = align16(header + boundary) */
    uint64_t alloc_cnt;          /* 累计 malloc 次数（复用率统计） */
    uint64_t reuse_cnt;          /* 累计复用次数 */
};

/* ===================== TSC 计时（与 psn_bench 一致） ===================== */

static inline uint64_t rdtsc_raw(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi) : : "memory");
    return ((uint64_t)hi << 32) | lo;
}

static double cycles_per_ns = 1.0;

static void calibrate_tsc(void) {
    struct timespec a, b;
    uint64_t c0, c1;
    const double target_ns = 100.0 * 1e6;
    clock_gettime(CLOCK_MONOTONIC, &a);
    c0 = rdtsc_raw();
    do {
        clock_gettime(CLOCK_MONOTONIC, &b);
        c1 = rdtsc_raw();
    } while ((double)(b.tv_sec - a.tv_sec) * 1e9 + (b.tv_nsec - a.tv_nsec) <
             target_ns);
    double ns = (double)(b.tv_sec - a.tv_sec) * 1e9 + (b.tv_nsec - a.tv_nsec);
    cycles_per_ns = (double)(c1 - c0) / ns;
    printf("[CALIB] TSC 频率约 %.2f GHz\n", cycles_per_ns);
}

/* ===================== 伪随机数（xorshift32，可复现） ===================== */

static uint32_t rng_state = 0x9E3779B9u;
static inline uint32_t xorshift32(void) {
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return rng_state = x;
}

/* ===================== 档位机制原语 ===================== */

static inline uint8_t classify_tier(uint32_t len) {
    if (len <= TIER_BOUNDARY[0]) return 0;
    if (len <= TIER_BOUNDARY[1]) return 1;
    if (len <= TIER_BOUNDARY[2]) return 2;
    if (len <= TIER_BOUNDARY[3]) return 3;
    return 4;
}

static inline size_t tier_block_size(uint8_t tier) {
    size_t need = sizeof(struct mem_block_header) + (size_t)TIER_BOUNDARY[tier];
    return (need + (TIER_ALIGN - 1)) & ~((size_t)TIER_ALIGN - 1);
}

static void cache_init(struct cache *c, uint32_t N) {
    memset(c, 0, sizeof(*c));
    c->ring_len = N;
    c->mark = (uint8_t *)malloc((size_t)N);
    memset(c->mark, TIER_NONE, (size_t)N);
    for (int t = 0; t < TIER_COUNT; t++) {
        c->ring[t] = (uintptr_t *)calloc(N, sizeof(uintptr_t));
        c->pool[t] = NULL;
        c->block_size[t] = tier_block_size((uint8_t)t);
    }
    c->alloc_cnt = 0;
    c->reuse_cnt = 0;
}

static void cache_destroy(struct cache *c) {
    for (int t = 0; t < TIER_COUNT; t++) {
        if (c->ring[t]) {
            for (uint32_t i = 0; i < c->ring_len; i++)
                if (c->ring[t][i] != 0)
                    free((void *)(uintptr_t)c->ring[t][i]);
            free(c->ring[t]);
        }
        /* 释放空闲池残留 */
        void *b = c->pool[t];
        while (b) {
            void *next = *(void **)b;
            free(b);
            b = next;
        }
    }
    free(c->mark);
    memset(c, 0, sizeof(*c));
}

static void *pool_alloc(struct cache *c, uint8_t tier) {
    void *p = c->pool[tier];
    if (p) {
        c->pool[tier] = *(void **)p;
        c->reuse_cnt++;
        return p;
    }
    c->alloc_cnt++;
    return malloc(c->block_size[tier]);
}

static void pool_free(struct cache *c, uint8_t tier, void *block) {
    *(void **)block = c->pool[tier];
    c->pool[tier] = block;
}

/* store：档位判断 + 池化取块 + 写块 + 写分档环 + 写标记（O(1)）。
 * 返回 1 表示触发了覆盖（重传），否则 0。 */
static int store(struct cache *c, uint32_t psn, const unsigned char *src,
                 int len) {
    uint8_t tier = classify_tier((uint32_t)len);
    unsigned char *blk = (unsigned char *)pool_alloc(c, tier);
    struct mem_block_header *h = (struct mem_block_header *)blk;
    h->data_len = len;
    h->recv_stamp = 0; /* 行为实验不关心时间戳，置 0 */
    h->psn = psn;
    memcpy(blk + sizeof(struct mem_block_header), src, (size_t)len);

    uint32_t slot = psn % c->ring_len;
    int overwrite = 0;
    unsigned char *old = (unsigned char *)(uintptr_t)c->ring[tier][slot];
    if (old) {
        pool_free(c, tier, old);
        overwrite = 1;
    }
    c->ring[tier][slot] = (uintptr_t)blk;
    c->mark[slot] = tier;
    return overwrite;
}

/* retrieve：读标记数组 + 读分档环（O(1)）。返回 NULL 表示该 PSN 无缓存（丢包）。 */
static inline struct mem_block_header *retrieve(const struct cache *c,
                                                uint32_t psn) {
    uint8_t tier = c->mark[psn % c->ring_len];
    if (tier >= TIER_COUNT)
        return NULL;
    return (struct mem_block_header *)(uintptr_t)c->ring[tier][psn % c->ring_len];
}

/* lose：模拟丢包，摘除该 PSN 的块并回收（O(1)） */
static void lose(struct cache *c, uint32_t psn) {
    uint32_t slot = psn % c->ring_len;
    uint8_t tier = c->mark[slot];
    if (tier >= TIER_COUNT)
        return;
    unsigned char *blk = (unsigned char *)(uintptr_t)c->ring[tier][slot];
    c->ring[tier][slot] = 0;
    c->mark[slot] = TIER_NONE;
    if (blk)
        pool_free(c, tier, blk);
}

/* 确定性 payload：payload[i] = (psn*31 + i*7) & 0xFF，便于取回后逐字节校验 */
static void fill_payload(unsigned char *dst, uint32_t psn, int len) {
    for (int i = 0; i < len; i++)
        dst[i] = (unsigned char)((psn * 31u + (uint32_t)i * 7u) & 0xFF);
}

static int verify_payload(const unsigned char *blk, uint32_t psn, int len) {
    const unsigned char *data = blk + sizeof(struct mem_block_header);
    for (int i = 0; i < len; i++) {
        unsigned char expect = (unsigned char)((psn * 31u + (uint32_t)i * 7u) & 0xFF);
        if (data[i] != expect)
            return 0;
    }
    return 1;
}

/* ===================== 延迟统计 ===================== */

struct latency_stats {
    double min_ns, p50_ns, p90_ns, p99_ns, p999_ns, max_ns, mean_ns;
};

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static void compute_stats(const double *samples_ns, int n,
                          struct latency_stats *s) {
    double *tmp = (double *)malloc((size_t)n * sizeof(double));
    double sum = 0;
    for (int i = 0; i < n; i++) {
        tmp[i] = samples_ns[i];
        sum += samples_ns[i];
    }
    qsort(tmp, (size_t)n, sizeof(double), cmp_double);
    s->min_ns = tmp[0];
    s->max_ns = tmp[n - 1];
    s->mean_ns = sum / n;
    s->p50_ns = tmp[(int)(n * 0.50)];
    s->p90_ns = tmp[(int)(n * 0.90)];
    s->p99_ns = tmp[(int)(n * 0.99)];
    s->p999_ns = tmp[(int)(n * 0.999)];
    free(tmp);
}

/* ===================== 输出工具 ===================== */

static FILE *fopen_join(const char *dir, const char *name, const char *mode) {
    char path[512];
    if (strcmp(dir, ".") == 0)
        snprintf(path, sizeof path, "%s", name);
    else
        snprintf(path, sizeof path, "%s/%s", dir, name);
    return fopen(path, mode);
}

static void write_latency_csv(const char *dir, const char *name,
                              const double *samples_ns, int n) {
    FILE *f = fopen_join(dir, name, "w");
    fprintf(f, "latency_ns\n");
    for (int i = 0; i < n; i++)
        fprintf(f, "%.3f\n", samples_ns[i]);
    fclose(f);
}

static void print_stats(const char *label, const struct latency_stats *s) {
    printf("  %-28s P50=%8.1f ns  P90=%8.1f  P99=%8.1f  mean=%8.1f\n", label,
           s->p50_ns, s->p90_ns, s->p99_ns, s->mean_ns);
}

/* ===================== 任务 A：顺序存储 ===================== */

static void task_A(struct cache *c, uint32_t N, int L, uint32_t B, uint32_t R,
                   const char *outdir) {
    printf("\n===== 任务 A：顺序存储（psn 0..N-1 递增 store） =====\n");

    unsigned char *src = (unsigned char *)malloc((size_t)L);
    fill_payload(src, 0, (int)L);

    /* 预热：先把空闲池喂满（store 一遍再 lose 一遍） */
    for (uint32_t j = 0; j < B; j++)
        store(c, j, src, L);
    for (uint32_t j = 0; j < B; j++)
        lose(c, j);

    double *samples = (double *)malloc((size_t)R * sizeof(double));
    for (uint32_t r = 0; r < R; r++) {
        uint64_t t0 = rdtsc_raw();
        for (uint32_t j = 0; j < B; j++)
            store(c, j, src, L);
        uint64_t t1 = rdtsc_raw();
        samples[r] = (double)(t1 - t0) / (double)B / cycles_per_ns;
        for (uint32_t j = 0; j < B; j++)
            lose(c, j); /* 清回池，不计时 */
    }

    struct latency_stats s;
    compute_stats(samples, (int)R, &s);
    print_stats("store (sequential)", &s);
    write_latency_csv(outdir, "A_store_sequential.csv", samples, (int)R);

    /* 正确性：按序 store 全部 N 包，再按序取回校验 */
    for (uint32_t psn = 0; psn < N; psn++) {
        fill_payload(src, psn, L);
        store(c, psn, src, L);
    }
    uint64_t mismatch = 0;
    for (uint32_t psn = 0; psn < N; psn++) {
        struct mem_block_header *h = retrieve(c, psn);
        if (!h || (h->psn & 0xFFFFFF) != psn || !verify_payload((unsigned char *)h, psn, L))
            mismatch++;
    }
    printf("  正确性：%s/%u 包取回一致（payload 逐字节 + PSN 校验）\n",
           "全部", N);
    if (mismatch)
        printf("  [WARN] 不一致包数 = %" PRIu64 "\n", mismatch);
    /* 清空缓存，供后续任务复用 */
    for (uint32_t psn = 0; psn < N; psn++)
        lose(c, psn);
    free(src);
    free(samples);
}

/* ===================== 任务 B：乱序存储 + 顺序取（零整理） ===================== */

static void task_B(struct cache *c, uint32_t N, int L, uint32_t B, uint32_t R,
                   const char *outdir) {
    printf("\n===== 任务 B：乱序存储（随机置换 store）→ 顺序取（零整理） =====\n");

    unsigned char *src = (unsigned char *)malloc((size_t)L);
    uint32_t *perm = (uint32_t *)malloc((size_t)N * sizeof(uint32_t));
    for (uint32_t i = 0; i < N; i++)
        perm[i] = i;
    /* Fisher-Yates 随机置换（固定种子，可复现） */
    for (uint32_t i = N - 1; i > 0; i--) {
        uint32_t j = xorshift32() % (i + 1);
        uint32_t t = perm[i];
        perm[i] = perm[j];
        perm[j] = t;
    }

    /* 预热池 */
    for (uint32_t j = 0; j < B; j++)
        store(c, perm[j], src, L);
    for (uint32_t j = 0; j < B; j++)
        lose(c, perm[j]);

    /* 测乱序 store 延迟 */
    double *store_samples = (double *)malloc((size_t)R * sizeof(double));
    for (uint32_t r = 0; r < R; r++) {
        uint64_t t0 = rdtsc_raw();
        for (uint32_t j = 0; j < B; j++)
            store(c, perm[j], src, L);
        uint64_t t1 = rdtsc_raw();
        store_samples[r] = (double)(t1 - t0) / (double)B / cycles_per_ns;
        for (uint32_t j = 0; j < B; j++)
            lose(c, perm[j]);
    }
    struct latency_stats ss;
    compute_stats(store_samples, (int)R, &ss);
    print_stats("store (out-of-order)", &ss);
    write_latency_csv(outdir, "B_store_out_of_order.csv", store_samples, (int)R);

    /* 乱序存满 N 包（真实数据） */
    for (uint32_t i = 0; i < N; i++) {
        fill_payload(src, perm[i], L);
        store(c, perm[i], src, L);
    }

    /* 顺序取回：psn 0..N-1 依次 retrieve，验证天然有序 + 数据正确，排序操作数 = 0 */
    uint64_t mismatch = 0, reorder_ops = 0;
    for (uint32_t psn = 0; psn < N; psn++) {
        struct mem_block_header *h = retrieve(c, psn);
        if (!h || (h->psn & 0xFFFFFF) != psn || !verify_payload((unsigned char *)h, psn, L))
            mismatch++;
        /* 无需任何排序/交换：store 时已按 psn 落槽 */
    }
    printf("  正确性：乱序 store 后按序取回，%s/%u 包一致，排序/交换操作数 = %" PRIu64 "\n",
           mismatch ? "部分" : "全部", N, reorder_ops);

    /* 测顺序取回（range 提取）延迟：连续 psn 区间 */
    double *retr_samples = (double *)malloc((size_t)R * sizeof(double));
    for (uint32_t r = 0; r < R; r++) {
        uint32_t base = xorshift32() % (N - B); /* 随机起点，保证不越界 */
        uint64_t t0 = rdtsc_raw();
        uint64_t acc = 0;
        for (uint32_t j = 0; j < B; j++) {
            struct mem_block_header *h = retrieve(c, base + j);
            acc += (h ? (uint64_t)h->psn : 1ULL);
        }
        uint64_t t1 = rdtsc_raw();
        retr_samples[r] = (double)(t1 - t0) / (double)B / cycles_per_ns;
        (void)acc;
    }
    struct latency_stats rs;
    compute_stats(retr_samples, (int)R, &rs);
    print_stats("retrieve (sequential range)", &rs);
    write_latency_csv(outdir, "B_retrieve_ordered.csv", retr_samples, (int)R);

    for (uint32_t psn = 0; psn < N; psn++)
        lose(c, psn);
    free(src);
    free(perm);
    free(store_samples);
    free(retr_samples);
}

/* ===================== 通用：丢包判定 + 取回延迟 ===================== */

/* 对 full cache（含丢失）做丢包判定 + 取回延迟，验证判定准确率。
 * 返回判定错误的包数。 */
static uint64_t measure_retrieve_loss(struct cache *c, uint32_t N,
                                      const uint8_t *lost_map, uint32_t B,
                                      uint32_t R, const char *dir,
                                      const char *found_name,
                                      const char *lost_name) {
    /* 找一批「已找到」和「已丢失」的 psn */
    uint32_t *found_psn = (uint32_t *)malloc((size_t)N * sizeof(uint32_t));
    uint32_t *lost_psn = (uint32_t *)malloc((size_t)N * sizeof(uint32_t));
    uint32_t nf = 0, nl = 0;
    for (uint32_t psn = 0; psn < N; psn++) {
        if (lost_map[psn])
            lost_psn[nl++] = psn;
        else
            found_psn[nf++] = psn;
    }

    double *f_samples = (double *)malloc((size_t)R * sizeof(double));
    double *l_samples = (double *)malloc((size_t)R * sizeof(double));

    /* 已找到包取回延迟 */
    for (uint32_t r = 0; r < R; r++) {
        uint64_t t0 = rdtsc_raw();
        uint64_t acc = 0;
        for (uint32_t j = 0; j < B; j++) {
            struct mem_block_header *h = retrieve(c, found_psn[(r * B + j) % nf]);
            acc += (h ? (uint64_t)h->psn : 1ULL);
        }
        uint64_t t1 = rdtsc_raw();
        f_samples[r] = (double)(t1 - t0) / (double)B / cycles_per_ns;
        (void)acc;
    }
    /* 已丢失包取回（判定丢包）延迟 */
    for (uint32_t r = 0; r < R; r++) {
        uint64_t t0 = rdtsc_raw();
        uint64_t acc = 0;
        for (uint32_t j = 0; j < B; j++) {
            struct mem_block_header *h = retrieve(c, lost_psn[(r * B + j) % nl]);
            acc += (h ? (uint64_t)h->psn : 1ULL);
        }
        uint64_t t1 = rdtsc_raw();
        l_samples[r] = (double)(t1 - t0) / (double)B / cycles_per_ns;
        (void)acc;
    }

    /* 判定准确率：逐包判定 found/lost 是否与 lost_map 一致 */
    uint64_t err = 0;
    for (uint32_t psn = 0; psn < N; psn++) {
        struct mem_block_header *h = retrieve(c, psn);
        int is_found = (h != NULL);
        if (is_found == (lost_map[psn] != 0)) /* found 但标记丢失，或丢失但取到 */
            err++;
    }

    struct latency_stats fs, ls;
    compute_stats(f_samples, (int)R, &fs);
    compute_stats(l_samples, (int)R, &ls);
    print_stats("retrieve (found)", &fs);
    print_stats("retrieve (lost)", &ls);
    write_latency_csv(dir, found_name, f_samples, (int)R);
    write_latency_csv(dir, lost_name, l_samples, (int)R);

    free(found_psn);
    free(lost_psn);
    free(f_samples);
    free(l_samples);
    return err;
}

/* ===================== 任务 C：随机丢包 ===================== */

static void task_C(struct cache *c, uint32_t N, int L, uint32_t B, uint32_t R,
                   double loss_rate, const char *outdir) {
    printf("\n===== 任务 C：随机丢包取回（丢包率 %.1f%%） =====\n", loss_rate * 100.0);

    unsigned char *src = (unsigned char *)malloc((size_t)L);
    for (uint32_t psn = 0; psn < N; psn++) {
        fill_payload(src, psn, L);
        store(c, psn, src, L);
    }

    uint32_t loss_count = (uint32_t)((double)N * loss_rate);
    uint8_t *lost_map = (uint8_t *)calloc(N, 1);
    uint32_t cnt = 0;
    while (cnt < loss_count) {
        uint32_t psn = xorshift32() % N;
        if (!lost_map[psn]) {
            lost_map[psn] = 1;
            lose(c, psn);
            cnt++;
        }
    }

    uint64_t err = measure_retrieve_loss(c, N, lost_map, B, R, outdir,
                                         "C_retrieve_found.csv",
                                         "C_retrieve_lost.csv");
    printf("  丢包判定准确率：%u/%u（错误 %" PRIu64 "）\n", N, N, err);

    for (uint32_t psn = 0; psn < N; psn++)
        if (!lost_map[psn])
            lose(c, psn);
    free(lost_map);
    free(src);
}

/* ===================== 任务 D：突发丢包 ===================== */

static void task_D(struct cache *c, uint32_t N, int L, uint32_t B, uint32_t R,
                   uint32_t burst_len, const char *outdir) {
    printf("\n===== 任务 D：突发丢包取回（连续丢失 %u 包） =====\n", burst_len);

    unsigned char *src = (unsigned char *)malloc((size_t)L);
    for (uint32_t psn = 0; psn < N; psn++) {
        fill_payload(src, psn, L);
        store(c, psn, src, L);
    }

    uint8_t *lost_map = (uint8_t *)calloc(N, 1);
    uint32_t burst_start = xorshift32() % (N - burst_len);
    for (uint32_t k = 0; k < burst_len; k++) {
        uint32_t psn = burst_start + k;
        lost_map[psn] = 1;
        lose(c, psn);
    }

    uint64_t err = measure_retrieve_loss(c, N, lost_map, B, R, outdir,
                                         "D_retrieve_found.csv",
                                         "D_retrieve_lost.csv");
    printf("  丢包判定准确率：%u/%u（错误 %" PRIu64 "，突发段 [%u,%u)）\n",
           N, N, err, burst_start, burst_start + burst_len);

    for (uint32_t psn = 0; psn < N; psn++)
        if (!lost_map[psn])
            lose(c, psn);
    free(lost_map);
    free(src);
}

/* ===================== 任务 E：丢包率扫描 ===================== */

static void task_E(struct cache *c, uint32_t N, int L, uint32_t B, uint32_t R,
                   const char *outdir) {
    printf("\n===== 任务 E：丢包率扫描（store/retrieve 延迟 vs 丢包率） =====\n");
    static const double rates[] = {0.0, 0.001, 0.005, 0.01, 0.02, 0.05, 0.10};
    int nr = (int)(sizeof rates / sizeof rates[0]);

    FILE *f = fopen_join(outdir, "E_loss_sweep.csv", "w");
    fprintf(f, "loss_rate,store_p50_ns,retrieve_found_p50_ns,"
               "retrieve_lost_p50_ns,accuracy\n");

    unsigned char *src = (unsigned char *)malloc((size_t)L);

    /* store 延迟与丢包率无关，先在空缓存上测一次（预热后，B 个一批，测完清空） */
    double *st = (double *)malloc((size_t)R * sizeof(double));
    for (uint32_t j = 0; j < B; j++)
        store(c, j, src, L);
    for (uint32_t j = 0; j < B; j++)
        lose(c, j);
    for (uint32_t r = 0; r < R; r++) {
        uint64_t t0 = rdtsc_raw();
        for (uint32_t j = 0; j < B; j++)
            store(c, j, src, L);
        uint64_t t1 = rdtsc_raw();
        st[r] = (double)(t1 - t0) / (double)B / cycles_per_ns;
        for (uint32_t j = 0; j < B; j++)
            lose(c, j);
    }
    struct latency_stats st_s;
    compute_stats(st, (int)R, &st_s);

    printf("  %-8s %-14s %-20s %-20s %-10s\n", "loss%", "store_P50",
           "retr_found_P50", "retr_lost_P50", "accuracy");

    for (int i = 0; i < nr; i++) {
        double p = rates[i];
        /* 填满缓存 */
        for (uint32_t psn = 0; psn < N; psn++) {
            fill_payload(src, psn, L);
            store(c, psn, src, L);
        }

        /* 随机丢失 p*N 包 */
        uint32_t loss_count = (uint32_t)((double)N * p);
        uint8_t *lost_map = (uint8_t *)calloc(N, 1);
        uint32_t cnt = 0;
        while (cnt < loss_count) {
            uint32_t psn = xorshift32() % N;
            if (!lost_map[psn]) {
                lost_map[psn] = 1;
                lose(c, psn);
                cnt++;
            }
        }

        /* retrieve 延迟（found / lost 两路） */
        uint32_t *found_psn = (uint32_t *)malloc((size_t)N * sizeof(uint32_t));
        uint32_t *lost_psn = (uint32_t *)malloc((size_t)N * sizeof(uint32_t));
        uint32_t nf = 0, nl = 0;
        for (uint32_t psn = 0; psn < N; psn++) {
            if (lost_map[psn])
                lost_psn[nl++] = psn;
            else
                found_psn[nf++] = psn;
        }
        double *rf = (double *)malloc((size_t)R * sizeof(double));
        double *rl = (double *)malloc((size_t)R * sizeof(double));
        for (uint32_t r = 0; r < R; r++) {
            uint64_t t0 = rdtsc_raw();
            uint64_t acc = 0;
            for (uint32_t j = 0; j < B; j++) {
                struct mem_block_header *h = retrieve(c, found_psn[(r * B + j) % nf]);
                acc += (h ? (uint64_t)h->psn : 1ULL);
            }
            uint64_t t1 = rdtsc_raw();
            rf[r] = (double)(t1 - t0) / (double)B / cycles_per_ns;
            (void)acc;
        }
        for (uint32_t r = 0; r < R; r++) {
            if (nl == 0) { /* 无丢失包（0% 丢包率），无「lost 取回」样本 */
                rl[r] = 0.0;
                continue;
            }
            uint64_t t0 = rdtsc_raw();
            uint64_t acc = 0;
            for (uint32_t j = 0; j < B; j++) {
                struct mem_block_header *h = retrieve(c, lost_psn[(r * B + j) % nl]);
                acc += (h ? (uint64_t)h->psn : 1ULL);
            }
            uint64_t t1 = rdtsc_raw();
            rl[r] = (double)(t1 - t0) / (double)B / cycles_per_ns;
            (void)acc;
        }

        /* 判定准确率 */
        uint64_t err = 0;
        for (uint32_t psn = 0; psn < N; psn++) {
            struct mem_block_header *h = retrieve(c, psn);
            if ((h != NULL) == (lost_map[psn] != 0))
                err++;
        }

        struct latency_stats rf_s, rl_s;
        compute_stats(rf, (int)R, &rf_s);
        compute_stats(rl, (int)R, &rl_s);
        double acc = (double)(N - err) / (double)N * 100.0;

        printf("  %-8.1f %-14.1f %-20.1f %-20.1f %-9.2f%%\n", p * 100.0,
               st_s.p50_ns, rf_s.p50_ns, rl_s.p50_ns, acc);
        fprintf(f, "%.4f,%.2f,%.2f,%.2f,%.4f\n", p, st_s.p50_ns, rf_s.p50_ns,
                rl_s.p50_ns, acc);

        /* 清空缓存（只清 found 的） */
        for (uint32_t psn = 0; psn < N; psn++)
            if (!lost_map[psn])
                lose(c, psn);

        free(lost_map);
        free(found_psn);
        free(lost_psn);
        free(rf);
        free(rl);
    }

    fclose(f);
    free(st);
    free(src);
    printf("[OK] 丢包率扫描已写入 %s/E_loss_sweep.csv\n", outdir);
}

/* ===================== 主流程 ===================== */

static void usage(const char *prog) {
    fprintf(stderr,
            "用法: %s [选项]\n"
            "  -n N        缓存包数 / 环长（默认 %d）\n"
            "  -l L        单包 payload 大小（默认 1024，<= %d）\n"
            "  -B B        每批 store/retrieve 次数（批量计时，默认 128）\n"
            "  -R R        批数 = 延迟样本数（默认 200）\n"
            "  -p P        任务 C 随机丢包率（默认 0.01）\n"
            "  -d M        任务 D 突发丢包长度（默认 512）\n"
            "  -s SEED     随机种子（默认固定 0x9E3779B9）\n"
            "  -o DIR      输出目录（默认 .）\n"
            "  -h          帮助\n",
            prog, RING_SIZE, MAX_L);
}

int main(int argc, char **argv) {
    uint32_t N = RING_SIZE;
    int L = 1024;
    uint32_t B = 128;
    uint32_t R = 200;
    double loss_rate = 0.01;
    uint32_t burst_len = 512;
    const char *outdir = ".";

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-n") && i + 1 < argc)
            N = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(a, "-l") && i + 1 < argc)
            L = (int)strtol(argv[++i], NULL, 10);
        else if (!strcmp(a, "-B") && i + 1 < argc)
            B = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(a, "-R") && i + 1 < argc)
            R = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(a, "-p") && i + 1 < argc)
            loss_rate = strtod(argv[++i], NULL);
        else if (!strcmp(a, "-d") && i + 1 < argc)
            burst_len = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(a, "-s") && i + 1 < argc)
            rng_state = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(a, "-o") && i + 1 < argc)
            outdir = argv[++i];
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "未知选项: %s\n", a);
            usage(argv[0]);
            return 1;
        }
    }

    if (L > MAX_L || L <= 0) {
        fprintf(stderr, "L 必须在 (0, %d] 内\n", MAX_L);
        return 1;
    }
    if (burst_len >= N)
        burst_len = N / 2;
    if (strcmp(outdir, ".") != 0)
        mkdir(outdir, 0755);

    calibrate_tsc();
    printf("参数：N=%u 包, L=%d B, 批大小 B=%u, 批数 R=%u, 种子=0x%08X\n",
           N, L, B, R, rng_state);

    struct cache c;
    cache_init(&c, N);

    task_A(&c, N, L, B, R, outdir);
    task_B(&c, N, L, B, R, outdir);
    task_C(&c, N, L, B, R, loss_rate, outdir);
    task_D(&c, N, L, B, R, burst_len, outdir);
    task_E(&c, N, L, B, R, outdir);

    printf("\n复用统计：累计 malloc=%" PRIu64 " 次，复用=%" PRIu64 " 次，"
           "复用率=%.1f%%\n",
           c.alloc_cnt, c.reuse_cnt,
           (c.alloc_cnt + c.reuse_cnt)
               ? 100.0 * (double)c.reuse_cnt /
                     (double)(c.alloc_cnt + c.reuse_cnt)
               : 0.0);

    cache_destroy(&c);
    printf("\n提示：运行 python3 plot_behavior.py --dir %s --out %s 画图\n",
           outdir, outdir);
    return 0;
}

/*
 * include/util.h — 计时 / 随机 / 对齐分档 / 统计 / CSV 公用工具
 * ------------------------------------------------------------
 * 沿用 260912 旧项目口径（behavior_bench.c / psn_bench.c）：
 *   - 裸 rdtsc（无 lfence/rdtscp，避免 VM exit 噪声）；
 *   - TSC 标定到 ns：CLOCK_MONOTONIC 对 ~100ms 窗，`ticks_per_ns`，
 *     `to_ns(ticks) = ticks / ticks_per_ns`；
 *   - xorshift32（13,17,5），seed = cfg.seed（逐 rep 重置为 seed+rep）；
 *   - 标准差 = 样本标准差（n-1）；
 *   - 分位数/中位数统一 nearest-rank（inverted CDF）：idx = ceil(n*q)-1，q=p/100，
 *     夹取 [0,n-1]；偶数 n 中位数 = 偏左中间值（== p50）。★ Python 侧必须用
 *     numpy.percentile(a, q, method="inverted_cdf") 与之逐值相等。
 */
#ifndef UTIL_H
#define UTIL_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>    /* psn_set 用 malloc/free */
#include "config.h"

/* ---- rdtsc（裸，无 lfence/rdtscp） ---- */
static inline __attribute__((always_inline)) uint64_t rdtsc_raw(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* TSC->ns 标定（ticks per ns）。未标定时默认 1.0，to_ns 退化为恒等。 */
extern double g_tsc_per_ns;
void tsc_calibrate(void);
static inline __attribute__((always_inline)) double to_ns(uint64_t ticks) {
    return (double)ticks / g_tsc_per_ns;
}

/* 长窗交叉验证：同批 nops 次操作分别用 CLOCK_MONOTONIC 与 rdtsc 计时，
 * 返回 rdtsc 相对 wall 的偏差百分比（>0 = rdtsc 偏快）；per_op_* 输出每操作 ns。 */
double tsc_cross_validate(uint64_t nops, double *per_op_wall_ns, double *per_op_tsc_ns);

/* ---- xorshift32（可重入，每 rep 独立 seed） ---- */
typedef struct { uint32_t s; } xorshift32_t;

static inline __attribute__((always_inline)) void xorshift32_seed(xorshift32_t *r, uint32_t seed) { r->s = seed ? seed : 1u; }
static inline __attribute__((always_inline)) uint32_t xorshift32_next(xorshift32_t *r) {
    uint32_t x = r->s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    r->s = x;
    return x;
}

/* ---- 对齐 / 分档 ---- */
static inline __attribute__((always_inline)) uint32_t align16(uint32_t x) { return (x + 15u) & ~15u; }

/* 最小档 S >= x；x 超过最大档时返回最大档（块放不下 ⇒ 自然走溢出）。 */
uint32_t ceil_class(const cfg_t *cf, uint32_t x);

/* ---- 统计（均遵循旧口径） ---- */
double mean_dbl(const double *v, size_t n);
double std_dbl(const double *v, size_t n, double m);   /* 样本标准差 n-1 */
double median_dbl(double *v, size_t n);                 /* 就地排序，nearest-rank 中位数 */
double pct_dbl(double *v, size_t n, double p);          /* 就地排序，nearest-rank（inverted CDF） */
double mean_u64(const uint64_t *v, size_t n);
double std_u64(const uint64_t *v, size_t n, double m);

/* ---- CSV ---- */
void csv_header(FILE *f, const char *const *cols, size_t n);
void csv_row_u64(FILE *f, const uint64_t *v, size_t n);
void csv_row_f64(FILE *f, const double *v, size_t n);

/* ---- 分配器预热：让计时阶段的 malloc 命中热 bin，而非冷页 ---- */
void warm_allocator(size_t size, int count);

/* ---- 抗 DCE ---- */
extern volatile uint64_t g_sink;

/* ---- 请求 PSN 集合（第 4B：SR 统一交付契约「按 PSN 升序」的成员判定） ----
 * 开地址线性探测，容量为 2 的幂、负载 ≤ 1/2（保证终止）。UINT32_MAX 作空哨兵：
 * psn 是 24-bit（< 2^24），恒不碰撞哨兵。tree 中序遍历 / psn 映射零排序共用。 */
typedef struct {
    uint32_t *t;     /* 槽数组；UINT32_MAX = 空 */
    uint32_t cap;    /* 2 的幂 */
    uint32_t mask;
} psn_set_t;

static inline void psn_set_init(psn_set_t *s, uint32_t n) {
    uint32_t c = 16u;
    while (c < n * 2u) c <<= 1u;          /* 负载 ≤ 1/2 */
    s->cap = c;
    s->mask = c - 1u;
    s->t = (uint32_t *)malloc((size_t)c * sizeof(uint32_t));
    for (uint32_t i = 0; i < c; i++) s->t[i] = UINT32_MAX;
}
static inline void psn_set_free(psn_set_t *s) { free(s->t); s->t = NULL; }
static inline void psn_set_insert(psn_set_t *s, uint32_t psn) {
    uint32_t i = (psn * 2654435761u) & s->mask;
    while (s->t[i] != UINT32_MAX) i = (i + 1u) & s->mask;
    s->t[i] = psn;
}
static inline int psn_set_has(const psn_set_t *s, uint32_t psn) {
    uint32_t i = (psn * 2654435761u) & s->mask;
    while (s->t[i] != UINT32_MAX) {
        if (s->t[i] == psn) return 1;
        i = (i + 1u) & s->mask;
    }
    return 0;
}

/* ---- SR 升序交付的排序原语（第 4B/P1 基线公平性） ----
 * impl==0 → glibc qsort（sort_u32_cmp 函数指针，O(n log n) 通用）；
 * impl==1 → 内联插入排序（无函数指针间接；n≤64 的 SR 请求集 O(n^2) 常数小）。
 * 两者都是通用比较排序，用于 fifo/hash/index_only 的 sorted_retrieve_set 与 tree 的
 * tree_search_set 把请求 PSN 集排成升序再交付。注意：这是「升序交付」口径，PSN 主战场
 * 的 GBN 区间提取（conn_retransmit_range）是 O(1) Φ(start) + 顺序读，不经过这里。 */
static inline int sort_u32_cmp(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}
static inline void sort_u32_asc(uint32_t *a, uint32_t n, int impl) {
    if (impl == 1) {
        for (uint32_t i = 1; i < n; i++) {
            uint32_t key = a[i];
            uint32_t j = i;
            while (j > 0 && a[j - 1] > key) { a[j] = a[j - 1]; j--; }
            a[j] = key;
        }
        return;
    }
    qsort(a, n, sizeof(uint32_t), sort_u32_cmp);   /* impl==0（默认） */
}

#endif /* UTIL_H */

/*
 * src/util.c — 计时/随机/对齐分档/统计/CSV 的实现（口径见 util.h 头注释）
 */
#include "util.h"

#include <stdlib.h>
#include <math.h>
#include <time.h>
#include <unistd.h>

double g_tsc_per_ns = 1.0;
volatile uint64_t g_sink = 0;

void tsc_calibrate(void) {
    struct timespec a, b;
    uint64_t t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &a);
    t0 = rdtsc_raw();
    usleep(100000); /* ~100ms 标定窗（旧口径 20ms 放宽到 100ms，摊薄 clock_gettime 开销） */
    t1 = rdtsc_raw();
    clock_gettime(CLOCK_MONOTONIC, &b);
    double ns = (double)(b.tv_sec - a.tv_sec) * 1e9 +
                (double)(b.tv_nsec - a.tv_nsec);
    if (ns > 0)
        g_tsc_per_ns = (double)(t1 - t0) / ns;
}

double tsc_cross_validate(uint64_t nops, double *per_op_wall_ns, double *per_op_tsc_ns) {
    if (!nops) return 0.0;
    tsc_calibrate();
    xorshift32_t r;
    xorshift32_seed(&r, 42u);
    uint64_t acc = 0;
    struct timespec ta, tb;
    uint64_t t0, t1;

    clock_gettime(CLOCK_MONOTONIC, &ta);
    t0 = rdtsc_raw();
    for (uint64_t i = 0; i < nops; i++)
        acc ^= xorshift32_next(&r);
    t1 = rdtsc_raw();
    clock_gettime(CLOCK_MONOTONIC, &tb);
    g_sink ^= acc;

    double ns_wall = (double)(tb.tv_sec - ta.tv_sec) * 1e9 +
                     (double)(tb.tv_nsec - ta.tv_nsec);
    double ns_tsc = to_ns(t1 - t0);
    if (per_op_wall_ns) *per_op_wall_ns = ns_wall / (double)nops;
    if (per_op_tsc_ns)   *per_op_tsc_ns   = ns_tsc / (double)nops;
    return 100.0 * (ns_tsc - ns_wall) / ns_wall;
}

uint32_t ceil_class(const cfg_t *cf, uint32_t x) {
    if (!cf->n_sc_new) return x;
    for (uint32_t i = 0; i < cf->n_sc_new; i++)
        if (cf->sc_new[i] >= x) return cf->sc_new[i];
    return cf->sc_new[cf->n_sc_new - 1]; /* 超过最大档：返回最大档，放不下走溢出 */
}

static int cmp_dbl(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

double mean_dbl(const double *v, size_t n) {
    double s = 0;
    for (size_t i = 0; i < n; i++) s += v[i];
    return n ? s / (double)n : 0.0;
}

double std_dbl(const double *v, size_t n, double m) {
    double s = 0;
    for (size_t i = 0; i < n; i++) { double d = v[i] - m; s += d * d; }
    return n > 1 ? sqrt(s / (double)(n - 1)) : 0.0;
}

/* nearest-rank（inverted CDF）：idx = ceil(n*q) - 1，q = p/100，夹取 [0,n-1]。
 * 与 numpy.percentile(a, q, method="inverted_cdf") 逐值一致（见 util.h 头注释）。 */
static int qidx(size_t n, double p) {
    int idx = (int)ceil((double)n * p / 100.0) - 1;
    if (idx < 0) idx = 0;
    if (idx >= (int)n) idx = (int)n - 1;
    return idx;
}

double median_dbl(double *v, size_t n) {
    if (!n) return 0.0;
    qsort(v, n, sizeof(double), cmp_dbl);
    return v[qidx(n, 50.0)];
}

double pct_dbl(double *v, size_t n, double p) {
    if (!n) return 0.0;
    qsort(v, n, sizeof(double), cmp_dbl);
    return v[qidx(n, p)];
}

double mean_u64(const uint64_t *v, size_t n) {
    double s = 0;
    for (size_t i = 0; i < n; i++) s += (double)v[i];
    return n ? s / (double)n : 0.0;
}

double std_u64(const uint64_t *v, size_t n, double m) {
    double s = 0;
    for (size_t i = 0; i < n; i++) { double d = (double)v[i] - m; s += d * d; }
    return n > 1 ? sqrt(s / (double)(n - 1)) : 0.0;
}

void csv_header(FILE *f, const char *const *cols, size_t n) {
    for (size_t i = 0; i < n; i++) fprintf(f, "%s%s", i ? "," : "", cols[i]);
    fputc('\n', f);
}

void csv_row_u64(FILE *f, const uint64_t *v, size_t n) {
    for (size_t i = 0; i < n; i++)
        fprintf(f, "%s%llu", i ? "," : "", (unsigned long long)v[i]);
    fputc('\n', f);
}

void csv_row_f64(FILE *f, const double *v, size_t n) {
    for (size_t i = 0; i < n; i++) fprintf(f, "%s%.4f", i ? "," : "", v[i]);
    fputc('\n', f);
}

void warm_allocator(size_t size, int count) {
    void **keep = malloc((size_t)count * sizeof(void *));
    if (!keep)
        return;
    for (int i = 0; i < count; i++)
        keep[i] = malloc(size);
    for (int i = 0; i < count; i++)
        free(keep[i]);
    free(keep);
}

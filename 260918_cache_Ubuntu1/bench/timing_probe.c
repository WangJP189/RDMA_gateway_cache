/*
 * bench/timing_probe.c — 第 1 步「实机计时规程」一次性诊断探针（非实验，不进 config.h）
 * ----------------------------------------------------------------------------------
 * 产出 docs/TIMING_PROTOCOL.md 需要的原始证据：
 *   (1) 原语对比：rdtsc_raw() vs clock_gettime(CLOCK_MONOTONIC) 单次成本（各 10^7 次，去首尾）。
 *   (2) 空批地板：与计时完全相同的循环结构（B 次 asm 空屏障夹一对 rdtsc），per-op floor。
 *   (3) 频率漂移验证：constant_tsc/nonstop_tsc 已由 /proc/cpuinfo 确认；本探针在「空转」与
 *       「内存密集（先跑 10 s memcpy 大块）」两种负载下各标定一次 ns/tick，报差异百分比。
 *
 * 与项目计时口径严格一致：
 *   - rdtsc_raw() = 裸 rdtsc（无 lfence/rdtscp），与 include/util.h 逐字相同；
 *   - 主时间基准 CLOCK_MONOTONIC；TSC 经 tsc_per_ns 标定到 ns。
 *
 * 编译 / 运行（绑定到 P-core CPU 0；governor 无法改 performance —— 无 root，见 TIMING_PROTOCOL）：
 *   gcc -O2 -Wall -Wextra -std=gnu11 bench/timing_probe.c -o build/timing_probe -lm
 *   taskset -c 0 ./build/timing_probe
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static inline __attribute__((always_inline)) uint64_t rdtsc_raw(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

volatile uint64_t g_sink = 0;

static double wall_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

/* TSC -> ns 标定：CLOCK_MONOTONIC 对 ~100ms usleep 窗。返回 tsc_per_ns。 */
static double calibrate_tsc_per_ns(void) {
    struct timespec a, b;
    uint64_t t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &a);
    t0 = rdtsc_raw();
    usleep(100000);
    t1 = rdtsc_raw();
    clock_gettime(CLOCK_MONOTONIC, &b);
    double ns = (double)(b.tv_sec - a.tv_sec) * 1e9 + (double)(b.tv_nsec - a.tv_nsec);
    return ns > 0 ? (double)(t1 - t0) / ns : 0.0;
}

static int cmp_dbl(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

/* 单原语单次成本：N_SUB 批 × M 次，per-call 样本排序，去首尾 trim 后报 median/min/max。
 * 每次调用都累加进 g_sink 抗 DCE。返回填好的样本数。 */
static size_t measure_primitive(int is_rdtsc, double tsc_per_ns, uint32_t M,
                                uint32_t N_SUB, double *out) {
    size_t n = 0;
    uint64_t acc = 0;
    struct timespec ts;
    for (uint32_t s = 0; s < N_SUB; s++) {
        uint64_t t0 = rdtsc_raw();
        if (is_rdtsc) {
            for (uint32_t k = 0; k < M; k++) acc += rdtsc_raw();
        } else {
            for (uint32_t k = 0; k < M; k++) { clock_gettime(CLOCK_MONOTONIC, &ts); acc += (uint64_t)ts.tv_nsec; }
        }
        uint64_t t1 = rdtsc_raw();
        out[n++] = (double)(t1 - t0) / tsc_per_ns / (double)M;   /* per-call ns */
    }
    g_sink ^= acc;
    return n;
}

/* 空批地板：B 次空屏障夹一对 rdtsc，per-op ns。返回样本数。 */
static size_t measure_floor(double tsc_per_ns, uint32_t B, uint32_t N_SUB, double *out) {
    size_t n = 0;
    for (uint32_t s = 0; s < N_SUB; s++) {
        uint64_t t0 = rdtsc_raw();
        for (uint32_t k = 0; k < B; k++) { asm volatile("" ::: "memory"); }
        uint64_t t1 = rdtsc_raw();
        out[n++] = (double)(t1 - t0) / tsc_per_ns / (double)B;
    }
    return n;
}

static void trim_report(const char *label, double *v, size_t n) {
    qsort(v, n, sizeof(double), cmp_dbl);
    size_t lo = n / 10, hi = n - n / 10;   /* 去首尾各 10% */
    double mn = 1e18, mx = 0, sum = 0;
    for (size_t i = lo; i < hi; i++) {
        if (v[i] < mn) mn = v[i];
        if (v[i] > mx) mx = v[i];
        sum += v[i];
    }
    printf("%-34s median=%8.3f ns   min=%8.3f   max=%8.3f   mean=%8.3f\n",
           label, v[(lo + hi) / 2], mn, mx, sum / (double)(hi - lo));
}

int main(void) {
    printf("== timing_probe: 实机计时规程原始证据 ==\n");
    printf("constant_tsc/nonstop_tsc/rdtscp 已由 /proc/cpuinfo 确认（见 ENV_CHECK.md）\n\n");

    double tsc_per_ns = calibrate_tsc_per_ns();
    printf("[calib] tsc_per_ns (idle)        = %.6f ticks/ns  => %.2f MHz\n",
           tsc_per_ns, tsc_per_ns * 1000.0);

    /* ---- (1) 原语对比：各 10^7 次，去首尾 ---- */
    printf("\n== (1) 原语单次成本（各 10^7 次，去首尾各 10%%） ==\n");
    {
        double *v = (double *)malloc(10000 * sizeof(double));
        size_t n = measure_primitive(1, tsc_per_ns, 1000, 10000, v);   /* 10^7 rdtsc */
        trim_report("rdtsc_raw()", v, n);
        n = measure_primitive(0, tsc_per_ns, 1000, 10000, v);           /* 10^7 clock_gettime */
        trim_report("clock_gettime(MONOTONIC)", v, n);
        free(v);
    }

    /* ---- (2) 空批地板（同循环结构） ---- */
    printf("\n== (2) 空批地板 per-op（同循环结构：B 次空屏障夹一对 rdtsc） ==\n");
    {
        static const uint32_t B_LIST[] = {1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192};
        double *v = (double *)malloc(100000 * sizeof(double));   /* 小 B 用 10^5 批，缓冲须够大 */
        for (uint32_t i = 0; i < sizeof(B_LIST) / sizeof(B_LIST[0]); i++) {
            uint32_t B = B_LIST[i];
            uint32_t N_SUB = (B >= 32) ? 10000 : 100000;   /* 小 B 多采一批，稳定 median */
            size_t n = measure_floor(tsc_per_ns, B, N_SUB, v);
            qsort(v, n, sizeof(double), cmp_dbl);
            char lbl[64];
            snprintf(lbl, sizeof(lbl), "floor B=%-5u", B);
            printf("%-34s median=%9.3f ns/op\n", lbl, v[n / 2]);
        }
        free(v);
    }

    /* ---- (3) 频率漂移验证：内存密集负载后再标定 ---- */
    printf("\n== (3) 频率漂移验证（内存密集 10 s memcpy 后重标定） ==\n");
    {
        const size_t GB = 1u << 30;
        size_t buf_sz = 4u * GB;                 /* 4 GB，远超 L3 33 MB */
        uint8_t *buf = (uint8_t *)malloc(buf_sz);
        if (!buf) { printf("OOM for 4GB memcpy buffer\n"); return 1; }
        memset(buf, 0x5a, buf_sz / 2);           /* 触页（2GB 范围往返） */
        double t_start = wall_ns();
        size_t iters = 0;
        while (wall_ns() - t_start < 10e9) {     /* 10 s 内存密集往返 */
            memcpy(buf + (buf_sz / 2), buf, buf_sz / 2);
            memcpy(buf, buf + (buf_sz / 2), buf_sz / 2);
            iters++;
        }
        printf("  memcpy 10 s: iters=%zu, 吞吐=%.2f GB/s\n",
               iters, (double)iters * buf_sz / 10.0 / 1e9);
        double tsc_mem = calibrate_tsc_per_ns();
        printf("[calib] tsc_per_ns (memory-load)= %.6f ticks/ns => %.2f MHz\n",
               tsc_mem, tsc_mem * 1000.0);
        double drift = 100.0 * (tsc_mem - tsc_per_ns) / tsc_per_ns;
        printf("[drift] idle vs memory-load 差异 = %+.3f%%  %s\n", drift,
               (drift > 1.0 || drift < -1.0) ? ">>> 超过 1%% 阈值，时间一律改用 CLOCK_MONOTONIC <<<"
                                             : "(<=1%%，TSC 可用作主原语)");
        free(buf);
    }

    printf("\ndone.\n");
    return 0;
}

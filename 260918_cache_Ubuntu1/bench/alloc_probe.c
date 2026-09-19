/*
 * bench/alloc_probe.c — 分配器噪声地板（第 5 条 5.5）
 * ------------------------------------------------------------------
 * 裸 malloc(n)+free(n) 的每操作耗时（batch 摊销，p50 为主指标），n ∈ {32,56,64,4128}：
 *   32   = fifo/hash 节点头（sizeof 实测 32 B，无 payload）
 *   56   = avl 节点头（sizeof 实测 56 B，无 payload）
 *   64   = align16(56)（avl 对齐后的最小槽，也等于 32+32）
 *   4128 = 32 + 4096（fifo/hash 节点 + 最大 MTU payload，perstore 最大 malloc 尺寸）
 * 目的：把「perstore 逐 store malloc/free 的纯分配器成本」变成显式已知量，供
 *   alloc_sensitivity.md 与 exp1a perstore 的 store 耗时对照（分配器成本 vs 索引/拷贝成本）。
 *
 * 口径与 exp1a 一致：B 次 malloc+free 夹一对裸 rdtsc，per-op=(t1-t0)/B，REPS=5 池化样本。
 * malloc/free 是 libc 副作用调用，不会被 DCE；循环里反复同尺寸 malloc 命中 tcache，
 * 与 perstore store 循环（同样反复同尺寸 malloc）处于同一分配器热态，具代表性。
 */
#include "baseline.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>

/* 抗 DCE：复用 util.h 的 g_sink（volatile uint64_t），把 malloc 返回指针喂进去，
 * 阻止 GCC 消除「malloc 后立即 free」的死对。 */

static int mkdir_p(const char *path) {
    char buf[512];
    size_t n = strlen(path);
    if (n == 0 || n >= sizeof(buf)) return -1;
    memcpy(buf, path, n + 1);
    for (char *p = buf + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(buf, 0777) && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(buf, 0777) && errno != EEXIST) return -1;
    return 0;
}

int main(int argc, char **argv) {
    cfg_t cfg; cfg_default(&cfg);
    snprintf(cfg.out_dir, sizeof(cfg.out_dir), "out/alloc_probe");
    if (cfg_override_cli(&cfg, argc, argv) != 0) return 1;

    tsc_calibrate();
    if (mkdir_p(cfg.out_dir) < 0) { fprintf(stderr, "[alloc_probe] 无法创建输出目录 %s\n", cfg.out_dir); return 1; }

    char path[512];
    snprintf(path, sizeof(path), "%s/resolved_config.json", cfg.out_dir);
    cfg_dump_json(&cfg, path);

    snprintf(path, sizeof(path), "%s/alloc_probe.csv", cfg.out_dir);
    FILE *f = fopen(path, "w");
    if (!f) { fprintf(stderr, "[alloc_probe] 无法打开 %s\n", path); return 1; }
    fprintf(f, "# alloc_probe: bare malloc(n)+free(n) per-op ns, batch-amortized (B=2048, reps=5)\n");
    fprintf(f, "# n_bytes: 32=fifo/hash node, 56=avl node, 64=align16(56), 4128=32+4096(max perstore malloc)\n");
    fprintf(f, "n_bytes,B,n_batches,reps,mean_ns,std_ns,p50_ns,p90_ns,p99_ns\n");

    const uint32_t SIZES[] = { 32u, 56u, 64u, 4128u };
    const uint32_t N_SIZES = sizeof(SIZES) / sizeof(SIZES[0]);
    const uint32_t B = 2048u;                 /* 与 exp1a 主矩阵一致 */
    const uint32_t reps = cfg.reps;
    const uint32_t n_batches = 1000u;         /* 每 size 批数：×reps=5000 样本，p99 稳定 */

    double *samples = (double *)malloc((size_t)n_batches * reps * sizeof(double));
    for (uint32_t si = 0; si < N_SIZES; si++) {
        uint32_t n = SIZES[si];
        size_t idx = 0;
        for (uint32_t r = 0; r < reps; r++) {
            for (uint32_t b = 0; b < n_batches; b++) {
                uint64_t t0 = rdtsc_raw();
                for (uint32_t k = 0; k < B; k++) { void *p = malloc(n); g_sink ^= (uint64_t)(uintptr_t)p; free(p); }
                uint64_t t1 = rdtsc_raw();
                samples[idx++] = to_ns(t1 - t0) / (double)B;
            }
        }
        double mean = mean_dbl(samples, idx);
        double std  = std_dbl(samples, idx, mean);
        double p50  = pct_dbl(samples, idx, 50.0);
        double p90  = pct_dbl(samples, idx, 90.0);
        double p99  = pct_dbl(samples, idx, 99.0);
        fprintf(f, "%u,%u,%u,%u,%.3f,%.3f,%.3f,%.3f,%.3f\n",
                n, B, n_batches, reps, mean, std, p50, p90, p99);
        printf("alloc_probe n=%5u: mean=%8.3f  p50=%8.3f  p90=%8.3f  p99=%8.3f ns/op\n",
               n, mean, p50, p90, p99);
        fflush(f);
    }

    fclose(f);
    free(samples);
    printf("alloc_probe done -> %s/alloc_probe.csv\n", cfg.out_dir);
    return 0;
}

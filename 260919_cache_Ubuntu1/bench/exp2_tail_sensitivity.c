/*
 * bench/exp2_tail_sensitivity.c — 实验二(b)：尾包占比敏感性（tail sensitivity）
 * --------------------------------------------------------------------------------
 * 问题：弹性 S 是「单一全局块大小」，只能对齐到最大包（MTU）。当工作量在满 MTU 包之外
 *   混入占比 f 的小尾包时，弹性 S 被最大包钉在 MTU=4096，小尾包只浪费槽空间。
 *   本实验量化：利用率随尾包占比 f 如何退化，以及弹性机制是否会被尾包扰动（扩缩/溢出/丢弃）。
 *
 * 工作量（逐字来自 config CFG_E2_TAIL_FRAC_LIST 注释）：
 *   包尺寸 (1-f)@MTU + f@U[1,MTU]，f ∈ e2_tail_frac_list = { 0.0, 0.01, 0.03, 0.06, 0.10, 0.20, 0.30 }。
 *   MTU = e2_fixed_l = 4096（=最大 MTU）；N = e2_n = 4096（满窗，与 exp2_space 同口径）。
 *
 * 预期（并在此验证）：
 *   ① 弹性 S 收敛并钉在 final_S = 4096（被 (1-f) 的 MTU 包钉住，规则 B 不缩：L_ring=4096 恒 >= S）；
 *   ② 尾包 len<=S=4096 走环内正常路径，不触发溢出/扩缩 ⇒ n_ovf_ins = n_drop = n_resize = 0；
 *      即「单全局 S」在尺寸尾下是稳定的、优雅降级的（无抖动），代价只是利用率线性下降；
 *   ③ utilization = Σlen / allocated，随 f 从 99.6%（f=0）单调下降（f=0.30 时 ~85%）。
 *
 * 满窗语义：store psn=0..N-1 逐槽命中（Φ=psn%N，无碰撞、无淘汰），活集恒 N，
 *   allocated = cache_footprint_bytes（sizeof 实测），payload = Σ len(psn)。
 *
 * 编译：make build/exp2_tail_sensitivity && ./build/exp2_tail_sensitivity [--out=DIR]
 * 产物：<out>/tail_summary.csv + <out>/resolved_config.json
 *   列：f, final_S, payload_bytes, allocated_bytes, utilization, overhead_per_pkt, n_ovf_ins, n_drop, n_resize
 */
#include "baseline.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>

/* ---- 输出目录（与 exp2_space 同款递归 mkdir） ---- */
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
    snprintf(cfg.out_dir, sizeof(cfg.out_dir), "out/exp2_tail");
    if (cfg_override_cli(&cfg, argc, argv) != 0) return 1;

    if (mkdir_p(cfg.out_dir) < 0) { fprintf(stderr, "[exp2_tail] 无法创建输出目录 %s\n", cfg.out_dir); return 1; }

    char path[512];
    snprintf(path, sizeof(path), "%s/resolved_config.json", cfg.out_dir);
    cfg_dump_json(&cfg, path);

    const uint32_t N   = cfg.e2_n;
    const uint32_t MTU = cfg.e2_fixed_l;   /* =4096（最大 MTU） */
    const uint32_t n_f = cfg.e2_tail_frac_n;

    snprintf(path, sizeof(path), "%s/tail_summary.csv", cfg.out_dir);
    FILE *f = fopen(path, "w");
    if (!f) { fprintf(stderr, "[exp2_tail] 无法打开 %s\n", path); return 1; }
    fprintf(f, "# exp2_tail sensitivity: packet size (1-f)@MTU + f@U[1,MTU]. N=%u MTU=%u (full window)\n", N, MTU);
    fprintf(f, "# f scanned from e2_tail_frac_list (n=%u); xorshift32 seed = 0x12345678 + frac_index (deterministic)\n", n_f);
    fprintf(f, "# elastic S pinned at MTU by the (1-f) MTU packets -> no grow/shrink, no overflow, no drop\n");
    fprintf(f, "# utilization = payload_bytes / allocated_bytes; allocated = cache_footprint_bytes (sizeof measured)\n");
    fprintf(f, "# overhead_per_pkt (B) = (allocated_bytes - payload_bytes) / N\n");
    fprintf(f, "f,final_S,payload_bytes,allocated_bytes,utilization,overhead_per_pkt,n_ovf_ins,n_drop,n_resize\n");

    uint8_t payload[4096];
    memset(payload, 0x5a, sizeof(payload));

    for (uint32_t fi = 0; fi < n_f; fi++) {
        double fr = cfg.e2_tail_frac_list[fi];

        cfg.adaptive_enable = 1u;   /* 忠实于弹性机制：让 S 自行收敛（此处钉在 MTU，不缩不扩） */
        b_cache_t *c = make_dynblock(&cfg, MTU);   /* S0 = ceil_class(MTU) = 4096 */

        xorshift32_t rng;
        xorshift32_seed(&rng, 0x12345678u + fi);   /* 每档 f 独立、可复现 */

        uint64_t payload_bytes = 0;
        for (uint32_t k = 0; k < N; k++) {
            uint32_t len;
            double u = (double)xorshift32_next(&rng) / 4294967296.0;   /* [0,1) */
            if (u < fr) {
                len = 1u + (xorshift32_next(&rng) % MTU);              /* 尾包 U[1,MTU] */
            } else {
                len = MTU;                                             /* (1-f) 满 MTU */
            }
            c->ops.store(c, k, payload, len);
            payload_bytes += len;
        }

        uint32_t final_S  = dynblock_cur_S(c);
        uint64_t alloc    = cache_footprint_bytes(c);
        double   util     = alloc ? (double)payload_bytes / (double)alloc : 0.0;
        double   ovh      = (alloc >= payload_bytes) ? (double)(alloc - payload_bytes) / (double)N : 0.0;

        uint64_t n_ovf_ins = 0, n_drop = 0, n_resize = 0;
        dynblock_stats(c, &n_ovf_ins, &n_drop, &n_resize);

        fprintf(f, "%.2f,%u,%llu,%llu,%.6f,%.3f,%llu,%llu,%llu\n",
                fr, final_S,
                (unsigned long long)payload_bytes, (unsigned long long)alloc, util, ovh,
                (unsigned long long)n_ovf_ins, (unsigned long long)n_drop, (unsigned long long)n_resize);
        printf("  f=%.2f  S=%4u  payload=%10llu B  alloc=%10llu B  util=%6.2f%%  ovh=%.1f B  "
               "ovf_ins=%llu drop=%llu resize=%llu\n",
               fr, final_S,
               (unsigned long long)payload_bytes, (unsigned long long)alloc, util * 100.0, ovh,
               (unsigned long long)n_ovf_ins, (unsigned long long)n_drop, (unsigned long long)n_resize);
        fflush(f); fflush(stdout);

        c->ops.destroy(c);
    }

    fclose(f);
    printf("exp2_tail done -> %s/tail_summary.csv\n", cfg.out_dir);
    return 0;
}

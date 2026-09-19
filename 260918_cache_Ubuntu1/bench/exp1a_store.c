/*
 * bench/exp1a_store.c — 实验一(a)：存时间开销（store time cost）
 * --------------------------------------------------------------------------------
 * 精简矩阵（2026-09-16 范围收窄后）：方法 = fifo_bounded / chained_hash_bounded /
 *   balanced_tree_bounded / psn_dynblock / psn_dynblock_fixed / index_only。
 *   去掉三个无界版（每 store 一次 malloc 已由冒烟 1.0-vs-0 证明，不再计一遍时）。
 * payload ∈ {256, 512, 1024, 2048, 4096}（RDMA 5 档 MTU）；主矩阵 B=2048（实机一对 rdtsc=8.19ns ⇒ 摊 0.004ns，占最快方法 ~0.2%，见 TIMING_PROTOCOL.md）；
 *   B=1024 作交叉验证（不同 B 下排序/差距一致 ⇒ 地板论证闭环）；timed_ops=500000；REPS=5。
 *
 * dynblock 口径（「收敛后冻结」，忠实于自适应机制）：
 *   psn_dynblock        —— 预热期 adaptive_enable=1 让 S 按机制自行收敛（64B→128、1024B→1024、
 *                          4096B→4096 不缩），记录收敛后 S，再 adaptive_enable=0 冻结计时。
 *   psn_dynblock_fixed  —— 全程 adaptive_enable=0，S=4096 固定（「不做自适应」的对照）。
 *   其余方法不 resize；index_only 给 Φ 的不可约成本（映射+槽写，无 payload 拷贝）。
 *
 * 计时纪律（实机一对裸 rdtsc=8.19ns ⇒ 批量摊销，B 可远小于 VM 时代）：
 *   - 每批 B 次 store 夹在一对裸 rdtsc 之间，per-op = (t1-t0)/B；
 *   - REPS=5，样本跨 rep 池化后报 mean/std/p50/p90/p99；
 *   - 主指标 p50，次指标 p90；p99 仅 CSV 留痕（实机尾部干净，p99 备查）；
 *   - 空批地板：同循环结构 + 同一对 rdtsc，批内无 store（asm 屏障防 DCE），
 *     floor_median 写进 CSV，把读钟开销变成显式已知量（floor ≈ 读钟/B）。
 *
 * 编译：make build/exp1a_store && ./build/exp1a_store [--out=DIR]
 * 产物：<out>/store_summary.csv + <out>/resolved_config.json
 */
#include "baseline.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>

/* ---- 输出目录（与 sim_main.c 同款递归 mkdir） ---- */
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

/* ---- 方法表（工厂 + 收敛/块大小口径） ---- */
typedef b_cache_t *(*make_fn)(const cfg_t *cfg, uint32_t payload_len);

static b_cache_t *mk_fifo_b(const cfg_t *cfg, uint32_t pl) { (void)cfg; return make_fifo_bounded(cfg->e1a_n, pl); }
static b_cache_t *mk_hash_b(const cfg_t *cfg, uint32_t pl) { return make_chained_hash_bounded(cfg->e1a_n, cfg->hash_nbuckets, pl); }
static b_cache_t *mk_tree_b(const cfg_t *cfg, uint32_t pl) { (void)cfg; return make_balanced_tree_bounded(cfg->e1a_n, pl); }
static b_cache_t *mk_dyn   (const cfg_t *cfg, uint32_t pl) { (void)pl; return make_dynblock(cfg, 4096); } /* S0=ceil_class(4096)=4096 */
static b_cache_t *mk_idx   (const cfg_t *cfg, uint32_t pl) { (void)pl; return make_index_only(cfg->e1a_n); }
/* 第 5 条 perstore 工厂：同一索引结构，唯一变量=分配策略（make_dynblock 读 cfg.alloc_mode 自选） */
static b_cache_t *mk_fifo_p(const cfg_t *cfg, uint32_t pl) { (void)cfg; return make_fifo_perstore(cfg->e1a_n, pl); }
static b_cache_t *mk_hash_p(const cfg_t *cfg, uint32_t pl) { return make_chained_hash_perstore(cfg->e1a_n, cfg->hash_nbuckets, pl); }
static b_cache_t *mk_tree_p(const cfg_t *cfg, uint32_t pl) { (void)cfg; return make_balanced_tree_perstore(cfg->e1a_n, pl); }

typedef struct {
    const char *name;
    make_fn      make;
    int          converge;    /* 1 = 预热期 adaptive=1 收敛、计时期冻结为 0 */
    int          is_dynblock; /* 1 = 输出 block_S 列（收敛后/固定块大小） */
} method_t;

/* pooled（alloc_mode=0，主结果）：5 索引结构 + 固定块对照。index_only 仅 pooled（无 payload 块）。 */
static const method_t METHODS_POOLED[] = {
    { "fifo_bounded",          mk_fifo_b, 0, 0 },
    { "chained_hash_bounded",  mk_hash_b, 0, 0 },
    { "balanced_tree_bounded", mk_tree_b, 0, 0 },
    { "psn_dynblock",          mk_dyn,    1, 1 },   /* 收敛后冻结（主结果） */
    { "psn_dynblock_fixed",    mk_dyn,    0, 1 },   /* S=4096 固定（对照） */
    { "index_only",            mk_idx,    0, 0 },
};

/* perstore（alloc_mode=1）：同 4 个真实索引结构，逐 store malloc/free。无 index_only / 无 fixed 对照。 */
static const method_t METHODS_PERSTORE[] = {
    { "fifo_perstore",          mk_fifo_p, 0, 0 },
    { "chained_hash_perstore",  mk_hash_p, 0, 0 },
    { "balanced_tree_perstore", mk_tree_p, 0, 0 },
    { "psn_dynblock_perstore",  mk_dyn,    1, 1 },  /* 收敛后冻结（唯一变量=分配策略） */
};

/* ---- store 批量计时：每批 B 次 store 夹一对 rdtsc，per-op ns 写入 samples ---- */
static size_t time_store(b_cache_t *c, uint32_t pl, uint32_t B,
                         uint32_t n_batches, uint32_t reps, double *samples) {
    uint8_t payload[4096];
    memset(payload, 0x5a, sizeof(payload));   /* 内容不影响 memcpy 成本；固定暖源 */
    uint32_t psn = 0;
    size_t idx = 0;
    for (uint32_t r = 0; r < reps; r++) {
        for (uint32_t b = 0; b < n_batches; b++) {
            uint64_t t0 = rdtsc_raw();
            for (uint32_t k = 0; k < B; k++) c->ops.store(c, psn++, payload, pl);
            uint64_t t1 = rdtsc_raw();
            samples[idx++] = to_ns(t1 - t0) / (double)B;
        }
    }
    g_sink ^= (uint64_t)psn;                 /* 抗 DCE */
    return idx;
}

/* ---- 空批地板：同循环结构 + 同一对 rdtsc，批内无 store（屏障防循环塌缩/DCE） ---- */
static size_t time_floor(uint32_t B, uint32_t n_batches, uint32_t reps, double *samples) {
    size_t idx = 0;
    for (uint32_t r = 0; r < reps; r++) {
        for (uint32_t b = 0; b < n_batches; b++) {
            uint64_t t0 = rdtsc_raw();
            for (uint32_t k = 0; k < B; k++) { asm volatile("" ::: "memory"); }
            uint64_t t1 = rdtsc_raw();
            samples[idx++] = to_ns(t1 - t0) / (double)B;
        }
    }
    return idx;
}

int main(int argc, char **argv) {
    cfg_t cfg; cfg_default(&cfg);
    snprintf(cfg.out_dir, sizeof(cfg.out_dir), "out/exp1a_store");
    if (cfg_override_cli(&cfg, argc, argv) != 0) return 1;

    /* 第 5 条：perstore 默认落盘独立目录，避免覆盖 pooled 主结果。 */
    if (cfg.alloc_mode && strcmp(cfg.out_dir, "out/exp1a_store") == 0)
        snprintf(cfg.out_dir, sizeof(cfg.out_dir), "out/exp1a_store_perstore");
    const char  *alloc_mode_str = cfg.alloc_mode ? "perstore" : "pooled";
    const method_t *methods = cfg.alloc_mode ? METHODS_PERSTORE : METHODS_POOLED;
    uint32_t n_methods      = cfg.alloc_mode
                            ? (uint32_t)(sizeof(METHODS_PERSTORE) / sizeof(METHODS_PERSTORE[0]))
                            : (uint32_t)(sizeof(METHODS_POOLED) / sizeof(METHODS_POOLED[0]));

    tsc_calibrate();
    if (mkdir_p(cfg.out_dir) < 0) { fprintf(stderr, "[exp1a] 无法创建输出目录 %s\n", cfg.out_dir); return 1; }

    char path[512];
    snprintf(path, sizeof(path), "%s/resolved_config.json", cfg.out_dir);
    cfg_dump_json(&cfg, path);

    uint32_t N         = cfg.e1a_n;
    uint32_t reps      = cfg.reps;
    uint32_t warm      = (cfg.e1a_timed_ops > 2 * N) ? cfg.e1a_timed_ops : 2 * N;

    /* 两个 B：主矩阵 + 交叉验证（同源 config：e1a_batch_ops / e1a_batch_xval_ops） */
    const uint32_t B_LIST[] = { cfg.e1a_batch_ops, cfg.e1a_batch_xval_ops };
    const char    *B_TAG[]  = { "main", "xval" };
    const uint32_t N_B = sizeof(B_LIST) / sizeof(B_LIST[0]);

    snprintf(path, sizeof(path), "%s/store_summary.csv", cfg.out_dir);
    FILE *f = fopen(path, "w");
    if (!f) { fprintf(stderr, "[exp1a] 无法打开 %s\n", path); return 1; }
    fprintf(f, "# exp1a store time cost (batch-amortized). N=%u timed_ops=%u reps=%u warm=%u\n",
            N, cfg.e1a_timed_ops, reps, warm);
    fprintf(f, "# B: main=%u (n_batches=%u), xval=%u (n_batches=%u)\n",
            cfg.e1a_batch_ops, cfg.e1a_timed_ops / cfg.e1a_batch_ops,
            cfg.e1a_batch_xval_ops, cfg.e1a_timed_ops / cfg.e1a_batch_xval_ops);
    fprintf(f, "# dynblock: psn_dynblock=收敛后冻结(预热 adaptive=1, 记录收敛 S), psn_dynblock_fixed=S=4096 固定\n");
    fprintf(f, "# alloc_mode: pooled=一次性预分配复用(主结果) perstore=逐 store malloc/free(第 5 条正交)\n");
    fprintf(f, "# p50=主指标 p90=次指标; p99 仅留痕(实机尾部干净, p99 备查); block_S=0 表示非 dynblock\n");
    fprintf(f, "# n_malloc/n_free: pooled 恒 0; perstore=计时期逐 store malloc/free 实计数(约 reps*timed_ops)\n");
    fprintf(f, "method,alloc_mode,payload,N,B,n_batches,reps,mean_ns,std_ns,p50_ns,p90_ns,p99_ns,floor_ns,block_S,n_malloc,n_free,n_resize\n");

    double *samples = (double *)malloc((size_t)cfg.e1a_timed_ops * reps * sizeof(double));

    for (uint32_t bi = 0; bi < N_B; bi++) {
        uint32_t B         = B_LIST[bi];
        uint32_t n_batches = cfg.e1a_timed_ops / B;   /* 全批（不裁尾批） */
        uint32_t sample_n  = n_batches * reps;

        /* 空批地板（方法无关，每 B 测一次） */
        double *floor_s = (double *)malloc((size_t)sample_n * sizeof(double));
        size_t  floor_n = time_floor(B, n_batches, reps, floor_s);
        double  floor_med = median_dbl(floor_s, floor_n);   /* 就地排序 */
        free(floor_s);

        fprintf(f, "# --- B=%u (%s): floor median=%.3f ns/op, samples=%u ---\n",
                B, B_TAG[bi], floor_med, sample_n);
        printf("exp1a store: B=%u (%s) n_batches=%u reps=%u warm=%u  floor_median=%.3f ns/op\n",
               B, B_TAG[bi], n_batches, reps, warm, floor_med);

        for (uint32_t pi = 0; pi < cfg.e1a_payload_n; pi++) {
            uint32_t pl = cfg.e1a_payload_list[pi];
            for (uint32_t mi = 0; mi < n_methods; mi++) {
                const method_t *m = &methods[mi];

                /* dynblock 收敛口径：预热期开自适应，让 S 收敛；其余方法/固定块则全程关 */
                cfg.adaptive_enable = m->converge ? 1u : 0u;
                b_cache_t *c = m->make(&cfg, pl);

                uint8_t warm_buf[4096]; memset(warm_buf, 0x5a, sizeof(warm_buf));
                uint32_t psn = 0;
                for (uint32_t k = 0; k < warm; k++) c->ops.store(c, psn++, warm_buf, pl); /* 预热=池逐页 touch + 收敛 S */

                uint32_t block_S = m->is_dynblock ? dynblock_cur_S(c) : 0;
                if (m->converge) cfg.adaptive_enable = 0u;    /* 冻结：计时区间无 resize */

                c->n_malloc = 0; c->n_resize = 0; c->n_cmp = 0; c->n_free = 0;   /* 计时区间计数器归零 */
                size_t ns = time_store(c, pl, B, n_batches, reps, samples);

                if (cfg.alloc_mode == 0 && c->n_malloc != 0) {
                    /* pooled：环路径/池化 store 必须零 malloc（核心不变量 I3）；违反即实现错误 */
                    fprintf(stderr, "[exp1a] ERROR pooled %s pl=%u B=%u: n_malloc=%llu (pooled 必须为 0)\n",
                            m->name, pl, B, (unsigned long long)c->n_malloc);
                    return 1;
                }
                if (c->n_resize != 0) fprintf(stderr, "[exp1a] WARN %s pl=%u B=%u: n_resize=%llu (expect 0, adaptive frozen)\n",
                                              m->name, pl, B, (unsigned long long)c->n_resize);

                double mean = mean_dbl(samples, ns);
                double std  = std_dbl(samples, ns, mean);
                double p50  = pct_dbl(samples, ns, 50.0);
                double p90  = pct_dbl(samples, ns, 90.0);
                double p99  = pct_dbl(samples, ns, 99.0);

                fprintf(f, "%s,%s,%u,%u,%u,%u,%u,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%u,%llu,%llu,%llu\n",
                        m->name, alloc_mode_str, pl, N, B, n_batches, reps,
                        mean, std, p50, p90, p99, floor_med, block_S,
                        (unsigned long long)c->n_malloc, (unsigned long long)c->n_free,
                        (unsigned long long)c->n_resize);
                printf("  %-22s pl=%4u B=%4u  mean=%8.3f  p50=%8.3f  p90=%8.3f  S=%u  malloc=%llu free=%llu resize=%llu\n",
                       m->name, pl, B, mean, p50, p90, block_S,
                       (unsigned long long)c->n_malloc, (unsigned long long)c->n_free,
                       (unsigned long long)c->n_resize);
                fflush(f); fflush(stdout);
                c->ops.destroy(c);
            }
        }
    }

    fclose(f);
    free(samples);
    printf("exp1a_store done -> %s/store_summary.csv\n", cfg.out_dir);
    return 0;
}

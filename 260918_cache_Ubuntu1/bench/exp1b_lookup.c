/*
 * bench/exp1b_lookup.c — 实验一(b)：取时间开销（lookup time cost vs N）
 * --------------------------------------------------------------------------------
 * 精简矩阵（2026-09-17 修订）：
 *   方法 = fifo_bounded / chained_hash_bounded / balanced_tree_bounded /
 *          psn_dynblock(S0=payload) / psn_dynblock_adaptive(收敛后冻结) / index_only(对照 E)。
 *   模式（NAK 重传的真实工作负载，四种）= GBN-long(64) / GBN-short(8) / SR-16 / SR-64。
 *   N（缓存深度 / 工作集条数）= {128,256,512,1024,2048,4096} + 锚点 5120（非 2 的幂，见 RING_N_DERIVATION.md mask 披露）（7 点）；
 *   payload 只跑 1024。
 *
 * 环长口径（2026-09-18 实机重推导）：扫描点 N = 重传窗口 = 环长（对所有方法同义）。
 *   dynblock 环长 = 扫描点 N（Φ=psn%N 的 N 随扫描点变，锚点 5120 直接验证运行时取模无 2 的幂优势）。
 *
 * dynblock 口径（修正 2026-09-17：不再用 S=4096 存 1024B 包，那是对 PSN 的不公平配置）：
 *   psn_dynblock          —— S0=payload（make_dynblock(cfg, pl)），等价于 CM 上报 MTU 的正确初始化；
 *   psn_dynblock_adaptive —— S0=4096，预热期 adaptive_enable=1 让 S 按机制收敛到 1024，
 *                            再 adaptive_enable=0 冻结计时（忠实于自适应机制的「收敛后冻结」）。
 *   两者 S 最终都 = 1024，结果应一致（互证 S0=MTU 是正确初始化）。
 *
 * 重排序维度（2026-09-17 新增，回应「上层是否需要整理」）：
 *   - GBN（连续区间）：所有方法都按 start+k 递增取，输出天然 PSN 序，无需重排 ⇒ reorder 仅 0。
 *   - SR（离散集合）：所有方法都按 psns[k] 查询序取，输出未排序；若上层要按 PSN 序发，需排序。
 *     排序对所有方法对称（同一 64 元素升序插入排序，计时内），故 reorder ∈ {0,1} 两版都跑，
 *     用于对比「计入整理后」相对排序是否改变。
 *
 * 计时口径：
 *   - 计时单位 = 一次 retrieve_range / retrieve_set（定位 + memcpy 全过程，即一次 GBN/SR 重传
 *     的完整开销；SR reorder=1 时额外含「把 64 个 PSN 升序整理」这一步）；per-op ns = (t1-t0)/B。
 *   - 主指标 p50，次指标 p90；p99 仅 CSV 留痕（实机尾部干净，p99 备查）。
 *   - n_cmp：retrieve 前清零、批后累计，报「每次取包平均比较次数」；排序不在缓存内，不计 n_cmp。
 *   - 空批地板：floor ∝ 1/B，同时测 B=512 与 B=32（xval）。
 *
 * 编译：make build/exp1b_lookup && ./build/exp1b_lookup [--out=DIR]
 * 产物：<out>/lookup_summary.csv + <out>/n_cmp.csv + <out>/resolved_config.json
 */
#include "baseline.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>

/* ---- 输出目录（与 exp1a_store.c 同款递归 mkdir） ---- */
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

/* ---- 方法表（工厂；cap = 扫描点 N = 工作集条数） ---- */
typedef b_cache_t *(*make_fn)(const cfg_t *cfg, uint32_t cap, uint32_t pl);

static b_cache_t *mk_fifo_b(const cfg_t *cfg, uint32_t cap, uint32_t pl) { (void)cfg; return make_fifo_bounded(cap, pl); }
static b_cache_t *mk_hash_b(const cfg_t *cfg, uint32_t cap, uint32_t pl) { return make_chained_hash_bounded(cap, cfg->hash_nbuckets, pl); }
static b_cache_t *mk_tree_b(const cfg_t *cfg, uint32_t cap, uint32_t pl) { (void)cfg; return make_balanced_tree_bounded(cap, pl); }
static b_cache_t *mk_dyn_pl(const cfg_t *cfg, uint32_t cap, uint32_t pl) { (void)cap; return make_dynblock(cfg, pl); }   /* S0=ceil_class(pl)=pl */
static b_cache_t *mk_dyn4096(const cfg_t *cfg, uint32_t cap, uint32_t pl) { (void)cap; (void)pl; return make_dynblock(cfg, 4096); } /* S0=4096，预热收敛 */
static b_cache_t *mk_idx   (const cfg_t *cfg, uint32_t cap, uint32_t pl) { (void)cfg; (void)pl; return make_index_only(cap); }

typedef struct {
    const char *name;
    make_fn      make;
    int          converge;   /* 1 = 预热期 adaptive=1 收敛 S→pl，再冻结为 0 */
} method_t;

static const method_t METHODS[] = {
    { "fifo_bounded",          mk_fifo_b,  0 },
    { "chained_hash_bounded",  mk_hash_b,  0 },
    { "balanced_tree_bounded", mk_tree_b,  0 },
    { "psn_dynblock",          mk_dyn_pl,  0 },   /* S0=payload（主结果） */
    { "psn_dynblock_adaptive", mk_dyn4096, 1 },   /* 收敛后冻结（机制忠实，互证） */
    { "index_only",            mk_idx,     0 },
};
#define N_METHODS (sizeof(METHODS) / sizeof(METHODS[0]))

/* ---- 模式（NAK 重传工作负载） ---- */
typedef struct {
    const char *name;
    uint32_t    count;   /* 每操作包数 */
    int         is_set;  /* 1 = SR(retrieve_set)，0 = GBN(retrieve_range) */
} e1b_mode_t;

/* ---- 查询序列预生成（RNG 不进计时区间；同一 (N, mode) 跨方法复用 ⇒ 公平） ---- */
static void gen_queries(uint32_t *q, const e1b_mode_t *m, uint32_t N,
                        uint32_t n_ops, uint32_t seed) {
    xorshift32_t rng; xorshift32_seed(&rng, seed);
    uint32_t count = m->count, stride = m->is_set ? count : 1u;
    if (m->is_set) {
        for (uint32_t i = 0; i < n_ops; i++)
            for (uint32_t k = 0; k < count; k++)
                q[(size_t)i * stride + k] = xorshift32_next(&rng) % N;
    } else {
        uint32_t span = N - count + 1u;   /* start ∈ [0, N-count] */
        for (uint32_t i = 0; i < n_ops; i++)
            q[i] = xorshift32_next(&rng) % span;
    }
}

/* 上层整理：把 PSN 集升序（小规模插入排序，同对所有方法、计入计时） */
static void ins_sort_u32(uint32_t *a, uint32_t n) {
    for (uint32_t i = 1; i < n; i++) {
        uint32_t x = a[i], j = i;
        while (j > 0 && a[j - 1] > x) { a[j] = a[j - 1]; j--; }
        a[j] = x;
    }
}

/* ---- retrieve 批量计时 + n_cmp：每批 B 次操作夹一对 rdtsc；n_cmp 批前清零、批后累计 ----
 * reorder=1 且 SR：先把该次 64 个 PSN 升序整理，再 retrieve_set（「开始取→整理完可发」）。 */
static size_t time_retrieve(b_cache_t *c, const e1b_mode_t *m, const uint32_t *q,
                            uint32_t B, uint32_t n_batches, uint32_t reps,
                            uint8_t *out, uint32_t out_cap, int reorder,
                            double *samples, uint64_t *total_cmp) {
    size_t idx = 0;
    uint64_t cmp = 0;
    uint32_t wsum = 0;
    uint32_t count = m->count, stride = m->is_set ? count : 1u;
    uint32_t sorted[64];
    for (uint32_t r = 0; r < reps; r++) {
        for (uint32_t b = 0; b < n_batches; b++) {
            const uint32_t *qb = q + (size_t)b * B * stride;
            c->n_cmp = 0;
            uint64_t t0 = rdtsc_raw();
            if (m->is_set) {
                for (uint32_t k = 0; k < B; k++) {
                    if (reorder) {
                        memcpy(sorted, qb + (size_t)k * stride, count * sizeof(uint32_t));
                        ins_sort_u32(sorted, count);
                        wsum += c->ops.retrieve_set(c, sorted, count, out, out_cap);
                    } else {
                        wsum += c->ops.retrieve_set(c, qb + (size_t)k * stride, count, out, out_cap);
                    }
                }
            } else {
                for (uint32_t k = 0; k < B; k++)
                    wsum += c->ops.retrieve_range(c, qb[(size_t)k * stride], count, out, out_cap);
            }
            uint64_t t1 = rdtsc_raw();
            samples[idx++] = to_ns(t1 - t0) / (double)B;
            cmp += c->n_cmp;
        }
    }
    g_sink ^= (uint64_t)wsum ^ (uint64_t)(out[0] + out[out_cap - 1]);   /* 抗 DCE */
    *total_cmp = cmp;
    return idx;
}

/* ---- 空批地板：同循环结构 + 同一对 rdtsc，批内无 retrieve（屏障防 DCE） ---- */
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
    snprintf(cfg.out_dir, sizeof(cfg.out_dir), "out/exp1b_lookup");
    if (cfg_override_cli(&cfg, argc, argv) != 0) return 1;
    /* 取路径无 resize；自适应变体在预热期临时开 adaptive、收敛后冻结（见下面 converge 处理） */

    tsc_calibrate();
    if (mkdir_p(cfg.out_dir) < 0) { fprintf(stderr, "[exp1b] 无法创建输出目录 %s\n", cfg.out_dir); return 1; }

    char path[512];
    snprintf(path, sizeof(path), "%s/resolved_config.json", cfg.out_dir);
    cfg_dump_json(&cfg, path);

    uint32_t reps      = cfg.reps;
    uint32_t B         = cfg.e1b_batch_ops;
    uint32_t n_batches = cfg.e1b_queries / B;      /* 全批（不裁尾批） */
    uint32_t n_ops     = n_batches * B;
    uint32_t sample_n  = n_batches * reps;
    uint32_t pl        = cfg.e1b_payload_list[0];  /* 只跑 1024（范围收窄） */
    /* warm 按扫描点 N 在循环内算（环长=N，6 纪元预热收敛 S0=4096→pl） */

    e1b_mode_t modes[4] = {
        { "gbn_long64", cfg.e1b_gbn_long, 0 },
        { "gbn_short8", cfg.e1b_gbn_short, 0 },
        { "sr_16",      cfg.e1b_sr_k1,    1 },
        { "sr_64",      cfg.e1b_sr_k2,    1 },
    };
    const uint32_t N_MODES = sizeof(modes) / sizeof(modes[0]);

    uint32_t max_count = 0;
    for (uint32_t mm = 0; mm < N_MODES; mm++)
        if (modes[mm].count > max_count) max_count = modes[mm].count;
    uint32_t out_cap = max_count * pl;   /* 64×1024 = 64KB */

    uint8_t *out     = (uint8_t *)malloc(out_cap);
    double  *samples = (double *)malloc((size_t)sample_n * sizeof(double));
    double  *fs      = (double *)malloc((size_t)sample_n * sizeof(double));
    if (!out || !samples || !fs) { fprintf(stderr, "[exp1b] OOM\n"); return 1; }

    /* 空批地板（方法无关，每 B 测一次；floor ∝ 1/B 论证） */
    size_t  fn_main = time_floor(B, n_batches, reps, fs);
    double  floor_main = median_dbl(fs, fn_main);
    uint32_t Bx = cfg.e1b_batch_xval_ops;
    size_t  fn_xval = time_floor(Bx, n_batches, reps, fs);
    double  floor_xval = median_dbl(fs, fn_xval);
    printf("exp1b floor: B=%u median=%.3f ns/op; B=%u (xval) median=%.3f ns/op\n",
           B, floor_main, Bx, floor_xval);

    /* lookup_summary.csv + n_cmp.csv */
    snprintf(path, sizeof(path), "%s/lookup_summary.csv", cfg.out_dir);
    FILE *f = fopen(path, "w");
    if (!f) { fprintf(stderr, "[exp1b] 无法打开 %s\n", path); return 1; }
    snprintf(path, sizeof(path), "%s/n_cmp.csv", cfg.out_dir);
    FILE *fc = fopen(path, "w");
    if (!fc) { fprintf(stderr, "[exp1b] 无法打开 %s\n", path); return 1; }

    fprintf(f, "# exp1b lookup/retrieve time cost (batch-amortized). payload=%u reps=%u B=%u n_batches=%u\n",
            pl, reps, B, n_batches);
    fprintf(f, "# methods: fifo_bounded / chained_hash_bounded / balanced_tree_bounded / psn_dynblock(S0=payload) / psn_dynblock_adaptive(收敛后冻结) / index_only(对照E)\n");
    fprintf(f, "# modes: gbn_long64(64) / gbn_short8(8) / sr_16(16) / sr_64(64); per-op = 一次 retrieve_range/retrieve_set(定位+memcpy 全过程)\n");
    fprintf(f, "# reorder: 0=仅取包; 1=取包前先把 PSN 集升序整理(仅 SR; GBN 输出天然有序故恒 0)\n");
    fprintf(f, "# N=缓存深度(工作集条数)=环长(所有方法同义); dynblock 环长=N(Φ=psn%%N O(1)); index_only=Φ纯算术 len=0 无 memcpy\n");
    fprintf(f, "# p50=主指标 p90=次指标; p99 仅留痕(实机尾部干净, p99 备查)\n");
    fprintf(f, "# floor(空批,读钟/B): B=%u median=%.3f ns/op; B=%u(xval) median=%.3f ns/op\n",
            B, floor_main, Bx, floor_xval);
    fprintf(f, "method,mode,N,payload,B,n_batches,reps,reorder,mean_ns,std_ns,p50_ns,p90_ns,p99_ns\n");

    fprintf(fc, "# exp1b n_cmp: retrieve 比较次数(纯取包比较; retrieve 前清零、批后累计)\n");
    fprintf(fc, "# mean_cmp_per_pkt = total_cmp / (n_ops*reps*count); FIFO=O(N)~N/2; tree=O(log N); hash~O(1); dynblock/index_only=0(Φ纯算术)\n");
    fprintf(fc, "# reorder 不影响 n_cmp(排序在缓存外)，故每 (method,mode,N) 只记一行(取 reorder=0)\n");
    fprintf(fc, "method,mode,N,payload,total_packets,total_cmp,mean_cmp_per_pkt\n");

    uint8_t pbuf[4096];
    memset(pbuf, 0x5a, pl);   /* 内容不影响 memcpy 成本；固定暖源 */

    for (uint32_t ni = 0; ni < cfg.e1b_n_n; ni++) {
        uint32_t N = cfg.e1b_n_list[ni];
        cfg.ring_n = N;                    /* dynblock 环长=扫描点 N（机制 Φ=psn%N 不变，仅实例化环长） */
        uint32_t warm = 6u * N;            /* 6 纪元预热：S0=4096 收敛到 pl 需 ~5 纪元（4 quiet+2 streak） */
        for (uint32_t mm = 0; mm < N_MODES; mm++) {
            const e1b_mode_t *m = &modes[mm];
            uint32_t stride = m->is_set ? m->count : 1u;
            uint32_t *q = (uint32_t *)malloc((size_t)n_ops * stride * sizeof(uint32_t));
            if (!q) { fprintf(stderr, "[exp1b] OOM query\n"); return 1; }
            gen_queries(q, m, N, n_ops, cfg.seed + N * 1009u + mm * 917u + 1u);

            for (uint32_t mi = 0; mi < N_METHODS; mi++) {
                const method_t *mt = &METHODS[mi];

                /* dynblock 收敛口径：预热期开自适应，让 S 收敛；其余方法/固定 S0 则全程关 */
                cfg.adaptive_enable = mt->converge ? 1u : 0u;
                b_cache_t *c = mt->make(&cfg, N, pl);

                if (mt->converge) {                       /* 预热收敛 S：4096 → pl(=1024) */
                    for (uint32_t k = 0; k < warm; k++) c->ops.store(c, k, pbuf, pl);
                    cfg.adaptive_enable = 0u;             /* 冻结：计时区间无 resize */
                }

                for (uint32_t psn = 0; psn < N; psn++) c->ops.store(c, psn, pbuf, pl);   /* 预填充 0..N-1 */

                /* 预填充冒烟：psn=0 与 N-1 必须命中（防容量/淘汰 off-by-one） */
                uint32_t len0 = 0, len1 = 0;
                const uint8_t *p0 = c->ops.retrieve(c, 0, &len0);
                const uint8_t *p1 = c->ops.retrieve(c, N - 1, &len1);
                if (!p0 || !p1)
                    fprintf(stderr, "[exp1b] WARN %s mode=%s N=%u: populate miss (0=%p N-1=%p)\n",
                            mt->name, m->name, N, (void *)p0, (void *)p1);

                /* reorder 维度：GBN 恒 0（输出天然有序）；SR 跑 0/1 两版对比 */
                int n_reorder = m->is_set ? 2 : 1;
                for (int ro = 0; ro < n_reorder; ro++) {
                    uint64_t total_cmp = 0;
                    size_t ns = time_retrieve(c, m, q, B, n_batches, reps, out, out_cap,
                                              ro, samples, &total_cmp);

                    double mean = mean_dbl(samples, ns);
                    double std  = std_dbl(samples, ns, mean);
                    double p50  = pct_dbl(samples, ns, 50.0);
                    double p90  = pct_dbl(samples, ns, 90.0);
                    double p99  = pct_dbl(samples, ns, 99.0);

                    fprintf(f, "%s,%s,%u,%u,%u,%u,%u,%d,%.3f,%.3f,%.3f,%.3f,%.3f\n",
                            mt->name, m->name, N, pl, B, n_batches, reps, ro,
                            mean, std, p50, p90, p99);

                    if (ro == 0) {   /* n_cmp 只记一次（reorder 不影响比较次数） */
                        uint64_t total_pkts  = (uint64_t)n_ops * reps * m->count;
                        double   cmp_per_pkt = (total_pkts > 0) ? (double)total_cmp / (double)total_pkts : 0.0;
                        fprintf(fc, "%s,%s,%u,%u,%llu,%llu,%.3f\n",
                                mt->name, m->name, N, pl,
                                (unsigned long long)total_pkts, (unsigned long long)total_cmp, cmp_per_pkt);
                    }
                    printf("  %-22s %-11s N=%5u reorder=%d  p50=%9.3f  p90=%9.3f\n",
                           mt->name, m->name, N, ro, p50, p90);
                    fflush(f); fflush(fc); fflush(stdout);
                }
                c->ops.destroy(c);
            }
            free(q);
        }
    }

    fclose(f); fclose(fc);
    free(out); free(samples); free(fs);
    printf("exp1b_lookup done -> %s/lookup_summary.csv + %s/n_cmp.csv\n", cfg.out_dir, cfg.out_dir);
    return 0;
}

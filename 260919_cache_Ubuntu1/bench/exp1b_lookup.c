/*
 * bench/exp1b_lookup.c — 实验一(b)：取时间开销（lookup time cost vs N）
 * --------------------------------------------------------------------------------
 * 精简矩阵（2026-09-17 修订）：
 *   方法 = fifo_bounded / chained_hash_bounded / balanced_tree_bounded /
 *          psn_dynblock(S0=payload) / psn_dynblock_adaptive(收敛后冻结) /
 *          ring_fixed(固定 S=4096 去弹性) / index_only(对照 E)。
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
 * 交付契约（第 4B，2026-09-19）：retrieve_set 统一按 PSN 升序交付命中包。
 *   - GBN（连续区间）：retrieve_range 天然按 start+k 升序 ⇒ 无需重排。
 *   - SR（离散集合）：retrieve_set 内部分法各异但同契约——fifo/hash 显式 qsort、
 *     tree 中序遍历零排序、dynblock 位置映射零排序扫槽；升序成本已含在计时内。
 *   （原 reorder∈{0,1} 维度因第 4B 升序契约上移到 retrieve_set 内而废止，不再单列。）
 *
 * 负载口径（第 35 条，2026-09-20 修正「固定重传报文数」）：
 *   - 每个操作（一次重传）恰好 K 个**互不相同**的报文，K 与 N 无关（K 由配置给定：8/16/64）。
 *   - GBN（连续）＝ K 个连续 PSN（start .. start+K-1）；SR（离散）＝ K 个离散互不相同 PSN（部分 Fisher–Yates 无放回）。
 *   - 原实现 SR 为「有放回抽样」，重复 PSN 被位图折叠 ⇒ 实际搬运包数随 N 变化（N=128≈50.6 vs N=5120≈63.6，差 26%），
 *     在「N 曲线」里混入假增长；本次改无放回，消除该偏差。
 *   - 前置守卫：N ≥ K（最小 N=128 ≥ 64）；GBN 分支 N-K+1 > 0。
 *
 * 计时口径：
 *   - 计时单位 = 一次 retrieve_range / retrieve_set（定位 + memcpy 全过程，即一次 GBN/SR 重传
 *     的完整开销；SR 升序交付成本已含在 retrieve_set 内——fifo/hash qsort、tree/dynblock 零排序）；
 *     per-op ns = (t1-t0)/B。**报告量 = 每批次（K 个报文）的完整提取时间（ns/batch），不除以 K。**
 *   - 主指标 p50（=batch_p50_ns），次指标 p90；p99 仅 CSV 留痕（实机尾部干净，p99 备查）。
 *   - n_cmp：内部诊断（retrieve 前清零、批后累计，报「每次取包平均比较次数」）；退出正文——主/次指标
 *     只用 p50/p90，n_cmp 不进 RESULTS.md/图/论文（n_cmp.csv 仍落盘留痕）；交付排序不计 n_cmp。
 *   - 空批地板：floor ∝ 1/B，同时测 B=512 与 B=32（xval）。
 *   - CSV 新列（第 35 条）：pkts_per_op=K（审计「K 不随 N 变」）、ring_bytes=N×stride（dynblock 占用面积，
 *     非 dynblock=0）、main_metric=batch_p50_ns（语义标注 p50_ns 为每批次）。
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
static b_cache_t *mk_dyn_fixed4096(const cfg_t *cfg, uint32_t cap, uint32_t pl) { (void)cap; (void)pl; return make_dynblock(cfg, 4096); } /* S=4096 固定（去弹性；converge=0） */
static b_cache_t *mk_idx   (const cfg_t *cfg, uint32_t cap, uint32_t pl) { (void)cfg; (void)pl; return make_index_only(cap); }

typedef struct {
    const char *name;
    make_fn      make;
    int          converge;   /* 1 = 预热期 adaptive=1 收敛 S→pl，再冻结为 0 */
} method_t;

static const method_t METHODS[] = {
    { "fifo_bounded",          mk_fifo_b,        0 },
    { "chained_hash_bounded",  mk_hash_b,        0 },
    { "balanced_tree_bounded", mk_tree_b,        0 },
    { "psn_dynblock",          mk_dyn_pl,        0 },   /* S0=payload（主结果） */
    { "psn_dynblock_adaptive", mk_dyn4096,       1 },   /* 收敛后冻结（机制忠实，互证） */
    { "ring_fixed",            mk_dyn_fixed4096, 0 },   /* PSN 位置映射 + 固定 S=4096（去弹性；唯一变量=块大小） */
    { "index_only",            mk_idx,           0 },
};
#define N_METHODS (sizeof(METHODS) / sizeof(METHODS[0]))

/* ---- 模式（NAK 重传工作负载） ---- */
typedef struct {
    const char *name;
    uint32_t    count;   /* 每操作包数 */
    int         is_set;  /* 1 = SR(retrieve_set)，0 = GBN(retrieve_range) */
} e1b_mode_t;

/* ---- 查询序列预生成（RNG 不进计时区间；同一 (N, mode) 跨方法复用 ⇒ 公平） ----
 * 第 35 条修正：每个操作恰好 K 个互不相同的 PSN（SR 无放回，部分 Fisher–Yates；GBN K 个连续）。 */
static void gen_queries(uint32_t *q, const e1b_mode_t *m, uint32_t N,
                        uint32_t n_ops, uint32_t seed) {
    xorshift32_t rng; xorshift32_seed(&rng, seed);
    uint32_t count = m->count, stride = m->is_set ? count : 1u;
    if (m->is_set) {
        /* SR：部分 Fisher–Yates 无放回抽样 K 个互不相同 PSN（全部落在 [0,N)）。
         * idx 恒等初始化一次；每操作做 K 次交换抽取后逆序撤销，恢复恒等 ⇒ 操作间独立。 */
        uint32_t *idx = (uint32_t *)malloc((size_t)N * sizeof(uint32_t));
        uint32_t *js  = (uint32_t *)malloc((size_t)count * sizeof(uint32_t));
        if (!idx || !js) { fprintf(stderr, "[exp1b] OOM query idx\n"); exit(1); }
        for (uint32_t i = 0; i < N; i++) idx[i] = i;
        for (uint32_t i = 0; i < n_ops; i++) {
            for (uint32_t k = 0; k < count; k++) {
                uint32_t j = k + xorshift32_next(&rng) % (N - k);
                uint32_t t = idx[k]; idx[k] = idx[j]; idx[j] = t;
                js[k] = j;
                q[(size_t)i * stride + k] = idx[k];
            }
            for (uint32_t k = count; k-- > 0;) {   /* 逆序撤销 K 次交换，恢复恒等 */
                uint32_t t = idx[k]; idx[k] = idx[js[k]]; idx[js[k]] = t;
            }
        }
        free(idx); free(js);
    } else {
        /* GBN：start ∈ [0, N-K]，K 个连续 PSN = start .. start+K-1（retrieve_range 自然升序） */
        uint32_t span = N - count + 1u;
        for (uint32_t i = 0; i < n_ops; i++)
            q[i] = xorshift32_next(&rng) % span;
    }
}

/* ---- 请求集唯一性门（第 35 条 T4.3）：对每个操作的 K 个 PSN 计数，必须 == K（无重复）。
 * 返回违规操作数（0 = 全部通过）。SR 查 K 个互异；GBN 查 start+count-1 不越界（连续性由构造保证）。 */
static uint32_t validate_queries(const uint32_t *q, const e1b_mode_t *m, uint32_t N,
                                 uint32_t n_ops) {
    uint32_t count = m->count, bad = 0;
    if (m->is_set) {
        uint32_t seen[64];
        for (uint32_t i = 0; i < n_ops; i++) {
            uint32_t ns = 0; int dup = 0;
            for (uint32_t k = 0; k < count && !dup; k++) {
                uint32_t v = q[(size_t)i * count + k];
                for (uint32_t s = 0; s < ns; s++) if (seen[s] == v) { dup = 1; break; }
                seen[ns++] = v;
            }
            if (dup) bad++;
        }
    } else {
        for (uint32_t i = 0; i < n_ops; i++)
            if (q[i] + count > N) bad++;   /* start+count-1 越界（K 个连续 PSN 必须落在 [0,N)） */
    }
    return bad;
}

/* ---- retrieve 批量计时 + n_cmp：每批 B 次操作夹一对 rdtsc；n_cmp 批前清零、批后累计 ----
 * retrieve_set 按第 4B 升序契约交付（排序在接口内），计时 = 定位 + memcpy + 升序交付全过程。 */
static size_t time_retrieve(b_cache_t *c, const e1b_mode_t *m, const uint32_t *q,
                            uint32_t B, uint32_t n_batches, uint32_t reps,
                            uint8_t *out, uint32_t out_cap,
                            double *samples, uint64_t *total_cmp,
                            uint64_t *delivered_bytes) {
    size_t idx = 0;
    uint64_t cmp = 0;
    uint64_t wsum = 0;
    uint32_t count = m->count, stride = m->is_set ? count : 1u;
    for (uint32_t r = 0; r < reps; r++) {
        for (uint32_t b = 0; b < n_batches; b++) {
            const uint32_t *qb = q + (size_t)b * B * stride;
            c->n_cmp = 0;
            uint64_t t0 = rdtsc_raw();
            if (m->is_set) {
                for (uint32_t k = 0; k < B; k++)
                    wsum += c->ops.retrieve_set(c, qb + (size_t)k * stride, count, out, out_cap);
            } else {
                for (uint32_t k = 0; k < B; k++)
                    wsum += c->ops.retrieve_range(c, qb[(size_t)k * stride], count, out, out_cap);
            }
            uint64_t t1 = rdtsc_raw();
            samples[idx++] = to_ns(t1 - t0) / (double)B;
            cmp += c->n_cmp;
        }
    }
    g_sink ^= wsum ^ (uint64_t)(out[0] + out[out_cap - 1]);   /* 抗 DCE */
    *total_cmp = cmp;
    *delivered_bytes = wsum;
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

    /* ---- 第 35 条验收门状态 ---- */
    int    gate_fail = 0;                    /* 硬门（T4.1 固定包数 / T4.2 同 K / T4.3 唯一性）任一 FAIL 置 1 */
    double min_p50  = 1e18;                  /* 全矩阵最小 p50（T4.4 地板门对照） */
    double idx_sr64_128 = 0, idx_sr64_5120 = 0;   /* T4.5 自洽：index_only sr_64 p50 @ 128/5120（sort_impl=0） */
    double psn_sr64_128 = 0, psn_sr64_5120 = 0;   /* T4.5 自洽：psn_dynblock_adaptive sr_64 p50 @ 128/5120 */

    /* T4.2 同 K 门：gbn_long64 与 sr_64 的 pkts_per_op 必须都 = 64 */
    if (cfg.e1b_gbn_long != 64u || cfg.e1b_sr_k2 != 64u) {
        fprintf(stderr, "[GATE2 same-K] FAIL: gbn_long64=%u sr_64=%u (均须=64)\n",
                cfg.e1b_gbn_long, cfg.e1b_sr_k2);
        gate_fail = 1;
    } else {
        printf("[GATE2 same-K] PASS: gbn_long64 pkts_per_op=%u sr_64 pkts_per_op=%u (=64)\n",
               cfg.e1b_gbn_long, cfg.e1b_sr_k2);
    }

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
    fprintf(f, "# methods: fifo_bounded / chained_hash_bounded / balanced_tree_bounded / psn_dynblock(S0=payload) / psn_dynblock_adaptive(收敛后冻结) / ring_fixed(固定S=4096去弹性) / index_only(对照E)\n");
    fprintf(f, "# modes: gbn_long64(64) / gbn_short8(8) / sr_16(16) / sr_64(64); per-op = 一次 retrieve_range/retrieve_set(定位+memcpy+升序交付全过程)\n");
    fprintf(f, "# SR retrieve_set 按第 4B 升序契约交付; sort_impl=0 主(glibc qsort)/1 xval(内联插入排序)，只作用于 SR 排序路径(GBN 无排序故两遍一致)\n");
    fprintf(f, "#   fifo/hash/index_only 显式 sort_u32_asc; tree=tree_search_set(排序+k 次 O(log N) 查找, 主; tree_inorder=1 才退中序 O(N)); dynblock 位置映射扫槽\n");
    fprintf(f, "# N=缓存深度(工作集条数)=环长(所有方法同义); dynblock 环长=N(Φ=psn%%N O(1)); index_only=Φ纯算术 len=0 无 memcpy\n");
    fprintf(f, "# p50=主指标 p90=次指标; p99 仅留痕(实机尾部干净, p99 备查)\n");
    fprintf(f, "# floor(空批,读钟/B): B=%u median=%.3f ns/op; B=%u(xval) median=%.3f ns/op\n",
            B, floor_main, Bx, floor_xval);
    fprintf(f, "# 第 35 条口径：pkts_per_op=K（每个操作固定 K 个重传报文；SR=K 个互异 PSN、GBN=K 个连续 PSN，K 在所有 N 下恒定）\n");
    fprintf(f, "# ring_bytes=dynblock 占用面积=N×stride(S)（stride=align16(S)，24B 头不计）；非 dynblock 方法=0（无环缓冲）\n");
    fprintf(f, "# main_metric=batch_p50_ns（每批固定 K 个报文的 retrieve 摊到单批的 p50，即「每批固定 K 个报文」口径）\n");
    fprintf(f, "method,sort_impl,mode,N,pkts_per_op,ring_bytes,payload,B,n_batches,reps,mean_ns,std_ns,p50_ns,p90_ns,p99_ns,main_metric\n");

    fprintf(fc, "# exp1b n_cmp: retrieve 比较次数(纯取包比较; retrieve 前清零、批后累计)\n");
    fprintf(fc, "# mean_cmp_per_pkt = total_cmp / (n_ops*reps*count); FIFO=O(N)~N/2; tree=O(log N); hash~O(1); dynblock/index_only=0(Φ纯算术)\n");
    fprintf(fc, "# 交付排序不计 n_cmp(n_cmp 只计缓存内检索比较)；sort_impl 只影响 SR 排序，n_cmp 两遍应一致\n");
    fprintf(fc, "method,sort_impl,mode,N,payload,total_packets,total_cmp,mean_cmp_per_pkt\n");

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

            /* T4.3 请求集唯一性门：每个操作的 K 个 PSN 无重复（SR）/ start+count-1 不越界（GBN） */
            {
                uint32_t bad = validate_queries(q, m, N, n_ops);
                if (bad) {
                    fprintf(stderr, "[GATE3 unique] FAIL: %s N=%u %u/%u ops 违规\n",
                            m->name, N, bad, n_ops);
                    gate_fail = 1;
                } else {
                    printf("[GATE3 unique] PASS: %s N=%u %u/%u ops 全通过\n",
                           m->name, N, n_ops - bad, n_ops);
                }
            }

            for (uint32_t mi = 0; mi < N_METHODS; mi++) {
                const method_t *mt = &METHODS[mi];

                /* sort_impl 两遍：主矩阵(0=glibc qsort) 与 xval(1=内联插入排序)。
                 * 查询序列 q 在 (N,mode) 生成一次、两遍复用 ⇒ 唯一变量=排序原语；GBN 无排序故两遍一致。 */
                for (uint32_t si = 0; si < 2; si++) {
                    cfg.e1b_sort_impl = si;

                    /* dynblock 收敛口径：预热期开自适应，让 S 收敛；其余方法/固定 S0 则全程关 */
                    cfg.adaptive_enable = mt->converge ? 1u : 0u;
                    b_cache_t *c = mt->make(&cfg, N, pl);
                    c->sort_impl    = (int)si;                   /* SR 排序原语透传（fifo/hash/index_only/tree） */
                    c->tree_inorder = (int)cfg.e1b_tree_inorder; /* tree SR 主(0)/xval(1) 路径透传 */

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
                        fprintf(stderr, "[exp1b] WARN %s mode=%s N=%u si=%u: populate miss (0=%p N-1=%p)\n",
                                mt->name, m->name, N, si, (void *)p0, (void *)p1);

                    /* retrieve_set 按第 4B 升序契约交付（排序在接口内），每 (method,sort_impl,mode,N) 测一次 */
                    {
                        uint64_t total_cmp = 0;
                        uint64_t delivered = 0;
                        size_t ns = time_retrieve(c, m, q, B, n_batches, reps, out, out_cap,
                                                  samples, &total_cmp, &delivered);

                        double mean = mean_dbl(samples, ns);
                        double std  = std_dbl(samples, ns, mean);
                        double p50  = pct_dbl(samples, ns, 50.0);
                        double p90  = pct_dbl(samples, ns, 90.0);
                        double p99  = pct_dbl(samples, ns, 99.0);

                        /* ring_bytes：dynblock 占用面积 = N × stride(S)（stride=align16(S)，24B 头不计）；非 dynblock=0 */
                        uint32_t S = dynblock_cur_S(c);
                        uint32_t ring_bytes = (S > 0) ? (N * stride_of(S)) : 0u;

                        /* T4.1 固定包数门：Σ交付字节 == n_ops×reps×K×payload（K 恒定 ⇒ 排除变包数假增长）。
                         * index_only 为 Φ 纯算术对照（len=0、无 payload memcpy），delivered=0 属设计使然，豁免本门。 */
                        uint64_t expect = (uint64_t)n_ops * reps * m->count * pl;
                        int is_index_only = (strcmp(mt->name, "index_only") == 0);
                        if (!is_index_only && delivered != expect) {
                            fprintf(stderr, "[GATE1 fixed-pkt] FAIL: %s si=%u %s N=%u delivered=%llu expect=%llu\n",
                                    mt->name, si, m->name, N,
                                    (unsigned long long)delivered, (unsigned long long)expect);
                            gate_fail = 1;
                        } else {
                            printf("[GATE1 fixed-pkt] PASS: %s si=%u %s N=%u delivered=%llu%s\n",
                                   mt->name, si, m->name, N, (unsigned long long)delivered,
                                   is_index_only ? "（index_only 无 payload，豁免）" : " == K×payload");
                        }

                        if (p50 < min_p50) min_p50 = p50;

                        /* T4.5 自洽门取样：sr_64 sort_impl=0 下 index_only / psn_dynblock_adaptive 的 p50 @ N=128/5120 */
                        if (si == 0 && strcmp(m->name, "sr_64") == 0) {
                            if (strcmp(mt->name, "index_only") == 0) {
                                if (N == 128)  idx_sr64_128  = p50;
                                if (N == 5120) idx_sr64_5120 = p50;
                            } else if (strcmp(mt->name, "psn_dynblock_adaptive") == 0) {
                                if (N == 128)  psn_sr64_128  = p50;
                                if (N == 5120) psn_sr64_5120 = p50;
                            }
                        }

                        fprintf(f, "%s,%u,%s,%u,%u,%u,%u,%u,%u,%u,%.3f,%.3f,%.3f,%.3f,%.3f,%s\n",
                                mt->name, si, m->name, N, m->count, ring_bytes, pl, B, n_batches, reps,
                                mean, std, p50, p90, p99, "batch_p50_ns");

                        uint64_t total_pkts  = (uint64_t)n_ops * reps * m->count;
                        double   cmp_per_pkt = (total_pkts > 0) ? (double)total_cmp / (double)total_pkts : 0.0;
                        fprintf(fc, "%s,%u,%s,%u,%u,%llu,%llu,%.3f\n",
                                mt->name, si, m->name, N, pl,
                                (unsigned long long)total_pkts, (unsigned long long)total_cmp, cmp_per_pkt);

                        printf("  %-22s si=%u %-11s N=%5u  p50=%9.3f  p90=%9.3f\n",
                               mt->name, si, m->name, N, p50, p90);
                        fflush(f); fflush(fc); fflush(stdout);
                    }
                    c->ops.destroy(c);
                }   /* si (sort_impl) */
            }
            free(q);
        }
    }

    fclose(f); fclose(fc);

    /* ---- T4.4 地板门：空批地板 ≤ 最快 op 的 1–2%（读钟摊销已从信号剥离） ---- */
    if (min_p50 < 1e17) {
        double ratio = floor_main / min_p50 * 100.0;
        printf("[GATE4 floor] floor(B=%u)=%.3f ns/op vs 最快 op p50=%.3f ns -> %.3f%% %s (阈值 ≤2%%)\n",
               B, floor_main, min_p50, ratio, (ratio <= 2.0) ? "PASS" : "WARN");
    }

    /* ---- T4.5 结果自洽门：index_only 平坦；SR-64 增长应明显 < 旧版 +74%（消除变包数假增长后） ---- */
    if (idx_sr64_128 > 0 && idx_sr64_5120 > 0 && psn_sr64_128 > 0 && psn_sr64_5120 > 0) {
        double idx_g = (idx_sr64_5120 - idx_sr64_128) / idx_sr64_128 * 100.0;
        double psn_g = (psn_sr64_5120 - psn_sr64_128) / psn_sr64_128 * 100.0;
        printf("[GATE5 self-consistency] sr_64 N=128->5120: index_only %.3f->%.3f (%.1f%%) ; "
               "psn_dynblock_adaptive %.3f->%.3f (%.1f%%, 旧版 +74%%)\n",
               idx_sr64_128, idx_sr64_5120, idx_g, psn_sr64_128, psn_sr64_5120, psn_g);
        printf("   index_only 平坦(增长<5%%): %s ; psn 增长明显小于旧版 +74%%: %s\n",
               (idx_g < 5.0) ? "是" : "否（见下诊断，如实报告）",
               (psn_g < 74.0) ? "是" : "否（如实报告，不改口径）");
    }

    free(out); free(samples); free(fs);
    if (gate_fail) {
        fprintf(stderr, "exp1b_lookup: 验收硬门 FAIL（见上 GATE1/2/3），退出非 0\n");
        return 1;
    }
    printf("exp1b_lookup done -> %s/lookup_summary.csv + %s/n_cmp.csv\n", cfg.out_dir, cfg.out_dir);
    return 0;
}

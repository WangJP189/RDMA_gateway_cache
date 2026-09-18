/*
 * bench/exp2_space.c — 实验二：空间利用率（space utilization）
 * --------------------------------------------------------------------------------
 * 指标：utilization = payload_bytes / allocated_bytes（满窗 N 条、包长 pl）。
 *   payload_bytes   = N × pl（真正承载的报文数据字节）。
 *   allocated_bytes = cache_footprint_bytes(bc)：为容纳满窗所需的全部预分配字节
 *                     （池 + 索引/桶 + 元数据），用 sizeof() 实测，不硬编码。
 *
 * 方法（5 个，全部有界预分配，无界版每 store malloc 不进 exp2）：
 *   fifo_bounded           —— 预分配节点池 cap=N（每节点 32B+pl）
 *   chained_hash_bounded   —— 预分配节点池 + 桶数组(16384) + slot_owner 数组(N)
 *   balanced_tree_bounded  —— 预分配节点池（每节点 56B+pl）+ FIFO 指针数组(N)
 *   psn_dynblock（弹性）   —— S=ceil_class(pl)=pl（5 档 MTU 精确命中），环池 stride=align16(24+S)
 *   psn_dynblock_fixed     —— S=e2_fixed_l=4096 固定（「不做弹性的固定块」对照）
 *
 * 满窗语义：N=4096（=CFG_RING_N=CFG_E2_N，重传窗口上界，见 RING_N_DERIVATION.md）。填满后活集恒 N。
 *   每个方法填 N 条 psn=0..N-1（逐槽命中，无淘汰、无溢出、无 resize），
 *   断言 n_malloc==0（dynblock 无溢出 malloc）且 n_resize==0（无自适应切换），
 *   即「满窗占用」是纯静态量，与访问序列无关。
 *
 * 想证的三件事（对应 RESULTS.md 正文）：
 *   ① 我们（弹性）利用率虽非最高，但与最优基线 FIFO 相差 <=5%（256B 档 ~4.4pp、4096B 档 ~0.3pp）；
 *   ② psn_dynblock_fixed 在 256B 档崩到 ~6%（4128B 槽只装 256B），证明「弹性槽大小」解决了
 *      固定块空间利用率低下的问题；
 *   ③ 弹性 S 收敛到各 MTU 档（block_S 列），且槽浪费 = align16 填充 + 24B 头 + 12B meta。
 *
 * 编译：make build/exp2_space && ./build/exp2_space [--out=DIR]
 * 产物：<out>/space_summary.csv + <out>/resolved_config.json
 */
#include "baseline.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>

/* ---- 输出目录（与 exp1a/sim_main 同款递归 mkdir） ---- */
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

/* ---- 方法表 ---- */
typedef b_cache_t *(*make_fn)(const cfg_t *cfg, uint32_t payload_len);

static b_cache_t *mk_fifo(const cfg_t *cfg, uint32_t pl) { (void)cfg; return make_fifo_bounded(cfg->e2_n, pl); }
static b_cache_t *mk_hash(const cfg_t *cfg, uint32_t pl) { return make_chained_hash_bounded(cfg->e2_n, cfg->hash_nbuckets, pl); }
static b_cache_t *mk_tree(const cfg_t *cfg, uint32_t pl) { (void)cfg; return make_balanced_tree_bounded(cfg->e2_n, pl); }
static b_cache_t *mk_dyn_elastic(const cfg_t *cfg, uint32_t pl) { return make_dynblock(cfg, pl); }   /* S0=ceil_class(pl)=pl */
static b_cache_t *mk_dyn_fixed(const cfg_t *cfg, uint32_t pl) { (void)pl; return make_dynblock(cfg, cfg->e2_fixed_l); } /* S0=4096 固定 */

typedef struct {
    const char *name;
    make_fn     make;
    int         is_dyn;    /* 1 = 输出 block_S 列 + 不查 n_live（dynblock 不维护 bc->n_live） */
} method_t;

static const method_t METHODS[] = {
    { "fifo_bounded",          mk_fifo,        0 },
    { "chained_hash_bounded",  mk_hash,        0 },
    { "balanced_tree_bounded", mk_tree,        0 },
    { "psn_dynblock",          mk_dyn_elastic, 1 },
    { "psn_dynblock_fixed",    mk_dyn_fixed,   1 },
};
#define N_METHODS (sizeof(METHODS) / sizeof(METHODS[0]))

int main(int argc, char **argv) {
    cfg_t cfg; cfg_default(&cfg);
    snprintf(cfg.out_dir, sizeof(cfg.out_dir), "out/exp2_space");
    if (cfg_override_cli(&cfg, argc, argv) != 0) return 1;

    if (mkdir_p(cfg.out_dir) < 0) { fprintf(stderr, "[exp2] 无法创建输出目录 %s\n", cfg.out_dir); return 1; }

    char path[512];
    snprintf(path, sizeof(path), "%s/resolved_config.json", cfg.out_dir);
    cfg_dump_json(&cfg, path);

    uint32_t N = cfg.e2_n;

    snprintf(path, sizeof(path), "%s/space_summary.csv", cfg.out_dir);
    FILE *f = fopen(path, "w");
    if (!f) { fprintf(stderr, "[exp2] 无法打开 %s\n", path); return 1; }
    fprintf(f, "# exp2 space utilization (utilization = payload_bytes / allocated_bytes). N=%u full window\n", N);
    fprintf(f, "# methods: fifo_bounded / chained_hash_bounded / balanced_tree_bounded / psn_dynblock(elastic) / psn_dynblock_fixed(S=%u)\n",
            cfg.e2_fixed_l);
    fprintf(f, "# psn_dynblock elastic S=ceil_class(pl)=pl (5 MTU tiers exact); fixed S=%u; block_S=0 for non-dynblock\n",
            cfg.e2_fixed_l);
    fprintf(f, "# allocated_bytes = cache_footprint_bytes (pool + index/buckets + meta, sizeof measured); utilization is fraction 0..1\n");
    fprintf(f, "method,payload,N,block_S,payload_bytes,allocated_bytes,utilization\n");

    uint8_t payload[4096];
    memset(payload, 0x5a, sizeof(payload));

    for (uint32_t pi = 0; pi < cfg.e2_pktsize_n; pi++) {
        uint32_t pl = cfg.e2_pktsize_list[pi];
        for (uint32_t mi = 0; mi < N_METHODS; mi++) {
            const method_t *m = &METHODS[mi];

            cfg.adaptive_enable = 0u;   /* footprint 是静态量：全程不 resize（弹性 S=pl 由构造给定，无需收敛） */
            b_cache_t *c = m->make(&cfg, pl);

            /* 填满窗：psn=0..N-1 逐槽命中（无淘汰、无溢出） */
            for (uint32_t k = 0; k < N; k++) c->ops.store(c, k, payload, pl);

            uint32_t block_S = m->is_dyn ? dynblock_cur_S(c) : 0u;

            /* 断言：无溢出 malloc、无 resize；有界方法满窗驻留 N */
            if (c->n_malloc != 0)
                fprintf(stderr, "[exp2] WARN %s pl=%u: n_malloc=%llu (expect 0: overflow?)\n",
                        m->name, pl, (unsigned long long)c->n_malloc);
            if (c->n_resize != 0)
                fprintf(stderr, "[exp2] WARN %s pl=%u: n_resize=%llu (expect 0: adaptive off)\n",
                        m->name, pl, (unsigned long long)c->n_resize);
            if (!m->is_dyn && c->n_live != N)
                fprintf(stderr, "[exp2] WARN %s pl=%u: n_live=%llu (expect %u)\n",
                        m->name, pl, (unsigned long long)c->n_live, N);

            uint64_t alloc  = cache_footprint_bytes(c);
            uint64_t pbytes = (uint64_t)N * pl;
            double   util   = alloc ? (double)pbytes / (double)alloc : 0.0;

            fprintf(f, "%s,%u,%u,%u,%llu,%llu,%.6f\n",
                    m->name, pl, N, block_S,
                    (unsigned long long)pbytes, (unsigned long long)alloc, util);
            printf("  %-22s pl=%4u S=%4u  payload=%10llu B  alloc=%10llu B  util=%6.2f%%\n",
                   m->name, pl, block_S,
                   (unsigned long long)pbytes, (unsigned long long)alloc, util * 100.0);
            fflush(f); fflush(stdout);

            c->ops.destroy(c);
        }
    }

    fclose(f);
    printf("exp2_space done -> %s/space_summary.csv\n", cfg.out_dir);
    return 0;
}

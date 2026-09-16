/*
 * include/config.h — 260916_cache_AddExperiment1 唯一配置文件
 * ------------------------------------------------------------
 * 机制参数 + 全部实验/负载预设值集中在此。改这里全局生效。
 *
 * 来源：
 *   - 06_动态内存块_实现与实验_完整交付说明.md §3.1（原配置）
 *   - 第 2 轮裁决 P0-B / P1（adaptive_enable、双档位集合、rdtsc 交叉验证、
 *     order_guard、single_thread、pool_use_mmap、sim_link_gbps（仅报告用）、exp1b payload 等）
 *
 * 纪律（第 2 轮重申）：
 *   - 任何被 cfg 读取的字段必须在 cfg_t 声明、cfg_default 赋值、cfg_dump_json 落盘；
 *     三者由 src/config.c 的 FIELDS 表驱动同源，cfg_selftest 断言一致。
 *   - slot_meta_t 等结构一律用 sizeof()，不得硬编码字节数（见 dynblock.h）。
 */
#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>
#include <stddef.h>

/* ==================== A. 机制参数 ==================== */

/* N = 环长（槽数）= 重传窗口长度（in-flight 包数上界），不是自由超参。
 * 定义式：N >= ceil(link_rate_gbps × RTT_ms / MTU_B) × safety
 *   例：100 Gbps × 1 ms RTT = 12.5 MB；MTU 4096 → 3052 包；×2 安全 → 6104 → 取档。
 *   10240 ≈ 100 Gbps × 3 ms RTT @ 4096 B（跨洲 WAN，合理但不精确）；真实场景按实际 RTT/速率重算。
 * 红线：不得为了让基线跑得快而把 N 取成 2 的幂；N 只服从「重传窗口」这一个约束。 */
#define CFG_RING_N              10240u

/* 新档位集合 SC_NEW（27 档）：只给 psn_dynblock / psn_map_static 用。
 * 128B 细粒度到 2048，256B 到 4096，之后 5120/6144/8192。
 * 加档零内存成本（档只是目标标量 S 的候选值），把非档位包长的浪费压到 <=128B。 */
#define CFG_SC_NEW  { 128u,256u,384u,512u,640u,768u,896u,1024u,1152u,1280u, \
                      1408u,1536u,1664u,1792u,1920u,2048u,2304u,2560u,2816u, \
                      3072u,3328u,3584u,3840u,4096u,5120u,6144u,8192u }
#define CFG_SC_NEW_N            27u

/* 旧 6 环档位集合：仅 psn_map_tiered_old 使用（旧代码口径，禁止用于 dynblock/static）。 */
#define CFG_SC_OLD_TIERED    { 256u,512u,1024u,1536u,2048u,4096u }
#define CFG_SC_OLD_TIERED_N     6u

#define CFG_OVF_CAP             2048u   /* 溢出数组容量（硬上限，不增长）；2048 贴近真实网关，使「打满=紧急」变难 */
#define CFG_OVF_THRESH          256u    /* τ_o 绝对条数阈值（不依赖 N、不依赖 OVF_CAP）：溢出条数达到此值触发阈值路径扩大。
                                         *  是「扩容灵敏度旋钮」：越小越灵敏，靠 k_dwell 滞后防误触。 */
#define CFG_HDR_SZ              24u     /* 每块头字节（见 dynblock.h 的 mem_block_header） */
#define CFG_K_DWELL             2u      /* 连续 K 个纪元同向才切换（>=2） */
#define CFG_J_QUIET             4u      /* 连续 J 个纪元无溢出才允许缩 */
#define CFG_SHRINK_GAMMA        0.75    /* 仅当 L_ring <= gamma*S 才缩 */
#define CFG_HIST_DECAY          8u      /* 连续 8 个安静纪元后清空 ovf_max_hist */
#define CFG_FALLBACK_MTU        4096u   /* 建联拿不到 MTU 时的兜底初始 S */

/* ---- 第 2 轮新增 ---- */
#define CFG_ADAPTIVE_ENABLE     1u      /* 0：exp1a/exp1b 彻底跳过 check_and_maybe_resize */
#define CFG_SINGLE_THREAD       1u      /* 1：锁编译为 no-op；基准与模拟器全程单线程 */
#define CFG_POOL_USE_MMAP       1u      /* 1：mmap(MAP_NORESERVE) 惰性提交，失败回落 malloc */
#define CFG_ORDER_GUARD         0u      /* 1A：防旧包覆盖护栏；默认 0 保持热路径纯净 */
#define CFG_SIM_LINK_GBPS       100u    /* 仅模拟器报告用（trace CSV elapsed_us 时间轴），机制侧绝不读取 */
#define CFG_SIM_PEND_CAP        4096u   /* NAK 待发列表容量 */

/* ==================== B. 通用计时/复现 ==================== */
#define CFG_SEED                42u
#define CFG_REPS                5u
#define CFG_TSC_BATCH_OPS       128u  /* 通用 TSC 摊销批量（ops）；exp1a store 用 e1a_batch_ops 单独指定 */
#define CFG_WARMUP_OPS          200000u
#define CFG_RDTSC_XVAL_OPS      10000000u /* P0-B-3：rdtsc 交叉验证操作数（10^7） */

/* ==================== C. 实验一 ==================== */
#define CFG_E1A_N               10240u
#define CFG_E1A_PAYLOAD_LIST    { 64u,1024u,4096u }
#define CFG_E1A_PAYLOAD_N       3u
#define CFG_E1A_TIMED_OPS       500000u  /* 500k：B=8192 下池化 305 点（p50 稳健、p90≈30 点；p99 弃用——VM 调度停顿污染尾部） */
#define CFG_E1A_BATCH_OPS       8192u    /* 主矩阵 store 计时批量 B：实测一对 rdtsc≈3.8μs ⇒ B=8192 摊 0.46ns（占最快方法 ~5.8%）；
                                          *   B=1024 的 3.7ns 地板占最快方法 46%，对数纵轴失真 ⇒ 弃。 */
#define CFG_E1A_BATCH_XVAL_OPS  1024u    /* 交叉验证批量：不同 B 下排序/差距一致 ⇒ 地板论证闭环（B=1024 那组不扔，作对照） */
#define CFG_E1A_SEQ_MODE_SEQ    1u       /* 1=顺序 PSN，0=随机 PSN */

#define CFG_E1B_N_LIST          { 512u,1024u,2048u,4096u,8192u,10240u }
#define CFG_E1B_N_N             6u
#define CFG_E1B_GBN_LONG        64u
#define CFG_E1B_GBN_SHORT       8u
#define CFG_E1B_SR_K1           16u
#define CFG_E1B_SR_K2           64u
#define CFG_E1B_QUERIES         10000u
#define CFG_E1B_PAYLOAD_LIST    { 1024u,64u } /* 主用 1024，64 横向对照 */
#define CFG_E1B_PAYLOAD_N       2u
#define CFG_E1B_BATCH_OPS       1u       /* NAK 计时批大小（ops），默认单事件 */
#define CFG_E1B_BATCH_XVAL_OPS  32u      /* P1-⑨ 对照批大小（ops） */

/* ==================== D. 实验二 ==================== */
#define CFG_E2_PKTSIZE_LIST  { 512u,640u,768u,1024u,1280u,1500u,1536u,2048u,3072u,4096u }
#define CFG_E2_PKTSIZE_N     10u
#define CFG_E2_N_LIST        { 256u,512u,1024u,2048u,4096u,10240u }
#define CFG_E2_N_N           6u
#define CFG_E2_N             10240u
#define CFG_E2_FIXED_L       1024u
#define CFG_HASH_NBUCKETS    16384u  /* hash 自身参数：按目标负载因子 ≈0.6 与预期元素数 N=10240 选定
                                      *  （10240/16384=0.625）；与 behavior_bench.c HASH_BUCKETS=16384 一致。
                                      *  不由 N 决定，不进「与基线对齐」那套；*_bounded 同样用 16384（活集恒 N ⇒ 负载恒 0.625）。 */
#define CFG_FIXED_BLOCK      5120u

/* ==================== E. 实验三 ==================== */
#define CFG_E3_PHASES   { {4u,4096u},{12u,1024u},{2u,4096u},{12u,1024u} }
#define CFG_E3_PHASE_N  4u
#define CFG_E3_MIXED_PHASES  { {6u,1024u},{40u,0u} }  /* 先 6 纪元 1024 缩到 1024，再 40 纪元混合（触发阈值路径） */
#define CFG_E3_MIXED_PHASE_N  2u
#define CFG_E3_MIXED_SMALL   1024u
#define CFG_E3_MIXED_BIG     4096u
#define CFG_E3_MIXED_BIG_P   0.03    /* 3% 大包：10240*3%≈307 落在 (256,2048) ⇒ 阈值路径先触发（非紧急） */
#define CFG_E3_JITTER        0.0
#define CFG_E3_NAK_MODE      0u       /* 0=SR 1=GBN 2=混合 */
#define CFG_E3_NAK_RATE      0.02
#define CFG_E3_LATENCY_PROBE 0u

/* ==================== F. 模拟器 ==================== */
#define CFG_SIM_LOSS_MODE     0u       /* 0=uniform 1=Gilbert-Elliott */
#define CFG_SIM_LOSS_RATE     0.02
#define CFG_SIM_BURST_GOOD_P  0.99
#define CFG_SIM_BURST_BAD_P   0.98
#define CFG_SIM_NAK_DELAY     2u
#define CFG_SIM_SEED          42u

/* ==================== cfg_t ==================== */

typedef struct { uint32_t epochs, pkt_len; } cfg_phase_t;

typedef struct {
    /* ---- 机制 ---- */
    uint32_t ring_n;
    uint32_t sc_new[CFG_SC_NEW_N];
    uint32_t n_sc_new;
    uint32_t sc_old_tiered[CFG_SC_OLD_TIERED_N];
    uint32_t n_sc_old;
    uint32_t hdr_sz;
    uint32_t ovf_cap;
    uint32_t ovf_thresh;
    uint32_t fallback_mtu;
    uint32_t k_dwell;
    uint32_t j_quiet;
    uint32_t hist_decay;
    uint32_t _pad0;              /* 显式对齐：保证 cfg_t 无隐式 padding（memcmp 往返自检） */
    double   shrink_gamma;
    uint32_t adaptive_enable;
    uint32_t single_thread;
    uint32_t pool_use_mmap;
    uint32_t order_guard;
    uint32_t sim_link_gbps;
    uint32_t sim_pend_cap;
    /* ---- 计时/复现 ---- */
    uint32_t seed;
    uint32_t reps;
    uint32_t tsc_batch_ops;
    uint32_t warmup_ops;
    uint32_t rdtsc_xval_ops;
    /* ---- 实验一 ---- */
    uint32_t e1a_n;
    uint32_t e1a_payload_list[CFG_E1A_PAYLOAD_N];
    uint32_t e1a_payload_n;
    uint32_t e1a_timed_ops;
    uint32_t e1a_batch_ops;
    uint32_t e1a_batch_xval_ops;
    uint32_t e1a_seq_mode;
    uint32_t e1b_n_list[CFG_E1B_N_N];
    uint32_t e1b_n_n;
    uint32_t e1b_gbn_long;
    uint32_t e1b_gbn_short;
    uint32_t e1b_sr_k1;
    uint32_t e1b_sr_k2;
    uint32_t e1b_queries;
    uint32_t e1b_payload_list[CFG_E1B_PAYLOAD_N];
    uint32_t e1b_payload_n;
    uint32_t e1b_batch_ops;
    uint32_t e1b_batch_xval_ops;
    /* ---- 实验二 ---- */
    uint32_t e2_pktsize_list[CFG_E2_PKTSIZE_N];
    uint32_t e2_pktsize_n;
    uint32_t e2_n_list[CFG_E2_N_N];
    uint32_t e2_n_n;
    uint32_t e2_n;
    uint32_t e2_fixed_l;
    uint32_t hash_nbuckets;
    uint32_t fixed_block;
    /* ---- 实验三 ---- */
    uint32_t e3_phase_n;
    cfg_phase_t e3_phases[16];
    uint32_t e3_mixed_small;
    uint32_t e3_mixed_big;
    uint32_t _pad1;
    double   e3_mixed_big_p;
    double   e3_jitter;
    double   e3_nak_rate;
    uint32_t e3_nak_mode;
    uint32_t e3_latency_probe;
    /* ---- 模拟器 ---- */
    uint32_t sim_loss_mode;
    uint32_t sim_nak_delay;
    uint32_t sim_seed;
    uint32_t _pad2;
    double   sim_loss_rate;
    double   sim_burst_good_p;
    double   sim_burst_bad_p;
    /* ---- 运行期 ---- */
    char     out_dir[256];       /* 落盘目录（写入 resolved_config.json 便于复现） */
} cfg_t;

_Static_assert(sizeof(cfg_t) % 8 == 0, "cfg_t 必须 8 字节对齐（memcmp 往返自检的前提）");

/* ---- API ---- */
void     cfg_default(cfg_t *c);
int      cfg_override_cli(cfg_t *c, int argc, char **argv);
int      cfg_dump_json(const cfg_t *c, const char *path);  /* 返回落盘字段数，-1 失败 */
int      cfg_load_json(cfg_t *c, const char *path);        /* 返回解析字段数，-1 失败 */
int      cfg_selftest(const cfg_t *c, const char *path);   /* 0=通过 */

#endif /* CONFIG_H */

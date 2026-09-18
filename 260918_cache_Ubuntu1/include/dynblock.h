/*
 * include/dynblock.h — psn_dynblock 数据模型 + 接口
 * ------------------------------------------------------------------
 * 结构体逐字来自 §P2（照抄），并按裁决修订：
 *   ③ slot_meta_t 自然 12 B（不加 pad），一律 sizeof() 不得硬编码；
 *   ④ 溢出 = 每项 malloc 变长块（删 arena）；新增 n_ovf_alloc/n_ovf_malloc；
 *   ⑥ p0 ≡ 0（常量，不进结构体）；删 window_base；next_psn 保留单调插入计数；
 *      phi(psn) = (psn & 0xFFFFFFu) % N（24-bit 归一化）；
 *   ⑦ mem_block_header 逐字沿用 260912（24 B）+ hdr_set（stamp 真写）；
 *   ② 锁：全程单线程（cfg.single_thread 默认 1），编译为 no-op，不计入被测延迟；
 *   1A order_guard 计数 n_ooo_drop；
 *   时间兜底已移除（无 now_ns/vtime/last_check）。
 *
 * 注意（一处必要偏离）：pool_t 增加了 uint8_t use_mmap —— pool_free 必须知道
 * 内存是 mmap 还是 malloc 才能正确释放（裁决 ⑪ 要求 mmap 失败回落 malloc）。
 */
#ifndef DYNBLOCK_H
#define DYNBLOCK_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <assert.h>
#include "config.h"
#include "util.h"    /* align16 */

/* ---- 数据包头（与 260912 一致，24 B） ---- */
struct mem_block_header {
    int32_t  data_len;    /* +0  */
    uint32_t _pad0;       /* +4  */
    uint64_t recv_stamp;  /* +8  单调递增包序号，真写 */
    uint32_t psn;         /* +16 */
    uint32_t _pad1;       /* +20 */
};  /* 24 B */
_Static_assert(sizeof(struct mem_block_header) == 24, "hdr must be 24 B");

#define HDR_SZ ((uint32_t)sizeof(struct mem_block_header))

static inline void hdr_set(void *blk, uint16_t len, uint32_t psn, uint64_t stamp) {
    struct mem_block_header *h = (struct mem_block_header *)blk;
    h->data_len = len; h->psn = psn; h->recv_stamp = stamp;
}

/* 由 payload 指针取回其块头（spec 里的 hdr(dst)）。 */
static inline struct mem_block_header *hdr_of(uint8_t *payload) {
    return (struct mem_block_header *)(payload - HDR_SZ);
}

/* ---- 元数据（自然 12 B，不加 pad） ---- */
enum { SLOT_FREE = 0, SLOT_IN_RING = 1, SLOT_IN_OVF = 2 };

typedef struct {
    uint32_t psn;      /* 槽内当前 PSN（可服务性校验用） */
    uint16_t len;
    uint8_t  flag;     /* FREE / IN_RING / IN_OVF */
    uint8_t  gen;      /* 属于哪一代池 */
    uint32_t ovf_idx;
} slot_meta_t;         /* sizeof = 12；一律 sizeof()，不得硬编码 */

/* ---- 池（一代） ---- */
typedef struct {
    uint8_t *base;
    uint32_t N, S, stride; /* N 一并存下：pool_free 用 N*stride 回推 munmap 长度 */
    uint8_t  gen;
    uint8_t  use_mmap;     /* 偏离 §P2：pool_free 需据此选 munmap 还是 free */
} pool_t;

/* ---- 溢出项（每项 malloc 变长块，无 arena） ---- */
typedef struct {
    uint32_t psn;
    uint16_t len;
    uint8_t  used, pad;
    uint8_t *blk;      /* 块基址（malloc 返回值），与环路径 slot() 同构 */
} ovf_entry_t;

/* 溢出块 payload = 块基址 + HDR_SZ（与 payload_of(pool,i) 同构；全库唯一指针约定） */
static inline uint8_t *ovf_payload(const ovf_entry_t *e) { return e->blk + HDR_SZ; }

/* ---- 锁：no-op（单线程；论文不声称并发性能，锁不计入被测延迟） ---- */
typedef struct { int _unused; } spin_t;
static inline void spin_lock(spin_t *lk)   { (void)lk; }
static inline void spin_unlock(spin_t *lk) { (void)lk; }

/* ---- resize 事件（实验三 resize_events.csv 数据源；dynblock 记录，模拟器补 epoch 号） ---- */
enum { REV_NONE = 0, REV_GROW = 1, REV_SHRINK = 2, REV_OVF_GROW = 3 };
#define CFG_REV_LOG_MAX 128u
typedef struct {
    uint32_t epoch;        /* 由模拟器回填（dynblock 不知道纪元号） */
    uint32_t S_old, S_new;
    uint32_t reason;       /* REV_GROW / REV_SHRINK / REV_OVF_GROW */
    uint32_t moved_pkts;   /* 本次 drain 搬回的溢出包数（shrink 恒 0） */
    uint64_t resize_ns;    /* gen_switch（pool_alloc + 指针切换）耗时 */
    uint64_t drain_ns;     /* drain_overflow 耗时 */
} resize_event_t;

/* ---- 连接（psn_dynblock 全部状态） ---- */
typedef struct {
    uint32_t N, S;             /* 无 next_psn（裁决 B-4）：映射用包自身 psn，淘汰是位置淘汰 */
    pool_t   cur;
    pool_t   old; int has_old;
    uint32_t cur_live, old_live;
    slot_meta_t *meta;         /* N 项，堆分配 */
    ovf_entry_t *ovf;          /* cfg.ovf_cap 项 */
    uint32_t ovf_count, ovf_max_hist;
    uint32_t epoch_stores, epoch_ring_max, quiet_epochs;
    uint32_t grow_streak, shrink_streak;   /* 非负纪元计数，与 k_dwell 同型避免符号比较告警 */
    uint64_t n_store, n_lookup, n_resize, n_drain, n_evict, n_drop, n_ovf_ins;
    uint64_t n_ovf_alloc, n_ovf_malloc; /* ④ 溢出按次计数 */
    uint64_t n_ooo_drop;               /* 1A order_guard 丢弃计数 */
    uint64_t n_drain_residual;         /* ③ drain 时因 len>S（S 封顶 SC_MAX）留在溢出区的项数；语义=最近一次 drain（每次重置） */
    uint64_t n_drain_skip;             /* ② drain 目标槽校验失败跳过的项数（不覆盖原槽；累计） */
    const cfg_t *cfg;
    spin_t   lk;
    /* ---- 实验三观测：resize 事件日志 + 分段计时（只在罕见 resize 路径写，不影响 store 热路径） ---- */
    uint32_t n_rev;
    resize_event_t rev[CFG_REV_LOG_MAX];
    uint32_t last_epoch_ring_max;   /* 最近一个已完结纪元的环内最大包长（S_trace 的 L_ring） */
    uint32_t last_epoch_ovf_count;  /* 最近一个已完结纪元末（drain 前）的溢出条数 */
    uint32_t last_epoch_L_ovf;      /* 最近一个已完结纪元末（drain 前）的溢出最大包长 */
    uint64_t check_ns;              /* 最近一次 check_and_maybe_resize 扫描+评估耗时（ns） */
    uint64_t resize_ns;             /* 最近一次 gen_switch 耗时（ns；0=本纪元无切换） */
    uint64_t drain_ns;              /* 最近一次 drain_overflow 耗时（ns；0=本纪元无 drain） */
} conn_t;

/* ---- 模拟器（§P2 照抄） ---- */
typedef struct {
    const cfg_t *cfg;
    uint32_t psn, phase_idx, phase_epochs_run, epoch_in_phase;
    int preset_mixed; uint64_t rng, total_pkts;
} traffic_t;

typedef struct { const cfg_t *cfg; uint64_t rng; int bad_state; } loss_t;

typedef struct {
    const cfg_t *cfg;
    uint32_t *pend; uint32_t n_pend, cap;
    uint32_t first_lost_psn;
    uint64_t rng, nak_n;
} nakgen_t;

typedef struct {
    uint64_t nak_n, nak_ns_total, bytes_retrans, hit, miss, pend_overflow;
} sim_stats_t;

typedef struct {
    FILE *f_epoch;     /* S_trace.csv           */
    FILE *f_util;      /* utilization_trace.csv */
    FILE *f_ovf;       /* overflow_trace.csv    */
    FILE *f_lat;       /* latency_trace.csv     */
    FILE *f_overhead;  /* overhead.csv          */
    FILE *f_resize;    /* resize_events.csv     */
    FILE *f_metrics;   /* metrics.csv（含 elapsed_us，唯一读 sim_link_gbps 的列） */
    uint32_t epoch;
} trace_writer_t;

/* ---- 定位 / 寻址（p0 ≡ 0；psn 24-bit 归一化） ---- */
static inline uint32_t phi(const conn_t *c, uint32_t psn) {
    return (psn & 0xFFFFFFu) % c->N;
}
static inline uint32_t stride_of(uint32_t S) {
    return align16(HDR_SZ + S);
}
static inline uint8_t *slot(const pool_t *p, uint32_t i) {
    return p->base + (size_t)i * p->stride;
}
static inline uint8_t *payload_of(const pool_t *p, uint32_t i) {
    return slot(p, i) + HDR_SZ;
}
static inline pool_t *pool_for(conn_t *c, uint8_t gen) {
    assert(gen == c->cur.gen || (c->has_old && gen == c->old.gen)); /* C-9：gen 必须合法 */
    return (gen == c->cur.gen) ? &c->cur : &c->old;
}

/* 1A order_guard：a 是否严格新于 b（24-bit 模序）。a==b 返回 0。 */
static inline int psn_newer(uint32_t a, uint32_t b) {
    uint32_t d = (a - b) & 0xFFFFFFu;
    return d != 0 && d < 0x800000u;
}

/* ---- 接口：pool.c ---- */
int      pool_alloc(pool_t *p, uint32_t N, uint32_t S, int prefer_mmap); /* prefer_mmap=偏好；结果看 p->use_mmap */
void     pool_free(pool_t *p);
void     ovf_reset(conn_t *c);
int      ovf_alloc(conn_t *c, uint32_t psn, const uint8_t *payload, uint16_t len);
void     ovf_release(conn_t *c, uint32_t ovf_idx);
uint32_t ovf_scan_max(const conn_t *c);

/* ---- 接口：dynblock.c ---- */
enum { R_OK = 0, R_DROPPED = -1 };   /* conn_store 返回值 */

void           conn_init(conn_t *c, const cfg_t *cfg, uint32_t mtu_from_cm);
void           conn_destroy(conn_t *c);
int            conn_store(conn_t *c, uint32_t psn, const uint8_t *payload, uint16_t len);
const uint8_t *conn_lookup(conn_t *c, uint32_t psn, uint16_t *out_len);
int            conn_retransmit_range(conn_t *c, uint32_t p_start, uint32_t L_req,
                                     uint8_t *out, uint32_t out_cap);        /* GBN */
int            conn_retransmit_set(conn_t *c, const uint32_t *psns, uint32_t n,
                                   uint8_t *out, uint32_t out_cap);          /* SR */
void           conn_force_check(conn_t *c); /* 管理面手动重评估（无时钟） */

#endif /* DYNBLOCK_H */

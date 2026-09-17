/*
 * baselines/baseline.h — 统一基线接口（fifo / chained_hash / balanced_tree + *_bounded + dynblock 适配）
 * ---------------------------------------------------------------------------------------------------
 * 语义与 260912 psn_behavior_bench/behavior_bench.c 逐字段一致（出处见 docs/baseline_param_provenance.md）。
 *
 * 节点结构（24B 头沿用 include/dynblock.h 的 struct mem_block_header，与旧 behavior_bench.c:55-59 同构）：
 *   fifo_node = next(8) + hdr(24)                     = 32 B + payload
 *   hash_node = next(8) + hdr(24)                     = 32 B + payload
 *   avl_node  = l(8)+r(8)+p(8)+h(4) + hdr(24)         = 56 B + payload
 * malloc 请求尺寸 = sizeof(node) + payload_len（与 docs/R1_warmup_coverage.md §3 表一致）。
 *
 * 三种实现口径：
 *   无界基线（真实基线参考）  —— 每 store 一次 malloc、不 free、无界增长（D2）。
 *   *_bounded（对照 D）       —— 预分配空闲链表 + 容量 N + 滑动淘汰（活集恒 N ⇒ hash 负载恒 N/nbuckets）。
 *   psn_dynblock（我们）      —— 有界窗口 + 预分配 mmap 池 + 位置淘汰（原生，见 dynblock.c）。
 */
#ifndef BASELINE_H
#define BASELINE_H

#include <stdint.h>
#include <stddef.h>
#include "dynblock.h"   /* struct mem_block_header + conn_t（适配用） */

typedef struct b_cache b_cache_t;

typedef struct {
    /* store：把 (psn, payload[0..len)) 存入缓存（含索引插入 + 必要淘汰）。 */
    void  (*store)(b_cache_t *c, uint32_t psn, const uint8_t *payload, uint32_t len);
    /* retrieve：按 psn 取 payload 指针（+out_len）。miss 返回 NULL。 */
    const uint8_t *(*retrieve)(b_cache_t *c, uint32_t psn, uint32_t *out_len);
    /* retrieve_range：GBN 连续区间 [start, start+count)。返回写出字节数（定位+memcpy 全过程）。 */
    uint32_t (*retrieve_range)(b_cache_t *c, uint32_t start, uint32_t count,
                               uint8_t *out, uint32_t out_cap);
    /* retrieve_set：SR 离散 PSN 集。返回写出字节数。 */
    uint32_t (*retrieve_set)(b_cache_t *c, const uint32_t *psns, uint32_t n,
                             uint8_t *out, uint32_t out_cap);
    void  (*destroy)(b_cache_t *c);
} b_ops_t;

struct b_cache {
    const char *name;
    b_ops_t     ops;
    uint64_t    n_malloc;  /* store 路径 malloc 调用次数（malloc_per_store 证据：我们/`*_bounded` 恒 0） */
    uint64_t    n_cmp;     /* retrieve 比较次数（诊断；exp1b 的第三项指标「每次取包比较次数」） */
    uint64_t    n_resize;  /* store 路径 resize 次数（exp1a 断言 dynblock 在 adaptive_enable=0 下恒 0） */
    uint64_t    n_live;    /* 当前驻留条数（诊断；smoke 断言 *_bounded 全程 resident==N） */
};

/* ---- 无界基线（真实基线参考） ---- */
b_cache_t *make_fifo(void);
b_cache_t *make_chained_hash(uint32_t nbuckets);
b_cache_t *make_balanced_tree(void);

/* ---- *_bounded（对照 D：预分配池 + 容量 N + 滑动淘汰） ---- */
b_cache_t *make_fifo_bounded(uint32_t capacity, uint32_t payload_len);
b_cache_t *make_chained_hash_bounded(uint32_t capacity, uint32_t nbuckets, uint32_t payload_len);
b_cache_t *make_balanced_tree_bounded(uint32_t capacity, uint32_t payload_len);

/* ---- psn_dynblock 适配器（我们的方法） ---- */
b_cache_t *make_dynblock(const cfg_t *cfg, uint32_t mtu_from_cm);
/* 读 dynblock 当前块大小 S（exp1a 收敛阶段记录「收敛后的 S」用；非 dynblock 返回 0）。 */
uint32_t dynblock_cur_S(const b_cache_t *bc);

/* ---- 对照 E：index_only（只算 Φ + 一次槽写/读，不拷贝 payload） ----
 * 给出位置映射的不可约成本（映射 + 槽命中），回答「取模不是免费的」。n_cmp=0。 */
b_cache_t *make_index_only(uint32_t N);

/* ---- 测试辅助 ---- */
/* 校验 tree_bounded 的 AVL 不变式（中序严格升序 + |BF|≤1）。返回 0=通过，-1=违反。
 * 仅对 balanced_tree_bounded 有意义（其它方法直接返回 0）。 */
int baseline_tree_verify(b_cache_t *bc);

/* exp2 空间利用率：返回该缓存为容纳满窗所需的总分配字节（sizeof 实测）。
 * 无界基线返回 0（每 store malloc，不参与 exp2）。 */
uint64_t cache_footprint_bytes(const b_cache_t *bc);

#endif /* BASELINE_H */

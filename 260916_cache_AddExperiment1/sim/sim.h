/*
 * sim/sim.h — 单机模拟器（发包/丢包/发 NAK/驱动）对外接口
 * ------------------------------------------------------------------
 * 三个模块严格按 spec §4：
 *   traffic.c 按阶段表生成 PSN 递增的数据包；
 *   loss.c    判定丢包（uniform / Gilbert-Elliott）；
 *   nak.c     按 NAK_MODE 组装 GBN 区间或 SR 集合，延迟 NAK_DELAY 后提交网关；
 *   sim_main.c 串联三者 + conn_store / conn_lookup + 落盘全部 trace。
 *
 * 语义（spec §4.3 澄清，必须保持）：丢包发生在网关下游——网关已缓存该包，
 * 故「每个包都 conn_store，无论丢不丢；只有丢的包进 NAK」。
 *
 * 随机：与 util 的 xorshift32 同参数（13,17,5），seed = cfg.sim_seed。
 *       sim_link_gbps 机制侧绝不读取，只进 metrics.csv 的 elapsed_us 列。
 */
#ifndef SIM_H
#define SIM_H

#include "dynblock.h"

/* ---- 随机（xorshift32，13,17,5；seed 存于各结构体的 uint64_t rng 低 32 位） ---- */
static inline uint32_t sim_rng_u32(uint64_t *s) {
    uint32_t x = (uint32_t)*s;
    if (!x) x = 1u;                 /* 防 seed=0 卡死（与 util xorshift32_seed 同规） */
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

static inline double rng_double(uint64_t *s) {
    return (double)(sim_rng_u32(s) >> 8) / 16777216.0;   /* 24-bit uniform [0,1) */
}

/* ---- 发包器（sim/traffic.c） ---- */
void     traffic_init(traffic_t *t, const cfg_t *cfg, int preset_mixed);
uint32_t traffic_next(traffic_t *t, uint32_t *pkt_len);  /* 返回 psn；结束返回 UINT32_MAX */

/* ---- 丢包（sim/loss.c） ---- */
void loss_init(loss_t *l, const cfg_t *cfg);
int  loss_is_lost(loss_t *l, uint32_t psn);

/* ---- NAK（sim/nak.c） ---- */
void nak_init(nakgen_t *g, const cfg_t *cfg);
void nak_on_packet(nakgen_t *g, const cfg_t *cfg, uint32_t psn, int lost,
                   conn_t *gw, uint8_t *outbuf, uint32_t outcap, sim_stats_t *st);

/* ---- 驱动（sim/sim_main.c） ---- */
uint32_t phase_for_epoch(const cfg_t *cfg, uint32_t epoch);   /* 纪元号 → 阶段号 */
int  sim_run(const cfg_t *cfg, sim_stats_t *st, trace_writer_t *tw);

#endif /* SIM_H */

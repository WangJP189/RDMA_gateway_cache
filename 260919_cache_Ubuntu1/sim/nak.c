/*
 * sim/nak.c — 发 NAK：延迟 NAK_DELAY 后把丢失的 PSN 汇总成 GBN 区间或 SR 集合（spec §4.4）
 * ------------------------------------------------------------------
 * 关键统计（实验三取包数据来源）：
 *   st->nak_n         NAK 次数
 *   st->nak_ns_total  处理所有 NAK 的总耗时（ns）
 *   st->bytes_retrans 重传字节数
 *   st->hit / miss    请求 PSN 是否还在缓存（conn_lookup 返回 NULL 即 miss）
 *
 * 计时纪律：本机（VM）每次读钟（rdtsc / clock_gettime）有 ~μs 级固定开销，
 * 故单次 lookup 不做逐次 rdtsc（那样测到的是读钟开销，不是 lookup）。逐 lookup
 * 的延迟见 sim_main.c 的批量摊销基准（latency_trace 数据源）。这里只计时整次 NAK。
 */
#include "sim.h"

#include <stdlib.h>
#include <string.h>

void nak_init(nakgen_t *g, const cfg_t *cfg) {
    memset(g, 0, sizeof(*g));
    g->cfg = cfg;
    g->cap = cfg->sim_pend_cap;
    g->pend = (uint32_t *)malloc((size_t)g->cap * sizeof(uint32_t));
    g->rng = cfg->sim_seed;   /* 0 由 sim_rng_u32 兜底 */
    /* pend malloc 失败：cap 保持但 pend==NULL；push 有判空防御，正常到不了 */
}

static void nak_push(nakgen_t *g, uint32_t psn) {
    if (!g->pend || g->n_pend >= g->cap) return;
    g->pend[g->n_pend++] = psn;
}

/* 与 conn_retransmit_set 等价，额外统计 hit/miss。 */
static uint32_t retransmit_set(conn_t *gw, const uint32_t *psns, uint32_t n,
                               uint8_t *out, uint32_t outcap, sim_stats_t *st) {
    uint32_t w = 0;
    for (uint32_t k = 0; k < n; k++) {
        uint16_t len = 0;
        const uint8_t *p = conn_lookup(gw, psns[k], &len);
        if (p) {
            st->hit++;
            if (w + len <= outcap) { memcpy(out + w, p, len); w += len; }
        } else st->miss++;
    }
    return w;
}

/* 与 conn_retransmit_range 等价（GBN：连续区间），额外统计 hit/miss。 */
static uint32_t retransmit_range(conn_t *gw, uint32_t p_start, uint32_t L_req,
                                 uint8_t *out, uint32_t outcap, sim_stats_t *st) {
    uint32_t w = 0;
    for (uint32_t k = 0; k < L_req; k++) {
        uint16_t len = 0;
        const uint8_t *p = conn_lookup(gw, p_start + k, &len);
        if (p) {
            st->hit++;
            if (w + len <= outcap) { memcpy(out + w, p, len); w += len; }
        } else st->miss++;
    }
    return w;
}

static uint32_t pend_min(const nakgen_t *g) {
    uint32_t lo = UINT32_MAX;
    for (uint32_t k = 0; k < g->n_pend; k++)
        if (g->pend[k] < lo) lo = g->pend[k];
    return lo;
}

static uint32_t pend_max(const nakgen_t *g) {
    uint32_t hi = 0;
    for (uint32_t k = 0; k < g->n_pend; k++)
        if (g->pend[k] > hi) hi = g->pend[k];
    return hi;
}

void nak_on_packet(nakgen_t *g, const cfg_t *cfg, uint32_t psn, int lost,
                   conn_t *gw, uint8_t *outbuf, uint32_t outcap, sim_stats_t *st) {
    if (lost) {
        if (g->n_pend == 0) g->first_lost_psn = psn;
        uint32_t before = g->n_pend;
        nak_push(g, psn);
        if (before == g->n_pend && g->n_pend == g->cap) st->pend_overflow++;
    }
    if (g->n_pend == 0) return;
    if (psn - g->first_lost_psn < cfg->sim_nak_delay) return;   /* 延迟未到 */

    const uint64_t t0 = rdtsc_raw();
    uint32_t w = 0;
    if (cfg->e3_nak_mode == 0) {                     /* SR：离散集合 */
        w = retransmit_set(gw, g->pend, g->n_pend, outbuf, outcap, st);
    } else if (cfg->e3_nak_mode == 1) {              /* GBN：连续区间 */
        uint32_t lo = pend_min(g), hi = pend_max(g);
        w = retransmit_range(gw, lo, hi - lo + 1, outbuf, outcap, st);
    } else {                                         /* 混合：随机 */
        if (rng_double(&g->rng) < 0.5) {
            w = retransmit_set(gw, g->pend, g->n_pend, outbuf, outcap, st);
        } else {
            uint32_t lo = pend_min(g), hi = pend_max(g);
            w = retransmit_range(gw, lo, hi - lo + 1, outbuf, outcap, st);
        }
    }
    const uint64_t t1 = rdtsc_raw();

    st->nak_n++;
    st->nak_ns_total += (uint64_t)to_ns(t1 - t0);
    st->bytes_retrans += w;
    g->n_pend = 0;   /* reset_pending */
}

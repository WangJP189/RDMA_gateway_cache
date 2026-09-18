/*
 * sim/traffic.c — 发包器：按阶段表生成 PSN 递增的数据包（spec §4.2）
 * ------------------------------------------------------------------
 * 阶段边界按「纪元」（= N 个包）计，与网关的检查周期严格对齐。
 * pkt_len==0 表示「混合分布」；e3_jitter>0 时对包长加 ±jitter 抖动（默认 0）。
 */
#include "sim.h"

#include <string.h>

void traffic_init(traffic_t *t, const cfg_t *cfg, int preset_mixed) {
    memset(t, 0, sizeof(*t));
    t->cfg = cfg;
    t->preset_mixed = preset_mixed;   /* 记录在案；实际混合判定走 pkt_len==0（与 spec 一致） */
    t->rng = cfg->sim_seed;           /* 0 由 sim_rng_u32 兜底 */
}

uint32_t traffic_next(traffic_t *t, uint32_t *pkt_len) {
    if (t->phase_idx >= t->cfg->e3_phase_n) return UINT32_MAX;   /* 阶段表跑完 */
    const uint32_t len    = t->cfg->e3_phases[t->phase_idx].pkt_len;
    const uint32_t epochs = t->cfg->e3_phases[t->phase_idx].epochs;

    uint32_t L = len;
    if (L == 0) {   /* 0 = 混合分布（备用负载 --e3-preset=mixed） */
        const double u = rng_double(&t->rng);
        L = (u < t->cfg->e3_mixed_big_p) ? t->cfg->e3_mixed_big
                                         : t->cfg->e3_mixed_small;
    }
    if (t->cfg->e3_jitter > 0.0) {          /* 可选：包长抖动 */
        const double j = 1.0 + (rng_double(&t->rng) * 2.0 - 1.0) * t->cfg->e3_jitter;
        L = (uint32_t)((double)L * j);
        if (L < 64) L = 64;
        if (L > t->cfg->sc_new[t->cfg->n_sc_new - 1]) L = t->cfg->sc_new[t->cfg->n_sc_new - 1];
    }

    t->psn++;
    if (++t->epoch_in_phase >= t->cfg->ring_n) {   /* 满一个纪元 */
        t->epoch_in_phase = 0;
        if (++t->phase_epochs_run >= epochs) {      /* 该阶段已跑够纪元数 */
            t->phase_idx++;
            t->phase_epochs_run = 0;
        }
    }
    t->total_pkts++;
    *pkt_len = L;
    return t->psn - 1;                      /* PSN 严格递增，从 0 起 */
}

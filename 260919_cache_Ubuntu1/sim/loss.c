/*
 * sim/loss.c — 丢包模型：uniform / Gilbert-Elliott（spec §4.3）
 * ------------------------------------------------------------------
 * 语义（spec §4.3 澄清）：丢包发生在网关下游；网关已缓存该包，故调用方
 * 必须对每个包都 conn_store，只有 loss_is_lost==1 的包才进 NAK。
 */
#include "sim.h"

#include <string.h>

void loss_init(loss_t *l, const cfg_t *cfg) {
    memset(l, 0, sizeof(*l));
    l->cfg = cfg;
    l->rng = cfg->sim_seed;   /* 0 由 sim_rng_u32 兜底 */
}

int loss_is_lost(loss_t *l, uint32_t psn) {
    (void)psn;
    if (l->cfg->sim_loss_mode == 0)                       /* uniform：独立同分布 */
        return rng_double(&l->rng) < l->cfg->sim_loss_rate;

    /* Gilbert-Elliott：两状态马尔可夫链（坏状态全丢 → 模拟 WAN 突发） */
    const double p = l->bad_state ? l->cfg->sim_burst_bad_p
                                  : l->cfg->sim_burst_good_p;
    if (rng_double(&l->rng) >= p) l->bad_state ^= 1;
    return l->bad_state;
}

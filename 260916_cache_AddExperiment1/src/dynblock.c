/*
 * src/dynblock.c — psn_dynblock 算法体（建联 / store / lookup / 自适应 resize）
 * ------------------------------------------------------------------
 * 严格按 spec §2.4–§2.11 伪代码，并按第 2 轮裁决修订：
 *   B-4  删 next_psn：映射用包自身 psn、淘汰是位置淘汰、stamp=psn；纪元边界只认 epoch_stores>=N；
 *   B-5  emergency_grow 改 grow_locked（假定调用方已持锁，不 unlock/relock）；
 *   B-6  n_resize 只计「真正发生的切换」（gen_switch 延后/分配失败不计）；
 *   B-7  adaptive_enable==0 → 彻底不调 check，且不 emergency_grow（溢出满即 drop），热路径零额外开销；
 *   B-8  旧代释放只靠 O(1) cur_live/old_live 计数；gen_switch: old_live=cur_live, cur_live=0，不扫数组；
 *   C-9  pool_for 断言 gen 合法；C-10 drain 断言不遇旧代环包；
 *   1A   order_guard 检查在 evict_occupant 之前；丢弃仍推进 epoch_stores 并计 n_ooo_drop。
 *   2A   drain 目标槽校验（选项 a：与 gen_switch 同临界区，新池空 ⇒ 槽必为本项 IN_OVF，assert）；
 *   3A   drain 只搬 len<=S 的项，len>S（S 封顶 SC_MAX）留溢出区，计 n_drain_residual。
 *
 * 溢出路径是「每项 malloc 变长块」（裁决 ④，无 arena）；环路径稳态 malloc==0（不变量 I3）。
 */
#include "dynblock.h"

#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* 前向声明（static 函数先声明后使用） */
static void check_and_maybe_resize(conn_t *c);
static int  gen_switch(conn_t *c, uint32_t S_new);
static void release_old_if_drained(conn_t *c);
static uint32_t drain_overflow(conn_t *c);
static void grow_locked(conn_t *c, uint32_t need);
static void rev_log(conn_t *c, uint32_t S_old, uint32_t S_new, uint32_t reason,
                    uint32_t moved, uint64_t resize_ns, uint64_t drain_ns);

/* ==================== 建联 / 销毁 ==================== */

void conn_init(conn_t *c, const cfg_t *cfg, uint32_t mtu_from_cm) {
    memset(c, 0, sizeof(*c));
    c->cfg = cfg;
    c->N = cfg->ring_n;
    c->S = ceil_class(cfg, mtu_from_cm ? mtu_from_cm : cfg->fallback_mtu);

    c->cur.gen = 0;
    pool_alloc(&c->cur, c->N, c->S, cfg->pool_use_mmap); /* MAP_NORESERVE 几乎必成；失败则 base=NULL */

    c->meta = (slot_meta_t *)calloc(c->N, sizeof(slot_meta_t));        /* 全 FREE */
    c->ovf  = (ovf_entry_t *)calloc(cfg->ovf_cap, sizeof(ovf_entry_t));
    /* 其余字段已被 memset 清零：has_old/cur_live/old_live/ovf_count/…/全部计数器 */
}

void conn_destroy(conn_t *c) {
    if (!c->cfg) return;                        /* 幂等：未初始化或已销毁 */
    for (uint32_t oi = 0; oi < c->cfg->ovf_cap; oi++) {
        if (c->ovf[oi].used) {
            free(c->ovf[oi].blk);               /* blk 是块基址，直接 free */
            c->ovf[oi].used = 0; c->ovf[oi].blk = NULL;
        }
    }
    free(c->ovf);  c->ovf = NULL;
    free(c->meta); c->meta = NULL;
    pool_free(&c->cur);
    if (c->has_old) pool_free(&c->old);
    c->has_old = 0;
    c->cur_live = c->old_live = 0;
    c->cfg = NULL;
}

/* ==================== 淘汰（环回绕即淘汰，O(1)） ==================== */

static void evict_occupant(conn_t *c, uint32_t i) {
    slot_meta_t *m = &c->meta[i];
    if (m->flag == SLOT_FREE) return;
    if (m->flag == SLOT_IN_RING) {
        if (m->gen == c->cur.gen) c->cur_live--; else c->old_live--;
    } else {                                   /* 溢出项需显式回收（下标不被索引覆盖） */
        ovf_release(c, m->ovf_idx);
    }
    m->flag = SLOT_FREE;
    c->n_evict++;
}

/* ==================== store ==================== */

int conn_store(conn_t *c, uint32_t psn, const uint8_t *payload, uint16_t len) {
    spin_lock(&c->lk);
    uint32_t i = phi(c, psn);
    slot_meta_t *m = &c->meta[i];
    int ret = R_OK;

    /* 1A order_guard：旧包晚到 → 丢弃 incoming，不回收槽内 occupant */
    if (c->cfg->order_guard && m->flag != SLOT_FREE && psn_newer(m->psn, psn)) {
        c->n_ooo_drop++;
        ret = R_DROPPED;
        goto done;
    }

    evict_occupant(c, i);

    if (len <= c->S) {                          /* 正常路径：进环 */
        uint8_t *dst = payload_of(&c->cur, i);
        memcpy(dst, payload, len);
        hdr_set(slot(&c->cur, i), len, psn, (uint64_t)psn);   /* stamp = psn */
        *m = (slot_meta_t){ .psn = psn, .len = len, .flag = SLOT_IN_RING,
                            .gen = c->cur.gen, .ovf_idx = 0 };
        c->cur_live++;
        if (c->cfg->adaptive_enable && len > c->epoch_ring_max) c->epoch_ring_max = len;
    } else {                                     /* 溢出路径：每项 malloc 变长块 */
        int oi = ovf_alloc(c, psn, payload, len);
        if (oi < 0 && c->cfg->adaptive_enable) {
            grow_locked(c, len);                 /* 调用方已持锁，不 relock */
            oi = ovf_alloc(c, psn, payload, len);
        }
        if (oi < 0) {                            /* 溢出满且无法扩 ⇒ 丢弃（不破坏正确性） */
            c->n_drop++;
            ret = R_DROPPED;
            goto done;
        }
        *m = (slot_meta_t){ .psn = psn, .len = len, .flag = SLOT_IN_OVF,
                            .gen = c->cur.gen, .ovf_idx = (uint32_t)oi };
        c->n_ovf_ins++;
        if (c->cfg->adaptive_enable && len > c->ovf_max_hist) c->ovf_max_hist = len;
    }

done:
    c->n_store++;
    if (c->cfg->adaptive_enable) {
        c->epoch_stores++;                        /* 丢弃路径也推进（B-4：纪元只认此计数） */
        release_old_if_drained(c);
        if (c->epoch_stores >= c->N)
            check_and_maybe_resize(c);            /* 唯一触发（无时间兜底） */
    }
    spin_unlock(&c->lk);
    return ret;
}

/* ==================== lookup / 重传 ==================== */

const uint8_t *conn_lookup(conn_t *c, uint32_t psn, uint16_t *out_len) {
    uint32_t i = phi(c, psn);
    slot_meta_t m = c->meta[i];
    if (m.flag == SLOT_FREE || m.psn != psn) return NULL;   /* 已滑出窗口 */
    c->n_lookup++;
    if (m.flag == SLOT_IN_RING) {
        pool_t *p = pool_for(c, m.gen);
        *out_len = m.len;
        return payload_of(p, i);
    }
    *out_len = c->ovf[m.ovf_idx].len;
    return ovf_payload(&c->ovf[m.ovf_idx]);      /* blk 是块基址，payload = blk+HDR_SZ */
}

int conn_retransmit_range(conn_t *c, uint32_t p_start, uint32_t L_req,
                          uint8_t *out, uint32_t out_cap) {
    uint32_t w = 0;
    for (uint32_t k = 0; k < L_req; k++) {
        uint16_t len; const uint8_t *p = conn_lookup(c, p_start + k, &len);
        if (!p) continue;
        memcpy(out + w, p, len); w += len;
        if (w >= out_cap) break;
    }
    return (int)w;
}

int conn_retransmit_set(conn_t *c, const uint32_t *psns, uint32_t n,
                        uint8_t *out, uint32_t out_cap) {
    uint32_t w = 0;
    for (uint32_t k = 0; k < n; k++) {
        uint16_t len; const uint8_t *p = conn_lookup(c, psns[k], &len);
        if (!p) continue;
        memcpy(out + w, p, len); w += len;
        if (w >= out_cap) break;
    }
    return (int)w;
}

/* ==================== 自适应评估（每环满一圈检查一次） ==================== */

static void check_and_maybe_resize(conn_t *c) {
    const cfg_t *cf = c->cfg;
    uint64_t t0 = rdtsc_raw();                   /* 计时：check 评估（不含切换/drain） */
    uint32_t L_ring  = c->epoch_ring_max;
    uint32_t ovf_cnt = c->ovf_count;
    uint32_t S_old   = c->S;

    /* 实验三观测：快照「刚完结纪元末（drain 前）」的形态，供 S_trace/overflow_trace 落盘。
     * L_ovf 只在规则 A 需要（ceil_class 目标）；稳态/规则 B 不扫溢出数组（O(1) 临界区）。 */
    c->last_epoch_ring_max  = c->epoch_ring_max;
    c->last_epoch_ovf_count = ovf_cnt;
    c->last_epoch_L_ovf     = 0;                 /* 仅规则 A 触发时填真实值（ovf_scan_max） */
    c->resize_ns = c->drain_ns = 0;              /* 每纪元重置本纪元切换/drain 计时 */

    c->epoch_stores = 0;
    c->epoch_ring_max = 0;
    if (ovf_cnt == 0) c->quiet_epochs++; else c->quiet_epochs = 0;
    if (c->quiet_epochs >= cf->hist_decay) c->ovf_max_hist = 0;

    /* ===== 规则 A：溢出过多 ⇒ 块太小 ⇒ 扩 ===== */
    if (ovf_cnt >= cf->ovf_thresh) {
        uint32_t L_ovf = ovf_scan_max(c);        /* 只在规则 A 里扫（O(OVF_CAP)）；稳态零扫描 */
        c->last_epoch_L_ovf = L_ovf;
        c->grow_streak++; c->shrink_streak = 0;
        if (c->grow_streak >= cf->k_dwell) {
            uint32_t S_new = ceil_class(cf, L_ovf ? L_ovf : c->S + 1);
            if (S_new > c->S) {
                uint64_t tr = rdtsc_raw();
                int ok = gen_switch(c, S_new);
                c->resize_ns = (uint64_t)to_ns(rdtsc_raw() - tr);
                if (ok) {
                    uint64_t td = rdtsc_raw();
                    uint32_t moved = drain_overflow(c);
                    c->drain_ns = (uint64_t)to_ns(rdtsc_raw() - td);
                    c->n_resize++;               /* B-6：真正切换才计数 */
                    rev_log(c, S_old, S_new, REV_GROW, moved, c->resize_ns, c->drain_ns);
                }
            }
            c->grow_streak = 0;
        }
        c->check_ns = (uint64_t)to_ns(rdtsc_raw() - t0);
        return;
    }
    c->grow_streak = 0;

    /* ===== 规则 B：环内最大包远小于 S ⇒ 块太大 ⇒ 缩 ===== */
    if (ovf_cnt == 0 && c->quiet_epochs >= cf->j_quiet && L_ring > 0) {
        uint32_t target = ceil_class(cf, L_ring);
        if (target < ceil_class(cf, c->ovf_max_hist))
            target = ceil_class(cf, c->ovf_max_hist);   /* 绝不缩到最近溢出包以下 */
        if (target < S_old && L_ring <= (uint32_t)((double)S_old * cf->shrink_gamma)) {
            c->shrink_streak++;
            if (c->shrink_streak >= cf->k_dwell) {
                uint64_t tr = rdtsc_raw();
                int ok = gen_switch(c, target);
                c->resize_ns = (uint64_t)to_ns(rdtsc_raw() - tr);
                if (ok) {
                    c->n_resize++;
                    rev_log(c, S_old, target, REV_SHRINK, 0, c->resize_ns, 0);
                }
                c->shrink_streak = 0;
            }
        } else c->shrink_streak = 0;
    } else c->shrink_streak = 0;
    c->check_ns = (uint64_t)to_ns(rdtsc_raw() - t0);
}

/* ==================== 切换 / 排空 / drain / 紧急扩 ==================== */

/* 双世代切换（零迁移）。返回 1=真的切换了；0=延后（旧池未排空）或分配失败。
 * gen 是 8-bit，会回绕（np.gen = cur.gen+1）；安全性依赖下面这道前置检查：
 * 切换时旧池必须已排空（old_live==0）⇒ 系统里永远不存在「两代之前」的活包，
 * 故 gen 回绕不会撞到两个同名 gen 的活池。放宽这道检查前，必须重新论证该不变量。 */
static int gen_switch(conn_t *c, uint32_t S_new) {
    if (c->has_old && c->old_live > 0) return 0; /* §2.11-3：旧池未排空 ⇒ 延后，下纪元重试 */
    if (c->has_old) { pool_free(&c->old); c->has_old = 0; }

    pool_t np;
    if (pool_alloc(&np, c->N, S_new, c->cfg->pool_use_mmap) < 0) return 0;
    np.gen = (uint8_t)(c->cur.gen + 1);

    c->old = c->cur; c->has_old = 1; c->old_live = c->cur_live;   /* B-8：O(1) 计数，不扫数组 */
    c->cur = np;      c->cur_live = 0;
    c->S = S_new;
    return 1;
}

static void release_old_if_drained(conn_t *c) {
    if (c->has_old && c->old_live == 0) { pool_free(&c->old); c->has_old = 0; }
}

/* 扩后把溢出包搬回环内。
 * ② 选项(a)：drain 与 gen_switch 在同一临界区完成（check_and_maybe_resize / grow_locked
 *   均在调用方已持锁时被调），新池此刻为空 ⇒ 目标槽不可能已被更新的包占用；故对每个活溢出项，
 *   目标槽必是它自己占的 IN_OVF 槽（psn 匹配），否则即实现错误，直接 assert（不静默跳过）。
 * ③ 不可容纳项（len > S，S 已被 ceil_class 封顶到 SC_MAX）留在溢出区，计 n_drain_residual。 */
static uint32_t drain_overflow(conn_t *c) {
    uint32_t moved = 0, residual = 0, skipped = 0;
    for (uint32_t oi = 0; oi < c->cfg->ovf_cap; oi++) {
        ovf_entry_t *e = &c->ovf[oi];
        if (!e->used) continue;
        uint32_t i = phi(c, e->psn);
        slot_meta_t *m = &c->meta[i];
        /* 目标槽校验（正确性级，严格于 C-10）：只可能是本项自己的 IN_OVF 槽。
         * 断言 + 安全退化双保险：-DNDEBUG 下 assert 被删，仍不覆盖原槽、不计为搬回。 */
        if (!(m->flag == SLOT_IN_OVF && m->psn == e->psn)) {
            assert(0 && "drain: target slot not owned by this overflow entry");
            skipped++;                 /* 不覆盖，保留原槽内容 */
            continue;
        }
        if (e->len > c->S) { residual++; continue; }   /* ③ 超上限项，留在溢出区 */
        uint8_t *dst = payload_of(&c->cur, i);
        memcpy(dst, ovf_payload(e), e->len);
        hdr_set(slot(&c->cur, i), e->len, e->psn, (uint64_t)e->psn);
        m->flag = SLOT_IN_RING; m->gen = c->cur.gen; m->len = e->len;
        c->cur_live++;
        ovf_release(c, oi);
        moved++;
    }
    c->ovf_count = residual + skipped;   /* 只剩超上限项 + 跳过项（通常为 0） */
    c->n_drain += moved;
    c->n_drain_residual = residual;      /* 语义：最近一次 drain 的残留数（每次重置） */
    c->n_drain_skip += skipped;          /* 累计 */
    return moved;                        /* 供调用方记录 resize 事件的搬回包数 */
}

/* 紧急扩（溢出被瞬间打满）。假定调用方已持锁。 */
static void grow_locked(conn_t *c, uint32_t need) {
    uint32_t L_ovf = ovf_scan_max(c);
    uint32_t target = L_ovf > need ? L_ovf : need;  /* 必须覆盖新包与全部现存溢出包 */
    uint32_t S_new = ceil_class(c->cfg, target);
    if (S_new > c->S) {
        uint32_t S_old = c->S;
        uint64_t tr = rdtsc_raw();
        int ok = gen_switch(c, S_new);
        c->resize_ns = (uint64_t)to_ns(rdtsc_raw() - tr);
        if (ok) {
            uint64_t td = rdtsc_raw();
            uint32_t moved = drain_overflow(c);
            c->drain_ns = (uint64_t)to_ns(rdtsc_raw() - td);
            c->n_resize++;
            rev_log(c, S_old, S_new, REV_OVF_GROW, moved, c->resize_ns, c->drain_ns);
        }
    }
}

/* resize 事件日志（实验三 resize_events.csv 数据源）。日志满时丢弃，不影响正确性。 */
static void rev_log(conn_t *c, uint32_t S_old, uint32_t S_new, uint32_t reason,
                    uint32_t moved, uint64_t resize_ns, uint64_t drain_ns) {
    if (c->n_rev >= CFG_REV_LOG_MAX) return;
    resize_event_t *e = &c->rev[c->n_rev++];
    e->epoch = 0;                              /* 模拟器补纪元号 */
    e->S_old = S_old; e->S_new = S_new; e->reason = reason;
    e->moved_pkts = moved;
    e->resize_ns = resize_ns; e->drain_ns = drain_ns;
}

void conn_force_check(conn_t *c) {
    if (!c->cfg->adaptive_enable) return;
    spin_lock(&c->lk);
    check_and_maybe_resize(c);
    spin_unlock(&c->lk);
}

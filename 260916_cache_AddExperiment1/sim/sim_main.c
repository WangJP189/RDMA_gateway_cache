/*
 * sim/sim_main.c — 驱动：串联发包/丢包/NAK + 落盘全部 trace（spec §4.5）
 * ------------------------------------------------------------------
 * 输出（实验三全部数据来源）：
 *   S_trace.csv           epoch,phase,S_after,event,L_ring,ovf_count,L_ovf
 *   utilization_trace.csv epoch,phase,util_adaptive,util_static
 *   overflow_trace.csv    epoch,phase,ovf_count,ovf_bytes
 *   latency_trace.csv     epoch,phase,lookup_avg_ns,lookup_max_ns,clock_ns_per_op
 *   overhead.csv          #注意单次事件含底噪 / epoch,check_ns,resize_ns,drain_ns,moved_pkts,clock_floor_ns
 *   resize_events.csv     epoch,S_old,S_new,reason,moved_pkts
 *   metrics.csv           epoch,phase,elapsed_us,nak_n,hit,miss,bytes_retrans
 *
 * sim_link_gbps 只进 metrics.csv 的 elapsed_us 列（时间轴），机制侧绝不读取；
 * 因此换 sim_link_gbps 后 S_trace.csv 等算法相关 trace 逐字节一致（A13 运行时部分）。
 */
#include "sim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

#define MAX_PKT_SZ   16384u        /* 单包 payload 缓冲（len ≤ sc_new[26]=8192） */
#define RETX_BUF_SZ  (1u << 16)    /* 重传输出缓冲 64 KB（delay=2 时远超最大区间） */
#define LAT_BATCH    16384u        /* lookup 批量基准的摊销次数（本机读钟 ~μs/次，必须批量） */
#define LAT_SUB      1024u         /* 子批大小：1024 lookup 摊销一次读钟，子批间取 max 捕捉尖峰 */
#define CLK_BATCH    1024u         /* 读钟底噪测量：1024 次 rdtsc 摊销一次 */

/* ---- 通用辅助 ---- */
static void fill_payload(uint8_t *buf, uint32_t len, uint32_t psn) {
    for (uint32_t k = 0; k < len; k++) buf[k] = (uint8_t)((psn + k) & 0xFFu);
}

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

/* 当前溢出区占用字节（align16(HDR+len) 之和），O(OVF_CAP)。 */
static uint64_t ovf_bytes(const conn_t *c) {
    uint64_t b = 0;
    for (uint32_t oi = 0; oi < c->cfg->ovf_cap; oi++)
        if (c->ovf[oi].used) b += align16(HDR_SZ + c->ovf[oi].len);
    return b;
}

/* 纪元号 → 阶段号（阶段边界按累积纪元数切分，独立于 traffic 内部计数器）。 */
uint32_t phase_for_epoch(const cfg_t *cfg, uint32_t epoch) {
    uint32_t cum = 0;
    for (uint32_t k = 0; k < cfg->e3_phase_n; k++) {
        uint32_t next = cum + cfg->e3_phases[k].epochs;
        if (epoch < next) return k;
        cum = next;
    }
    return cfg->e3_phase_n;   /* 超出阶段表（不应发生） */
}

/* 每纪元末的批量摊销 lookup 基准 + 读钟底噪（本机每次读钟 ~μs，逐次计时不可用）。
 *   avg_ns          干净平均：一对 rdtsc 包住 16384 次 lookup（无内层读钟）——最干净的信号
 *   max_ns          16 子批×1024 lookup 的最大摊销值——捕捉局部尖峰（1ms 尖峰 → ~1μs 子批值）；
 *                   注意该列含 ~clock/1024 ≈ 3.9ns 读钟底噪摊销（见 clock_ns_per_op）
 *   clock_ns_per_op 1024 次 rdtsc 的摊销成本——读钟底噪，供读者判断信号/本底比 */
static void latency_probe(conn_t *gw, const cfg_t *cfg, uint32_t psn,
                          double *avg_ns, double *max_ns, double *clock_ns_per_op) {
    uint64_t t0 = rdtsc_raw();
    for (uint32_t k = 0; k < LAT_BATCH; k++) {
        uint16_t llen;
        const uint8_t *p = conn_lookup(gw, psn - (k % cfg->ring_n), &llen);
        g_sink ^= (uint64_t)(size_t)p ^ llen;
    }
    uint64_t t1 = rdtsc_raw();
    *avg_ns = to_ns(t1 - t0) / (double)LAT_BATCH;

    double mx = 0.0;
    for (uint32_t s = 0; s < LAT_BATCH / LAT_SUB; s++) {
        uint64_t ts = rdtsc_raw();
        for (uint32_t k = 0; k < LAT_SUB; k++) {
            uint16_t llen;
            const uint8_t *p = conn_lookup(gw, psn - ((s * LAT_SUB + k) % cfg->ring_n), &llen);
            g_sink ^= (uint64_t)(size_t)p ^ llen;
        }
        uint64_t te = rdtsc_raw();
        double sub = to_ns(te - ts) / (double)LAT_SUB;
        if (sub > mx) mx = sub;
    }
    *max_ns = mx;

    uint64_t c0 = rdtsc_raw();
    for (uint32_t k = 0; k < CLK_BATCH; k++) g_sink ^= rdtsc_raw();
    uint64_t c1 = rdtsc_raw();
    *clock_ns_per_op = to_ns(c1 - c0) / (double)CLK_BATCH;
}

/* ---- trace 生命周期 ---- */
static void trace_close(trace_writer_t *tw);

static int trace_open(trace_writer_t *tw, const char *dir) {
    memset(tw, 0, sizeof(*tw));
    char path[512];
#define OPEN(var, name) do { \
        snprintf(path, sizeof(path), "%s/%s", dir, name); \
        tw->var = fopen(path, "w"); \
        if (!tw->var) goto fail; \
    } while (0)
    OPEN(f_epoch,   "S_trace.csv");
    OPEN(f_util,    "utilization_trace.csv");
    OPEN(f_ovf,     "overflow_trace.csv");
    OPEN(f_lat,     "latency_trace.csv");
    OPEN(f_overhead,"overhead.csv");
    OPEN(f_resize,  "resize_events.csv");
    OPEN(f_metrics, "metrics.csv");
#undef OPEN
    fprintf(tw->f_epoch,    "epoch,phase,S_after,event,L_ring,ovf_count,L_ovf\n");
    fprintf(tw->f_util,     "epoch,phase,util_adaptive,util_static\n");
    fprintf(tw->f_ovf,      "epoch,phase,ovf_count,ovf_bytes\n");
    fprintf(tw->f_lat,      "epoch,phase,lookup_avg_ns,lookup_max_ns,clock_ns_per_op\n");
    fprintf(tw->f_overhead, "# 注意：check_ns/resize_ns/drain_ns 是单次事件计时（各一对 rdtsc），含 ~2*clock_floor_ns 读钟底噪；只能读作「<= 底噪+未知」，不可读作「很小」\n");
    fprintf(tw->f_overhead, "epoch,check_ns,resize_ns,drain_ns,moved_pkts,clock_floor_ns\n");
    fprintf(tw->f_resize,   "epoch,S_old,S_new,reason,moved_pkts\n");
    fprintf(tw->f_metrics,  "epoch,phase,elapsed_us,nak_n,hit,miss,bytes_retrans\n");
    return 0;
fail:
    trace_close(tw);
    return -1;
}

static void trace_close(trace_writer_t *tw) {
    if (tw->f_epoch)    fclose(tw->f_epoch);
    if (tw->f_util)     fclose(tw->f_util);
    if (tw->f_ovf)      fclose(tw->f_ovf);
    if (tw->f_lat)      fclose(tw->f_lat);
    if (tw->f_overhead) fclose(tw->f_overhead);
    if (tw->f_resize)   fclose(tw->f_resize);
    if (tw->f_metrics)  fclose(tw->f_metrics);
    memset(tw, 0, sizeof(*tw));
}

/* 每纪元末写一行到各 trace（resize_events 在收尾统一写）。 */
static void write_epoch_rows(trace_writer_t *tw, const cfg_t *cfg, const conn_t *gw,
                             const sim_stats_t *st, uint32_t epoch, uint32_t phase,
                             uint64_t ep_payload, uint32_t S_prev, uint64_t drain0,
                             uint64_t nak_n0, uint64_t hit0, uint64_t miss0,
                             uint64_t bytes0, double lookup_avg_ns, double lookup_max_ns,
                             double clock_ns_per_op) {
    const char *event = (gw->S > S_prev) ? "grow"
                      : (gw->S < S_prev) ? "shrink" : "none";
    uint64_t moved = gw->n_drain - drain0;
    uint64_t ovf_b = ovf_bytes(gw);

    /* 利用率 = payload / allocated（spec §5.3；allocated 只计当前池 + meta + 溢出） */
    uint64_t alloc_a = (uint64_t)gw->N * stride_of(gw->S)
                     + (uint64_t)gw->N * sizeof(slot_meta_t) + ovf_b;
    uint64_t alloc_s = (uint64_t)gw->N * stride_of(cfg->fallback_mtu)
                     + (uint64_t)gw->N * sizeof(slot_meta_t);
    double util_a = alloc_a ? (double)ep_payload / (double)alloc_a : 0.0;
    double util_s = alloc_s ? (double)ep_payload / (double)alloc_s : 0.0;

    /* 报告用时间轴：一纪元（N 个 MTU 包）在线路速率下的传输时长（微秒） */
    double elapsed_us = (double)gw->N * (double)cfg->fallback_mtu * 8.0
                      / ((double)cfg->sim_link_gbps * 1e3);

    fprintf(tw->f_epoch, "%u,%u,%u,%s,%u,%u,%u\n",
            epoch, phase, gw->S, event,
            gw->last_epoch_ring_max, gw->last_epoch_ovf_count, gw->last_epoch_L_ovf);
    fprintf(tw->f_util, "%u,%u,%.4f,%.4f\n", epoch, phase, util_a, util_s);
    fprintf(tw->f_ovf, "%u,%u,%u,%llu\n",
            epoch, phase, gw->last_epoch_ovf_count, (unsigned long long)ovf_b);
    fprintf(tw->f_lat, "%u,%u,%.3f,%.3f,%.3f\n", epoch, phase, lookup_avg_ns,
            lookup_max_ns, clock_ns_per_op);
    fprintf(tw->f_overhead, "%u,%llu,%llu,%llu,%llu,%.3f\n", epoch,
            (unsigned long long)gw->check_ns, (unsigned long long)gw->resize_ns,
            (unsigned long long)gw->drain_ns, (unsigned long long)moved, clock_ns_per_op);
    fprintf(tw->f_metrics, "%u,%u,%.4f,%llu,%llu,%llu,%llu\n", epoch, phase, elapsed_us,
            (unsigned long long)(st->nak_n - nak_n0),
            (unsigned long long)(st->hit - hit0),
            (unsigned long long)(st->miss - miss0),
            (unsigned long long)(st->bytes_retrans - bytes0));
}

static void write_resize_events(trace_writer_t *tw, const conn_t *gw) {
    for (uint32_t k = 0; k < gw->n_rev; k++) {
        const resize_event_t *e = &gw->rev[k];
        const char *reason = (e->reason == REV_GROW)      ? "grow"
                           : (e->reason == REV_SHRINK)    ? "shrink"
                           :                                "overflow_grow";
        fprintf(tw->f_resize, "%u,%u,%u,%s,%u\n",
                e->epoch, e->S_old, e->S_new, reason, e->moved_pkts);
    }
}

/* ---- 驱动 ---- */
int sim_run(const cfg_t *cfg, sim_stats_t *st, trace_writer_t *tw) {
    tsc_calibrate();   /* 确保 to_ns 的 rdtsc→ns 标定生效 */

    conn_t gw; conn_init(&gw, cfg, cfg->fallback_mtu);
    traffic_t tr; traffic_init(&tr, cfg, 0);
    loss_t ls;    loss_init(&ls, cfg);
    nakgen_t ng;  nak_init(&ng, cfg);

    uint8_t *out = (uint8_t *)malloc(RETX_BUF_SZ);
    uint8_t *pkt = (uint8_t *)malloc(MAX_PKT_SZ);
    if (!out || !pkt) {
        conn_destroy(&gw);
        free(ng.pend); free(out); free(pkt);
        return -1;
    }

    uint32_t epoch = 0;
    uint64_t ep_payload = 0;
    uint64_t nak_n0 = 0, hit0 = 0, miss0 = 0, bytes0 = 0, drain0 = 0;
    uint32_t S_prev = gw.S;
    uint32_t rev_seen = 0;

    for (;;) {
        uint32_t len;
        uint32_t psn = traffic_next(&tr, &len);
        if (psn == UINT32_MAX) break;

        fill_payload(pkt, len, psn);
        conn_store(&gw, psn, pkt, (uint16_t)len);   /* 每个包都缓存（无论是否丢） */
        ep_payload += len;

        /* 把本纪元新产生的 resize 事件补上纪元号（check/grow_locked 可能在 conn_store 里触发） */
        while (gw.n_rev > rev_seen) gw.rev[rev_seen++].epoch = epoch;

        int lost = loss_is_lost(&ls, psn);
        nak_on_packet(&ng, cfg, psn, lost, &gw, out, RETX_BUF_SZ, st);

        if ((psn + 1) % cfg->ring_n == 0) {          /* 纪元边界：写一行 trace */
            double lookup_avg_ns, lookup_max_ns, clock_ns_per_op;
            latency_probe(&gw, cfg, psn, &lookup_avg_ns, &lookup_max_ns, &clock_ns_per_op);

            write_epoch_rows(tw, cfg, &gw, st, epoch, phase_for_epoch(cfg, epoch),
                             ep_payload, S_prev, drain0, nak_n0, hit0, miss0, bytes0,
                             lookup_avg_ns, lookup_max_ns, clock_ns_per_op);
            epoch++;
            ep_payload = 0;
            drain0  = gw.n_drain;
            nak_n0  = st->nak_n;  hit0 = st->hit;
            miss0   = st->miss;    bytes0 = st->bytes_retrans;
            S_prev  = gw.S;
        }
    }

    while (gw.n_rev > rev_seen) gw.rev[rev_seen++].epoch = epoch;  /* 收尾（正常无剩余） */

    uint64_t req = st->hit + st->miss;
    double hit_rate = req ? (double)st->hit / (double)req : 0.0;
    double avg_nak_us = st->nak_n ? (double)st->nak_ns_total / (double)st->nak_n / 1000.0 : 0.0;
    printf("[sim] epochs=%u total_pkts=%llu nak_n=%llu hit=%llu miss=%llu "
           "hit_rate=%.4f bytes_retrans=%llu\n",
           (unsigned)(tr.total_pkts / cfg->ring_n), (unsigned long long)tr.total_pkts,
           (unsigned long long)st->nak_n, (unsigned long long)st->hit,
           (unsigned long long)st->miss, hit_rate,
           (unsigned long long)st->bytes_retrans);
    printf("[sim] n_resize=%llu n_drain=%llu n_store=%llu n_lookup=%llu "
           "n_drop=%llu n_evict=%llu avg_nak_us=%.3f\n",
           (unsigned long long)gw.n_resize, (unsigned long long)gw.n_drain,
           (unsigned long long)gw.n_store, (unsigned long long)gw.n_lookup,
           (unsigned long long)gw.n_drop, (unsigned long long)gw.n_evict, avg_nak_us);

    write_resize_events(tw, &gw);
    conn_destroy(&gw);
    free(ng.pend);
    free(out); free(pkt);
    return 0;
}

/* ---- 入口 ---- */
int main(int argc, char **argv) {
    cfg_t cfg;
    cfg_default(&cfg);
    if (cfg_override_cli(&cfg, argc, argv) < 0) return 2;

    if (mkdir_p(cfg.out_dir) < 0) {
        fprintf(stderr, "[sim] 无法创建输出目录 %s\n", cfg.out_dir);
        return 1;
    }

    sim_stats_t *st = (sim_stats_t *)calloc(1, sizeof(*st));
    if (!st) return 1;

    trace_writer_t tw;
    if (trace_open(&tw, cfg.out_dir) < 0) {
        fprintf(stderr, "[sim] 无法打开 trace 文件（目录 %s）\n", cfg.out_dir);
        free(st);
        return 1;
    }

    int rc = sim_run(&cfg, st, &tw);
    trace_close(&tw);

    /* 每次运行落盘 resolved_config.json（§5.1：复现依据） */
    char path[512];
    snprintf(path, sizeof(path), "%s/resolved_config.json", cfg.out_dir);
    cfg_dump_json(&cfg, path);

    free(st);
    return rc;
}

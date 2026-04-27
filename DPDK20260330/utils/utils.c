#include "utils.h"
#include <rte_eal.h>
#include <stdio.h>

// ===================== PSN 环形比较函数 =====================
int psn_less_than(uint32_t a, uint32_t b) {
    uint32_t a_24 = a & PSN_MASK;
    uint32_t b_24 = b & PSN_MASK;

    if (a_24 == b_24) {
        return 0;
    }

    uint32_t forward_steps = (b_24 - a_24) & PSN_MASK;
    return (forward_steps <= PSN_HALF_CYCLE) ? 1 : 0;
}

int psn_greater_than(uint32_t a, uint32_t b) {
    uint32_t a_24 = a & PSN_MASK;
    uint32_t b_24 = b & PSN_MASK;

    if (a_24 == b_24) {
        return 0;
    }

    uint32_t forward_steps = (b_24 - a_24) & PSN_MASK;
    return (forward_steps > PSN_HALF_CYCLE) ? 1 : 0;
}

// ===================== PSN 过期判断 =====================
int is_psn_expired(struct conn_ctx_v4 *conn, uint32_t psn,
                   uint64_t current_ts) {
    uint32_t cache_index = psn & ARRAY_INDEX_MASK;

    if (conn->mbuf_array[cache_index].mbuf == NULL) {
        return RETRANS_NO_PACKET;
    }

    struct pkt_cache *cache = &conn->mbuf_array[cache_index];

    if (cache->psn != psn) {
        return RETRANS_PACkET_PSN_MISMATCH;
    }

    uint64_t survival_time = current_ts - cache->recv_stamp;
    return (survival_time > AGING_INTERVAL) ? 1 : 0;
}

// ===================== 连接空闲过期判断 =====================
int is_conn_idle_expired(struct conn_ctx_v4 *ctx, uint64_t current_tsc,
                         uint64_t timeout_tsc) {
    if (ctx == NULL) {
        return -1;
    }
    return (current_tsc - ctx->last_active_tsc > timeout_tsc) ? 1 : 0;
}

// ===================== 全局空闲连接清理 =====================
int clean_global_idle_entry(struct rte_hash *conn_hash) {
    if (conn_hash == NULL) {
        pr_err("[CLEAN IDLE] 连接哈希表无效\n");
        return 0;
    }

    uint64_t hz = rte_get_timer_hz();
    uint64_t timeout_tsc = (hz / 1000) * AGING_INTERVAL;
    uint64_t current_tsc = rte_get_timer_cycles();
    uint32_t aged_count = 0;
    uint32_t iterate_count = 0;

    const void *next_key;
    void *next_data;
    uint32_t iter = 0;

    pr_info("[CLEAN GLOBAL IDLE] 开始全局空闲连接清理\n");

    while (rte_hash_iterate(conn_hash, &next_key, &next_data, &iter) >= 0) {
        struct conn_ctx_v4 *ctx = (struct conn_ctx_v4 *)next_data;
        const struct conn_key_v4 *key = (const struct conn_key_v4 *)next_key;

        if (unlikely(ctx == NULL)) {
            continue;
        }

        if (is_conn_idle_expired(ctx, current_tsc, timeout_tsc) == 1) {
            rte_hash_del_key(conn_hash, key);
            aged_count++;
        }

        iterate_count++;
        if (iterate_count >= 500) {
            iterate_count = 0;
            usleep(100);
        }
    }

    if (aged_count > 0) {
        pr_info("[CLEAN GLOBAL IDLE] 清理完成，共释放 %u 个闲置连接\n",
                aged_count);
    } else {
        pr_info("[CLEAN GLOBAL IDLE] 清理完成，无闲置连接\n");
    }

    return aged_count;
}

// ===================== DPDK 连接释放回调（修复所有报错） =====================
void free_conn_ctx_cb(void *data, void *arg) {
    struct conn_ctx_v4 *ctx = (struct conn_ctx_v4 *)data;
    if (ctx == NULL)
        return;

    // 1. 停止DPDK重传定时器
    rte_timer_stop(&ctx->retry_timer);

    // 2. 批量清理所有缓存的DPDK mbuf
    batch_clean_psn_range(ctx, 0, PSN_MASK);

    // ===================== 关键修复 =====================
    // DPDK 静态初始化的自旋锁 【无需销毁】，删除 rte_spinlock_destroy
    // 原错误代码已删除，无野指针/内存泄漏

    // 3. 归还DPDK内存池（使用全局 g_data）
    rte_mempool_put(g_data.conn_ctx_pool, ctx);

    pr_debug("[FREE CONN] 连接资源已完全释放\n");
}
#ifndef UTILS_H
#define UTILS_H

#include "rdma_defs.h"
#include <arpa/inet.h>
#include <stdbool.h>

#include "config.h"
#include "data_structures.h"

// 提取BTH中16位的pkey
static inline uint16_t get_bth_pkey(const struct ib_bth *bth) {
    return rte_be_to_cpu_16(bth->pkey);
}

// 提取BTH中的24位DestQP
static inline uint32_t get_bth_dst_qp(const struct ib_bth *bth) {
    return rte_be_to_cpu_32(bth->reserved_qpn) & 0x00FFFFFF;
}

// 提取BTH中的ACK请求标志位
static inline uint8_t get_bth_ack_req(const struct ib_bth *bth) {
    return (rte_be_to_cpu_32(bth->ack_psn) >> 31) & 0x1;
}

// 提取BTH中的24位PSN
static inline uint32_t get_bth_psn(const struct ib_bth *bth) {
    return rte_be_to_cpu_32(bth->ack_psn) & 0x00FFFFFF;
}

// 提取AETH中的8位Syndrome
static inline uint8_t get_aeth_syndrome(const struct ib_aeth *aeth) {
    return (rte_be_to_cpu_32(aeth->syndrome_msn) >> 24) & 0xFF;
}

// 提取AETH中的24位MSN
static inline uint32_t get_aeth_msn(const struct ib_aeth *aeth) {
    return rte_be_to_cpu_32(aeth->syndrome_msn) & 0x00FFFFFF;
}

// 判断是否为数据报文
static inline bool is_data_packet(const struct ib_bth *bth) {
    return bth->opcode <= ROCE_OPCODE_RC_RDMA_READ_RESPONSE_ONLY;
}

// 判断是否为确认报文
static inline bool is_control_packet(const struct ib_bth *bth) {
    return bth->opcode == ROCE_OPCODE_RC_ACK;
}

// 提取确认包类型
static inline uint8_t get_aeth_type(const struct ib_aeth *aeth) {
    uint8_t syndrome = get_aeth_syndrome(aeth);
    return ((syndrome >> 5) & 0x07);
}

// 提取rnr timer时间索引
static inline uint8_t get_rnr_timer_index(const struct ib_aeth *aeth) {
    uint8_t syndrome = get_aeth_syndrome(aeth);
    return syndrome & 0x1F;
}

// 将IP字符串转换为本机字节序 (IPv4)
static inline uint32_t ip_str_to_host_v4(const char *ip_str) {
    struct in_addr addr;
    if (inet_pton(AF_INET, ip_str, &addr) != 1) {
        return 0; // 转换失败
    }
    return ntohl(addr.s_addr);
}




// 环形语境下判断a是否小于b（仅24位PSN，独立逻辑，无外部依赖）
// 返回1：a < b；返回0：a ≥ b（a==b 或 a > b）
int psn_less_than(uint32_t a, uint32_t b) {
    // 截取24位PSN（使用config.h宏定义）
    uint32_t a_24 = a & PSN_MASK;
    uint32_t b_24 = b & PSN_MASK;

    // 等值直接返回0
    if (a_24 == b_24) {
        return 0;
    }

    // 环形步进计算（使用config.h宏定义）
    uint32_t forward_steps = (b_24 - a_24) & PSN_MASK;

    return (forward_steps <= PSN_HALF_CYCLE) ? 1 : 0;
}

// 环形语境下判断a是否大于b（仅24位PSN，独立逻辑，无外部依赖）
// 返回1：a > b；返回0：a ≤ b（a==b 或 a < b）
int psn_greater_than(uint32_t a, uint32_t b) {
    // 截取24位PSN（使用config.h宏定义）
    uint32_t a_24 = a & PSN_MASK;
    uint32_t b_24 = b & PSN_MASK;

    if (a_24 == b_24) {
        return 0;
    }

    // 环形步进计算（使用config.h宏定义）
    uint32_t forward_steps = (b_24 - a_24) & PSN_MASK;

    return (forward_steps > PSN_HALF_CYCLE) ? 1 : 0;
}

// 判断单个PSN是否过期（适配DPDK架构）
// 入参：conn_ctx_v4 DPDK连接上下文、psn、当前时间戳
// 返回值：
// RETRANS_NO_PACKET       - 无缓存报文
// RETRANS_PACKET_PSN_MISMATCH - PSN不匹配
// 1 - 报文已过期，0 - 报文有效未过期
int is_psn_expired(struct conn_ctx_v4 *conn, uint32_t psn,
                   uint64_t current_ts) {
    // DPDK 高效数组索引：使用掩码替代取模（MAX_MBUF_ARRAY=4096=2^12）
    uint32_t cache_index = psn & ARRAY_INDEX_MASK;

    // 1. 无缓存报文（mbuf为空表示无数据，等价旧版ring_buf=0）
    if (conn->mbuf_array[cache_index].mbuf == NULL) {
        return RETRANS_NO_PACKET;
    }

    // 直接获取DPDK缓存结构体（无需强转内存块）
    struct pkt_cache *cache = &conn->mbuf_array[cache_index];

    // 2. PSN不匹配（旧版逻辑完全保留）
    if (cache->psn != psn) {
        return RETRANS_PACkET_PSN_MISMATCH;
    }

    // 3. 计算存活时间，判断是否过期（逻辑与旧版完全一致）
    // 老化阈值使用DPDK配置的 AGING_INTERVAL (500ms)
    uint64_t survival_time = current_ts - cache->recv_stamp;
    return (survival_time > AGING_INTERVAL) ? 1 : 0;
}



// WJP
// ============ 连接级老化清理 ============
/**
 * @brief DPDK版：判断连接是否空闲过期
 * @param ctx 连接上下文
 * @param current_tsc 当前TSC时钟
 * @param timeout_tsc 老化阈值(TSC周期)
 * @return 1=过期, 0=未过期, -1=参数异常
 */
int is_conn_idle_expired(struct conn_ctx_v4 *ctx, uint64_t current_tsc,
                         uint64_t timeout_tsc) {
    if (ctx == NULL) {
        return -1;
    }
    // 核心判断：最后活跃时间 > 老化阈值
    return (current_tsc - ctx->last_active_tsc > timeout_tsc) ? 1 : 0;
}

/**
 * @brief DPDK版：全局清理空闲连接
 * @param conn_hash DPDK连接哈希表
 * @return 清理的连接数量
 */
int clean_global_idle_entry(struct rte_hash *conn_hash) {
    if (conn_hash == NULL) {
        pr_err("[CLEAN IDLE] 连接哈希表无效\n");
        return 0;
    }

    // 直接使用宏定义 AGING_INTERVAL 计算超时周期
    uint64_t hz = rte_get_timer_hz();
    uint64_t timeout_tsc = (hz / 1000) * AGING_INTERVAL;
    uint64_t current_tsc = rte_get_timer_cycles();
    uint32_t aged_count = 0;
    uint32_t iterate_count = 0;

    // DPDK哈希表RCU迭代器
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

        // 判定连接过期
        if (is_conn_idle_expired(ctx, current_tsc, timeout_tsc) == 1) {
            rte_hash_del_key(conn_hash, key);
            aged_count++;
        }

        // 防CPU占满：迭代休眠
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

/**
 * @brief DPDK连接释放回调（自动释放mbuf/定时器/锁）
 */
void free_conn_ctx_cb(void *data, void *arg) {
    struct conn_ctx_v4 *ctx = (struct conn_ctx_v4 *)data;
    if (ctx == NULL)
        return;

    // 1. 停止DPDK重传定时器
    rte_timer_stop(&ctx->retry_timer);

    // 2. 批量清理所有缓存的DPDK mbuf
    batch_clean_psn_range(ctx, 0, PSN_MASK);

    // 3. 销毁自旋锁
    rte_spinlock_destroy(&ctx->lock);

    // 4. 从DPDK内存池释放连接上下文
    rte_mempool_put(g_data.conn_ctx_pool, ctx);

    pr_debug("[FREE CONN] 连接资源已完全释放\n");
}

#endif // UTILS_H
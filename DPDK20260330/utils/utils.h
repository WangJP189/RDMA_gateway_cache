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

#endif // UTILS_H
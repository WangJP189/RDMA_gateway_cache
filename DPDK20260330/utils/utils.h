#ifndef UTILS_H
#define UTILS_H

#include "rdma_defs.h"
#include <arpa/inet.h>
#include <stdbool.h>

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

#endif // UTILS_H
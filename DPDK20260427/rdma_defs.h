#ifndef RDMA_DEFS_H
#define RDMA_DEFS_H

#include <rte_byteorder.h>
#include <stdint.h>

#define ROCEV2_UDP_PORT 4791
#define RDMA_BTH_LEN 12
#define RDMA_AETH_LEN 4

// 标准RoCEv2(InfiniBand RC)操作码定义(BTH Opcode)
#define ROCE_OPCODE_RC_SEND_FIRST 0x00
#define ROCE_OPCODE_RC_SEND_MIDDLE 0x01
#define ROCE_OPCODE_RC_SEND_LAST 0x02
#define ROCE_OPCODE_RC_SEND_LAST_WITH_IMM 0x03
#define ROCE_OPCODE_RC_SEND_ONLY 0x04
#define ROCE_OPCODE_RC_SEND_ONLY_WITH_IMM 0x05

#define ROCE_OPCODE_RC_RDMA_WRITE_FIRST 0x06
#define ROCE_OPCODE_RC_RDMA_WRITE_MIDDLE 0x07
#define ROCE_OPCODE_RC_RDMA_WRITE_LAST 0x08
#define ROCE_OPCODE_RC_RDMA_WRITE_LAST_WITH_IMM 0x09
#define ROCE_OPCODE_RC_RDMA_WRITE_ONLY 0x0A
#define ROCE_OPCODE_RC_RDMA_WRITE_ONLY_WITH_IMM 0x0B

#define ROCE_OPCODE_RC_RDMA_READ_REQUEST 0x0C
#define ROCE_OPCODE_RC_RDMA_READ_RESPONSE_FIRST 0x0D
#define ROCE_OPCODE_RC_RDMA_READ_RESPONSE_MIDDLE 0x0E
#define ROCE_OPCODE_RC_RDMA_READ_RESPONSE_LAST 0x0F
#define ROCE_OPCODE_RC_RDMA_READ_RESPONSE_ONLY 0x10

#define ROCE_OPCODE_RC_ACK 0x11 // 注意：ACK和 NAK都用这个Opcode！
#define ROCE_OPCODE_RC_ATOMIC_ACK 0x12
#define ROCE_OPCODE_RC_CMP_SWAP 0x13
#define ROCE_OPCODE_RC_FETCH_ADD 0x14

// AETH 类型定义
// AETH Syndrome的高3位决定了是ACK,RNR还是NAK
#define AETH_TYPE_ACK 0x00  // 000xxxxx
#define AETH_TYPE_RNR 0x01  // 001xxxxx
#define AETH_TYPE_RSVD 0x02 // 010xxxxx (保留)
#define AETH_TYPE_NAK 0x03  // 011xxxxx

// NAK 错误码定义
#define NAK_CODE_SEQ_ERR 0x00 // PSN 序列号错误(也就是丢包，触发重传的关键)
#define NAK_CODE_INV_REQ 0x01 // 无效请求(如不支持的操作码)
#define NAK_CODE_RMT_ACC 0x02 // 远程访问错误(如rkey校验失败、权限不足)
#define NAK_CODE_RMT_OP 0x03 // 远程操作错误(如越界写)
#define NAK_CODE_INV_RD 0x04 // 无效的RDMA Read请求
// 0x05 ~ 0x1F 全部为协议保留，不存在其他的 NAK 代码

// RoCEv2 BTH 结构 (大端序)
struct ib_bth {
    uint8_t opcode;
    uint8_t flags;           // 包含SE,Mig,Pad,Ver(通过函数按位提取)
    rte_be16_t pkey;         // 16位，保留网络大端序类型
    rte_be32_t reserved_qpn; // 8位reserved1 + 24位DestQP
    rte_be32_t ack_psn;      // 1位AckReq + 7位reserved2 + 24位PSN
} __rte_packed;

// RoCEv2 AETH 结构 (大端序)
struct ib_aeth {
    rte_be32_t syndrome_msn; // 8位syndrome + 24位MSN
} __rte_packed;

/*
 * RoCEv2 / IB RNR Timer Lookup Table
 * 索引 0-31 对应的退避时间，单位：微秒 (microseconds)
 */
static const uint32_t RNR_TIMER_US_TABLE[32] = {
    655360, // 00000: 655.36 ms (注意：索引0是最大值)
    10,     // 00001: 0.01 ms
    20,     // 00010: 0.02 ms
    30,     // 00011: 0.03 ms
    40,     // 00100: 0.04 ms
    60,     // 00101: 0.06 ms
    80,     // 00110: 0.08 ms
    120,    // 00111: 0.12 ms
    160,    // 01000: 0.16 ms
    240,    // 01001: 0.24 ms
    320,    // 01010: 0.32 ms
    480,    // 01011: 0.48 ms
    640,    // 01100: 0.64 ms
    960,    // 01101: 0.96 ms
    1280,   // 01110: 1.28 ms
    1920,   // 01111: 1.92 ms
    2560,   // 10000: 2.56 ms
    3410,   // 10001: 3.41 ms
    4260,   // 10010: 4.26 ms
    5120,   // 10011: 5.12 ms
    6820,   // 10100: 6.82 ms
    8530,   // 10101: 8.53 ms
    10240,  // 10110: 10.24 ms
    13650,  // 10111: 13.65 ms
    17060,  // 11000: 17.06 ms
    20480,  // 11001: 20.48 ms
    27300,  // 11010: 27.30 ms
    34130,  // 11011: 34.13 ms
    40960,  // 11100: 40.96 ms
    54610,  // 11101: 54.61 ms
    68260,  // 11110: 68.26 ms
    81920   // 11111: 81.92 ms
};

#endif // RDMA_DEFS_H
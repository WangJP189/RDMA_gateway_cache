#include "utils.h"
#include <rte_hash.h>

// 从3字节数组读取24位值
static inline uint32_t read_24bit(const uint8_t *bytes) {
    return (bytes[0] << 16) | (bytes[1] << 8) | bytes[2];
}

// 写入24位值到3字节数组
static inline void write_24bit(uint8_t *bytes, uint32_t value) {
    bytes[0] = (value >> 16) & 0xFF;
    bytes[1] = (value >> 8) & 0xFF;
    bytes[2] = value & 0xFF;
}

// 获取RoCEv2报文中的PSN
static inline uint32_t get_psn_from_bth(const struct ib_bth *bth) {
    return read_24bit(bth->psn);
}

// 获取RoCEv2报文中的目标QP
static inline uint32_t get_dest_qp_from_bth(const struct ib_bth *bth) {
    return read_24bit(bth->dest_qp);
}

// 判断是否为ACK/NAK报文（基于操作码）
static inline bool is_control_packet(const struct ib_bth *bth) {
    uint8_t opcode = bth->opcode;
    return (opcode == ROCE_OPCODE_RC_ACK ||
            opcode == ROCE_OPCODE_RC_ATOMIC_ACK ||
            opcode == ROCE_OPCODE_RC_NAK);
}

// 判断是否为数据报文
static inline bool is_data_packet(const struct ib_bth *bth) {
    uint8_t opcode = bth->opcode;
    return (opcode <= ROCE_OPCODE_RC_RDMA_READ_RESPONSE_ONLY);
}

// 从AETH获取MSN（消息序列号）
static inline uint32_t get_msn_from_aeth(const struct ib_aeth *aeth) {
    return read_24bit(aeth->msn);
}

// 获取AETH类型（syndrome的高3位）
static inline uint8_t get_aeth_type(const struct ib_aeth *aeth) {
    return (aeth->syndrome >> 5) & 0x07;
}

// 获取AETH中的代码（syndrome的低5位）
static inline uint8_t get_aeth_code(const struct ib_aeth *aeth) {
    return aeth->syndrome & 0x1F;
}

// 判断是否为ACK报文
static inline bool is_ack_packet(const struct ib_aeth *aeth) {
    return get_aeth_type(aeth) == AETH_TYPE_ACK;
}

// 判断是否为RNR报文
static inline bool is_rnr_packet(const struct ib_aeth *aeth) {
    return get_aeth_type(aeth) == AETH_TYPE_RNR;
}

// 判断是否为NAK报文
static inline bool is_nak_packet(const struct ib_aeth *aeth) {
    return get_aeth_type(aeth) == AETH_TYPE_NAK;
}

// 哈希函数
static inline uint32_t cache_hash_v4_func(const struct cache_key_v4 *key) {
    return rte_hash_crc(key, sizeof(struct cache_key_v4), 0);
}

// 判断PSN大小（考虑24位翻转）
static inline bool is_psn_greater(uint32_t a, uint32_t b) {
    return ((a - b) & PSN_MASK) < PSN_HALF_CYCLE && a != b;
}

static inline bool is_psn_less(uint32_t a, uint32_t b) {
    return ((b - a) & PSN_MASK) < PSN_HALF_CYCLE && a != b;
}

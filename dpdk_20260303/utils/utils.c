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




//-----------------WJP-----------------

// 辅助函数：判断psn是否在[start, end]的环形区间内（24位PSN回绕兼容）
// 返回1表示在区间内，0表示不在
int psn_in_ring_range(uint32_t psn, uint32_t start, uint32_t end) {

    // 环形语境下：psn < start  OR  psn > end → 不在[start, end]区间内
    if (psn_less_than(psn, start) || psn_greater_than(psn, end)) {
        return 0; // 不在区间内
    }

    // 非上述情况 → psn在[start, end]环形区间内
    return 1; // 在区间内
}

// 辅助函数：获取当前系统的毫秒级时间戳
uint64_t get_current_timestamp_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts); // 使用单调时钟，避免系统时间修改影响
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

// 辅助函数：重新查找当前连接中有效的最小PSN（修正失效的start_psn）
uint32_t find_valid_min_psn(struct connection_cache_array *conn) {

    uint32_t min_psn = PSN_INVALID;
    // 遍历整个环形缓冲区，查找所有有效数据块
    for (int i = 0; i < conn->array_length; i++) {
        if (conn->ring_buf[i] == 0) {
            continue; // 空位置跳过
        }

        struct mem_block_header *hdr =
            (struct mem_block_header *)(uintptr_t)conn->ring_buf[i];
        if (!hdr) {
            continue;
        }

        uint32_t curr_psn = hdr->psn;
        // 第一次找到有效PSN，直接赋值
        if (min_psn == PSN_INVALID) {
            min_psn = curr_psn;
        } else {
            // 环形语境下比较，找到更小的PSN
            if (psn_less_than(curr_psn, min_psn)) {
                min_psn = curr_psn;
            }
        }
    }

    return min_psn;
}

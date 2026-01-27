#ifndef UTILS_H
#define UTILS_H

#include "packet_defs.h"
#include <dpdk/rte_mbuf.h>
#include <stdbool.h>
#include <stdint.h>

// 从3字节数组读取24位值
static inline uint32_t read_24bit(const uint8_t *bytes);
// 写入24位值到3字节数组
static inline void write_24bit(uint8_t *bytes, uint32_t value);
// 获取RoCEv2报文中的PSN
static inline uint32_t get_psn_from_bth(const struct ib_bth *bth);
// 获取RoCEv2报文中的目标QP
static inline uint32_t get_dest_qp_from_bth(const struct ib_bth *bth);
// 判断是否为ACK/NAK报文
static inline bool is_control_packet(const struct ib_bth *bth);
// 判断是否为数据报文
static inline bool is_data_packet(const struct ib_bth *bth);
// 从AETH获取MSN
static inline uint32_t get_msn_from_aeth(const struct ib_aeth *aeth);
// 获取AETH类型
static inline uint8_t get_aeth_type(const struct ib_aeth *aeth);
// 获取AETH中的代码
static inline uint8_t get_aeth_code(const struct ib_aeth *aeth);
// 判断是否为ACK报文
static inline bool is_ack_packet(const struct ib_aeth *aeth);
// 判断是否为RNR报文
static inline bool is_rnr_packet(const struct ib_aeth *aeth);
// 判断是否为NAK报文
static inline bool is_nak_packet(const struct ib_aeth *aeth);
// 哈希函数
static inline uint32_t cache_hash_v4_func(const struct cache_key_v4 *key);

#endif // UTILS_H
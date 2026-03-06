#ifndef UTILS_H
#define UTILS_H

#include "packet_defs.h"
#include <rte_mbuf.h>
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

// 判断PSN大小（考虑24位翻转）
static inline bool is_psn_greater(uint32_t a, uint32_t b);
static inline bool is_psn_less(uint32_t a, uint32_t b);

// ========== 从pkt_cache.h迁移并适配DPDK的工具函数声明 ==========
// 1. PSN范围判断（考虑24位回绕，通用工具）
static inline bool psn_in_ring_range(uint32_t psn, uint32_t start,
                                     uint32_t end);

// 3. 毫秒级时间戳获取（DPDK适配，替换原始time.h实现）
static inline uint64_t get_current_timestamp_ms(void);

// 4. 查找环形数组中最小有效PSN（通用环形数组操作，无业务耦合）
static inline uint32_t find_valid_min_psn(uintptr_t *ring_buf,
                                          uint32_t array_length,
                                          uint32_t start_psn, uint32_t end_psn);

// 5. 24位PSN回绕辅助判断（通用工具，补充原有PSN比较逻辑）
static inline bool is_psn_wrap_around(uint32_t a, uint32_t b);

#endif // UTILS_H
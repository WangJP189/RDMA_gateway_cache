#ifndef CONNECTION_TABLE_H
#define CONNECTION_TABLE_H

#include <stdio.h>
#include <stdint.h>
#include <dpdk/rte_mbuf.h>
#include "global.h"

// 连接表操作
struct cache_hash_v4 *cache_tbl_v4_create(uint32_t num_buckets);
void destroy_cache_table(void);
int cache_tbl_v4_insert_data(struct cache_hash_v4 *ht,
                             const struct cache_key_v4 *key,
                             struct rte_mbuf *mbuf);

// 报文缓存操作
struct pkt_cache *create_pkt_cache(uint32_t psn, struct rte_mbuf *mbuf);
void destroy_pkt_cache(struct pkt_cache *pc);

// 插入报文到连接条目的PSN数组（考虑PSN翻转）
static int insert_packet_to_cache_v4(struct cache_entry_v4 *entry,
                                     struct rte_mbuf *mbuf, uint32_t psn);



// 报文老化操作

// 连接老化检查（递归二分法批量老化）
uint32_t aging_check_binary(struct cache_entry_v4 *entry,
                                   uint32_t start_psn, uint32_t end_psn,
                                   uint64_t aging_threshold);

void age_expired_packets(struct cache_entry_v4 *entry);                                   





// =============报文缓存辅助函数=============

// 比较PSN大小（兼容psn回绕）
int psn_less_than(uint32_t a, uint32_t b);
int psn_greater_than(uint32_t a, uint32_t b);

// 二分法查找最后一个过期PSN
uint32_t binary_find_last_expired_psn(struct cache_entry_v4 *entry,
                                      uint32_t start_psn, uint32_t end_psn,
                                      uint64_t current_cycles);

// 批量清理指定PSN区间的数据包（单段遍历优化版，兼容回绕）
uint32_t batch_clean_psn_range(struct cache_entry_v4 *entry, uint32_t start,
                               uint32_t end);

// 查找连接条目中有效的最小PSN
int is_psn_expired(struct cache_entry_v4 *entry, uint32_t psn,
                   uint64_t current_cycles);


// ================全局资源老化函数========================

// 清理指定桶的所有连接条目（供老化线程调用）
int clean_cache_bucket(uint32_t bucket_idx);

// 清理所有桶的连接条目（供老化线程调用）
int clean_all_cache_buckets(void);

#endif // CONNECTION_TABLE_H
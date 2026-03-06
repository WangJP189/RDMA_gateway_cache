#ifndef CONNECTION_TABLE_H
#define CONNECTION_TABLE_H

#include "global.h"

// ========== 原有函数声明 ==========
// 连接表操作
struct cache_hash_v4 *cache_tbl_v4_create(uint32_t num_buckets);
void destroy_cache_table(void);
int cache_tbl_v4_insert_data(struct cache_hash_v4 *ht,
                             const struct cache_key_v4 *key,
                             struct rte_mbuf *mbuf);

// 报文缓存操作
struct pkt_cache *create_pkt_cache(uint32_t psn, struct rte_mbuf *mbuf);
void destroy_pkt_cache(struct pkt_cache *pc);

// ========== 从pkt_cache.h迁移并适配DPDK的函数声明 ==========
// 1. 连接缓存查找（纯查找，无创建）
struct pkt_cache *cache_tbl_v4_lookup(const struct cache_hash_v4 *ht,
                                      const struct cache_key_v4 *key);

// 2. 连接缓存删除（按key删除）
int cache_tbl_v4_remove_entry(const struct cache_hash_v4 *ht,
                              const struct cache_key_v4 *key);

// 3. PSN范围判断（适配DPDK，用于缓存PSN合法性校验）
int psn_in_ring_range(uint32_t psn, uint32_t start, uint32_t end);

// 4. ACK确认后清理已确认的报文缓存（释放rte_mbuf）
int clean_acked_packets(struct pkt_cache *pc, uint32_t ack_msn);

// 5. 定时老化过期报文缓存（DPDK中用rte_timer替代pthread线程）
void age_expired_packets(struct pkt_cache *pc);

// 6. 批量清理指定PSN范围的报文缓存
int batch_clean_psn_range(struct pkt_cache *pc, uint32_t start, uint32_t end);

// 7. 查找连接缓存中的最小有效PSN（用于重传/老化）
uint32_t find_valid_min_psn(struct pkt_cache *pc);

// 8. 连接空闲超时判断（用于连接老化）
int is_conn_idle_expired(struct pkt_cache *pc);

// 9. 重传处理函数（适配DPDK，返回值复用原枚举，需在global.h声明枚举）
int process_retransmit(struct pkt_cache *pc, uint32_t start_psn,
                       uint32_t end_psn, struct rte_mbuf **retrans_mbufs,
                       uint32_t *retrans_count);

// 10. PSN工具函数（处理24位PSN回绕，DPDK下保留核心逻辑）
int psn_less_than(uint32_t a, uint32_t b);
int psn_greater_equal(uint32_t a, uint32_t b);

// 11. 打印连接表状态（DPDK调试用，保留声明）
void print_all_connections_status(void);

#endif // CONNECTION_TABLE_H
#ifndef CONNECTION_TABLE_H
#define CONNECTION_TABLE_H

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

#endif // CONNECTION_TABLE_H
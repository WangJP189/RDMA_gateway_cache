#ifndef CONN_TABLE_H
#define CONN_TABLE_H

#include "global.h"

// 初始化连接表
int init_conn_table(void);

// 销毁连接表资源
void destory_conn_table(void);

// ==========================================
// 连接表(Connection Table)操作接口
// ==========================================

// 创建一个新的连接规则
struct conn_ctx_v4 *create_conn_ctx(const struct conn_key_v4 *key,
                                    enum gateway_role role);

// 删除一个连接规则
// int del_conn_ctx(const struct conn_key_v4 *key);

// 按KEY查找连接规则
struct conn_ctx_v4 *lookup_conn_ctx(const struct conn_key_v4 *key);

// ===================== 报文缓存函数声明 =====================
int add_to_connection_cache(struct conn_ctx_v4 *conn_cache, uint32_t psn,
                            struct rte_mbuf *mbuf);

int cache_rdma_packet(struct conn_ctx_v4 *conn, uint32_t psn,
                                    struct rte_mbuf *mbuf);

// ===================== 报文老化清理函数声明 =====================
void age_expired_packets(struct conn_ctx_v4 *conn);
uint32_t find_valid_min_psn(struct conn_ctx_v4 *conn);
void binary_age_psn(struct conn_ctx_v4 *conn, uint32_t start_psn,
                    uint32_t end_psn, uint64_t current_ts);
int batch_clean_psn_range(struct conn_ctx_v4 *ctx, uint32_t start,
                          uint32_t end);

#endif // CONN_TABLE_H
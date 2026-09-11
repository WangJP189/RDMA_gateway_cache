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

// ===================== 缓存模块新增接口（仅缓存，无重传）
// =====================
// 1. CM建连：初始化连接缓存
void conn_cache_init(struct conn_ctx_v4 *ctx);
// 2. CM断连/异常断开：全量清空连接缓存
void conn_cache_clean_all(struct conn_ctx_v4 *ctx);
// 3. ACK：按PSN清理已确认缓存（兼容回绕）
int conn_cache_clean_by_ack(struct conn_ctx_v4 *ctx, uint32_t ack_psn);
// 4. 原始NAK：查询缓存是否存在（只查不清）
int conn_cache_check_psn(struct conn_ctx_v4 *ctx, uint32_t psn);
// 5. 获取动态老化时间（按RTO/RNR计算）
uint64_t conn_cache_get_age_time(struct conn_ctx_v4 *ctx);

#endif // CONN_TABLE_H
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

#endif // CONN_TABLE_H
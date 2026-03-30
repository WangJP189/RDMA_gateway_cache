#ifndef FLOW_TABLE_H
#define FLOW_TABLE_H

#include "global.h"

// 初始化连接表
int init_flow_table(void);

// 销毁流表资源
void destory_flow_table(void);

// ==========================================
// 流表 (Flow Table) 操作接口
// ==========================================

// 添加一条流规则
int add_flow_rule(const struct flow_key_v4 *key, uint32_t src_qp,
                  enum gateway_role role);

// 删除一条流规则
int del_flow_rule(const struct flow_key_v4 *key);

// 按KEY查找流规则
struct flow_rule_v4 *lookup_flow_rule(const struct flow_key_v4 *key);

#endif // FLOW_TABLE_H
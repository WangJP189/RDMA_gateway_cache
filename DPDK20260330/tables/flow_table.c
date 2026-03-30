#include "tables/flow_table.h"
#include "config.h"
#include "utils/debug.h"
#include <rte_errno.h>
#include <rte_jhash.h>

// 初始化流表
int init_flow_table(void) {
    // 初始化Flow Table相关的Hash和Mempool
    struct rte_hash_parameters flow_hash_params = {
        .name = "flow_hash",
        .entries = MAX_FLOW_ENTRIES,
        .key_len = sizeof(struct flow_key_v4),
        .hash_func = rte_jhash, // Jenkins Hash算法
        .hash_func_init_val = 0,
        .socket_id = rte_socket_id(),
        // 开启读写并发支持 (使用RCU无锁机制或TSX)
        .extra_flag = RTE_HASH_EXTRA_FLAGS_RW_CONCURRENCY};

    g_data.flow_hash = rte_hash_create(&flow_hash_params);
    if (!g_data.flow_hash) {
        dbg_err("流Hash表创建失败: %s\n", rte_strerror(rte_errno));
        return ERR_INIT_GW_RESOURCE;
    }

    g_data.flow_ctx_pool = rte_mempool_create(
        "flow_info_pool", MAX_FLOW_ENTRIES, sizeof(struct flow_rule_v4), 0, 0,
        NULL, NULL, NULL, NULL, rte_socket_id(), 0);
    if (!g_data.flow_ctx_pool) {
        dbg_err("流CTX内存池创建失败: %s\n", rte_strerror(rte_errno));
        return ERR_INIT_GW_RESOURCE;
    }

    return SUCCESS;
}

// 销毁流表资源
void destory_flow_table(void) {
    if (g_data.flow_hash) {
        rte_hash_free(g_data.flow_hash);
        g_data.flow_hash = NULL;
    }

    if (g_data.flow_ctx_pool) {
        rte_mempool_free(g_data.flow_ctx_pool);
        g_data.flow_ctx_pool = NULL;
    }
}

// ==========================================
// 流表(Flow Table)接口实现
// ==========================================

// 添加一条流规则
int add_flow_rule(const struct flow_key_v4 *key, uint32_t src_qp,
                  enum gateway_role role) {
    struct flow_rule_v4 *info = NULL;

    // 1. CHECK：如果已经存在，直接拒绝并返回，防止内存泄露
    info = lookup_flow_rule(key);
    if (info) {
        dbg_err("流表规则已存在(SrcQP: %u)，跳过插入\n", info->src_qp);
        return ERR_FLOW_RULE_ADD;
    }

    // 2. 从内存池中申请一块 info 内存
    int ret = rte_mempool_get(g_data.flow_ctx_pool, (void **)&info);
    if (ret < 0) {
        dbg_err("流CTX内存池已满，无法添加规则\n");
        return ERR_FLOW_RULE_ADD;
    }

    // 3. 填充数据
    info->src_qp = src_qp;
    info->role = role;

    // 4. 将Key和指向CTX的指针一起存入表
    ret = rte_hash_add_key_data(g_data.flow_hash, key, info);
    if (ret < 0) {
        dbg_err("无法挂载到流表\n");
        rte_mempool_put(g_data.flow_ctx_pool, info); // 失败需归还内存
        return ERR_FLOW_RULE_ADD;
    }

    return SUCCESS;
}

// 删除一条流规则
int del_flow_rule(const struct flow_key_v4 *key) {
    struct flow_rule_v4 *info = NULL;

    // 删除前必须先找到CTX指针
    info = lookup_flow_rule(key);
    if (!info) {
        dbg_err("无法删除流表规则\n");
        return ERR_FLOW_RULE_DEL;
    }

    rte_hash_del_key(g_data.flow_hash, key);     // 从哈希表摘除
    rte_mempool_put(g_data.flow_ctx_pool, info); // 归还给内存池

    return SUCCESS;
}

// 按KEY查找流规则
struct flow_rule_v4 *lookup_flow_rule(const struct flow_key_v4 *key) {
    struct flow_rule_v4 *info = NULL;
    // Fast Path: O(1) 复杂度直接拿到 info 指针
    int ret = rte_hash_lookup_data(g_data.flow_hash, key, (void **)&info);
    if (ret < 0) {
        dbg("未查找到对应流规则\n");
        return NULL; // 未命中
    }
    return info;
}
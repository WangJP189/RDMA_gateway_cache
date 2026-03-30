#include "tables/conn_table.h"
#include "config.h"
#include "utils/debug.h"
#include <rte_errno.h>
#include <rte_jhash.h>
#include <rte_malloc.h>

// ==========================================
// 【新增1】RCU延迟释放回调函数(后台清理工)
// ==========================================
// 当RCU系统确认所有收包线程都不再持有这个ctx时，会自动调用此函数
static void free_conn_ctx_cb(void *key_data_ptr, void *conn_value_ptr) {
    struct rte_mempool *pool = (struct rte_mempool *)key_data_ptr;
    struct conn_ctx_v4 *ctx = (struct conn_ctx_v4 *)conn_value_ptr;

    if (ctx) {

        // ==========================================
        // 【新增2】：在释放内存前，强制同步停止定时器
        // rte_timer_stop_sync保证如果定时器正在其他核上运行，会等待它运行结束
        // ==========================================
        rte_timer_stop_sync(&ctx->retry_timer);

        // 1. 无需加锁！因为RCU保证此时绝对没有任何线程在访问它
        for (int i = 0; i < MAX_MBUF_ARRAY; i++) {
            if (ctx->mbuf_array[i].mbuf != NULL) {
                rte_pktmbuf_free(ctx->mbuf_array[i].mbuf);
                ctx->mbuf_array[i].mbuf = NULL;
            }
        }
        // 2. 将干净的上下文归还给大页内存池
        rte_mempool_put(pool, ctx);
        dbg("RCU后台已安全回收连接上下文\n");
    }
}

// 初始化连接表
int init_conn_table(void) {
    // 1. 创建哈希表
    struct rte_hash_parameters conn_hash_params = {
        .name = "conn_hash",
        .entries = MAX_CONN_ENTRIES,
        .key_len = sizeof(struct conn_key_v4),
        .hash_func = rte_jhash,
        .hash_func_init_val = 0,
        .socket_id = rte_socket_id(),
        // 改用无锁并发标志
        .extra_flag = RTE_HASH_EXTRA_FLAGS_RW_CONCURRENCY_LF};

    g_data.conn_hash = rte_hash_create(&conn_hash_params);
    if (!g_data.conn_hash) {
        dbg_err("连接Hash表创建失败: %s\n", rte_strerror(rte_errno));
        return ERR_INIT_GW_RESOURCE;
    }

    // 2. 创建内存池
    g_data.conn_ctx_pool = rte_mempool_create(
        "conn_state_pool", MAX_CONN_ENTRIES, sizeof(struct conn_ctx_v4), 0, 0,
        NULL, NULL, NULL, NULL, rte_socket_id(), 0);

    if (!g_data.conn_ctx_pool) {
        dbg_err("连接CTX内存池创建失败: %s\n", rte_strerror(rte_errno));
        return ERR_INIT_GW_RESOURCE;
    }

    //【新增1】：初始化RCU变量
    size_t sz = rte_rcu_qsbr_get_memsize(RTE_MAX_LCORE);
    g_data.conn_rcu_var =
        (struct rte_rcu_qsbr *)rte_zmalloc("conn_rcu", sz, RTE_CACHE_LINE_SIZE);
    if (!g_data.conn_rcu_var)
        return ERR_INIT_GW_RESOURCE;

    rte_rcu_qsbr_init(g_data.conn_rcu_var, RTE_MAX_LCORE);

    //【新增1】：将RCU绑定到Hash表，并配置延迟回收队列(Defer Queue)
    struct rte_hash_rcu_config rcu_cfg = {
        .v = g_data.conn_rcu_var,
        .mode = RTE_HASH_QSBR_MODE_DQ, // 开启延迟回收模式
        .dq_size =
            MAX_CONN_ENTRIES / 8, // 队列大小与最大表项一致  【不合理】ToDo 1/8
        .free_key_data_func = free_conn_ctx_cb, // 注册回调函数
        .key_data_ptr =
            (void *)g_data.conn_ctx_pool // 将内存池指针作为参数传给回调
    };

    if (rte_hash_rcu_qsbr_add(g_data.conn_hash, &rcu_cfg) != 0) {
        dbg_err("RCU绑定连接表失败\n");
        return ERR_INIT_GW_RESOURCE;
    }

    return SUCCESS;
}

// 销毁连接表资源
void destory_conn_table(void) {
    // 清理连接表(必须先释放内部缓存的mbuf，再释放表)
    if (g_data.conn_hash) {
        const void *next_key;
        void *next_data;
        uint32_t iter = 0;

        // 主动遍历，清理所有仍然存活的连接中的mbuf
        while (rte_hash_iterate(g_data.conn_hash, &next_key, &next_data,
                                &iter) >= 0) {
            struct conn_ctx_v4 *ctx = (struct conn_ctx_v4 *)next_data;
            if (ctx) {
                // 遍历连接的环形缓冲区，释放所有未确认被占用的 mbuf
                for (int i = 0; i < MAX_MBUF_ARRAY; i++) {
                    // *mbuf_array[] 动态分配情形
                    // if (ctx->mbuf_array[i] != NULL) {
                    //     rte_pktmbuf_free(
                    //         ctx->mbuf_array[i]->mbuf); // 归还mbuf到内存池
                    //     free(ctx->mbuf_array[i]); // 释放pkt_cache结构体内存
                    //     ctx->mbuf_array[i] = NULL;
                    // }

                    // mbuf_array[] 预分配情形
                    if (ctx->mbuf_array[i].mbuf != NULL) {
                        rte_pktmbuf_free(
                            ctx->mbuf_array[i].mbuf); // 归还mbuf到内存池
                        ctx->mbuf_array[i].mbuf = NULL;
                    }
                }
            }
        }

        // 2. 释放Hash表结构
        rte_hash_free(g_data.conn_hash);
        g_data.conn_hash = NULL;
    }

    // 3. 释放Mempool
    if (g_data.conn_ctx_pool) {
        rte_mempool_free(g_data.conn_ctx_pool);
        g_data.conn_ctx_pool = NULL;
    }

    //【新增1】释放RCU控制变量的内存
    if (g_data.conn_rcu_var) {
        rte_free(g_data.conn_rcu_var);
        g_data.conn_rcu_var = NULL;
    }
}

// ==========================================
// 连接表(Connection Table)接口实现
// ==========================================

// 创建一个新的连接上下文
struct conn_ctx_v4 *create_conn_ctx(const struct conn_key_v4 *key,
                                    enum gateway_role role) {
    struct conn_ctx_v4 *ctx = NULL;

    /* 改为外部CHECK
    // 1. CHECK：如果已经存在，直接拒绝并返回，防止内存泄露
    ctx = lookup_conn_ctx(key);
    if (ctx) {
        dbg("连接上下文已存在，取消创建");
        return ctx;
    }
    */

    // 2. 从内存池中申请一块ctx内存
    int ret = rte_mempool_get(g_data.conn_ctx_pool, (void **)&ctx);
    if (ret < 0) {
        dbg_err("连接CTX内存池已满，无法创建新连接\n");
        return NULL;
    }

    // 3. 内存块清零
    memset(ctx, 0, sizeof(struct conn_ctx_v4));

    // 4. 初始化自旋锁
    rte_spinlock_init(&ctx->lock);

    // ==========================================
    //【新增2】初始化内嵌的定时器
    // ==========================================
    // 只是将定时器状态置为STOPPED，并未启动
    rte_timer_init(&ctx->retry_timer);

    // 5. 填充参数
    ctx->start_psn = PSN_INVALID;
    ctx->end_psn = PSN_INVALID;
    ctx->last_active_tsc = rte_get_timer_cycles();
    ctx->packet_count = 0;
    ctx->role = role;
    // ==========================================
    //【新增2】填充task相关参数
    // ==========================================
    memcpy(&ctx->conn_key, key, sizeof(struct conn_key_v4));
    ctx->current_reason = REASON_NONE;
    ctx->nak_psn = PSN_INVALID;

    // 6. 将Key和指向CTX的指针一起存入表
    ret = rte_hash_add_key_data(g_data.conn_hash, key, ctx);
    if (ret < 0) {
        dbg_err("无法挂载到连接表\n");
        rte_mempool_put(g_data.conn_ctx_pool, ctx); // 失败需归还内存
        return NULL;
    }

    return ctx;
}

// 删除一个连接上下文
int del_conn_ctx(const struct conn_key_v4 *key) {
    struct conn_ctx_v4 *ctx = lookup_conn_ctx(key);
    if (!ctx) {
        dbg("连接键对应的上下文不存在，取消删除\n");
        return ERR_CONN_CTX_DEL;
    }

    // // 1. 先从Hash表摘除索引
    // // 这样老化线程/解析线程就再也查不到它了
    // rte_hash_del_key(g_data.conn_hash, key);

    // // 2. 加锁清理残留报文
    // rte_spinlock_lock(&ctx->lock);

    // for (int i = 0; i < MAX_MBUF_ARRAY; i++) {
    //     if (ctx->mbuf_array[i].mbuf != NULL) {
    //         rte_pktmbuf_free(ctx->mbuf_array[i].mbuf);
    //         ctx->mbuf_array[i].mbuf = NULL;
    //     }
    // }

    // rte_spinlock_unlock(&ctx->lock);

    // // 3. 将干净的内存块归还给 Mempool
    // rte_mempool_put(g_data.conn_ctx_pool, ctx);

    // 只要调用del_key，DPDK会将它移入RCU的Defer Queue。
    // 等到安全的时候，它会自动调用free_conn_ctx_cb
    int ret = rte_hash_del_key(g_data.conn_hash, key);
    if (ret < 0) {
        dbg_err("从Hash中删除连接键失败：%s\n", rte_strerror(rte_errno));
        return ERR_CONN_CTX_DEL;
    }

    return SUCCESS;
}

// 按KEY查找连接上下文
struct conn_ctx_v4 *lookup_conn_ctx(const struct conn_key_v4 *key) {
    struct conn_ctx_v4 *ctx = NULL;
    // Fast Path: O(1)复杂度直接拿到ctx指针
    int ret = rte_hash_lookup_data(g_data.conn_hash, key, (void **)&ctx);
    if (ret < 0) {
        dbg("未查找到对应的连接上下文\n");
        return NULL; // 未命中
    }
    return ctx;
}
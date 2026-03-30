#ifndef GLOBAL_H
#define GLOBAL_H

#include "data_structures.h"
#include <pthread.h>
#include <rte_hash.h>
#include <rte_mempool.h>
#include <rte_rcu_qsbr.h> // 【新增1】引入RCU头文件
#include <rte_ring.h>

struct global_data {
    volatile uint8_t stop; // 系统启停标志 (0: 运行, 1: 停止)

    // 内存池
    struct rte_mempool *mbuf_pool;

    // 网卡参数
    uint16_t port_id;
    uint16_t rx_queue_id;
    uint16_t tx_queue_id;

    // 软件无锁环
    struct rte_ring *rx_ring;
    struct rte_ring *tx_ring;
    struct rte_ring *task_ring;

    // 流表与连接表资源
    struct rte_hash *flow_hash;
    struct rte_mempool *flow_ctx_pool;

    struct rte_hash *conn_hash;
    struct rte_mempool *conn_ctx_pool;
    struct rte_rcu_qsbr *conn_rcu_var; // 【新增1】连接表的RCU变量

    // 线程ID
    // 注册到DPDK物理核
    pthread_t rx_thread;
    pthread_t parse_thread;
    pthread_t task_thread; // 【新增4】任务线程
    // 普通线程
    pthread_t aging_thread;   // 【新增3】老化线程
    pthread_t retrans_thread; // 【新增5】重传线程

    // 线程健康状态
    uint64_t rx_thread_last_active;
    uint64_t parse_thread_last_active;
    uint64_t task_thread_last_active;    // 【新增4】
    uint64_t aging_thread_last_active;   // 【新增3】
    uint64_t retrans_thread_last_active; // 【新增5】

    // DEBUG
    // 性能统计计数器
    uint64_t rx_count;
    uint64_t rx_dropped;
    uint64_t rx_pkt_count;
    uint64_t rx_ack_count;
};

extern struct global_data g_data;

#endif // GLOBAL_H
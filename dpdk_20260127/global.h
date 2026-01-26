#ifndef GLOBAL_H
#define GLOBAL_H

#include "data_structures.h"
#include <rte_mempool.h>
#include <rte_ring.h>
#include <pthread.h>


// 全局共享数据结构
struct global_data {
    struct rte_ring *rx_ring;
    struct rte_ring *tx_ring;
    struct rte_ring *delay_task_ring;
    
    volatile uint8_t stop;
    
    struct cache_hash_v4 *cache_tbl_v4;
    struct flow_hash_v4 *flow_tbl_v4;
    
    struct rte_mempool *mbuf_pool;
    
    // 线程ID
    pthread_t rx_thread;
    pthread_t parse_thread;
    pthread_t aging_thread;
    pthread_t sr_retrans_thread;
    pthread_t delay_proc_thread;
    pthread_t timer_epoll_thread;
    
    // 线程健康状态
    uint64_t rx_thread_last_active;
    uint64_t parse_thread_last_active;
    uint64_t aging_thread_last_active;
    uint64_t sr_retrans_thread_last_active;
    uint64_t delay_proc_thread_last_active;
    
    // 延时处理系统
    int timer_epoll_fd;
    uint8_t timer_system_inited;
    
    // 网卡配置
    uint16_t port_id;
    uint16_t rx_queue_id;
    uint16_t tx_queue_id;
};

// 声明全局变量
extern struct global_data g_data;

#endif // GLOBAL_H
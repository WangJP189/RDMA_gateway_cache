#ifndef DATA_STRUCTURES_H
#define DATA_STRUCTURES_H

#include "config.h"
#include <pthread.h>
#include <rte_mbuf.h>
#include <stdbool.h>

enum gateway_role { SRC_GATEWAY = 0, DST_GATEWAY = 1, UNKNOWN_GATEWAY };

// 延时处理任务类型枚举
enum delay_task_type {
    DELAY_TASK_RNR_RETRY = 0,
    DELAY_TASK_NAK_RETRY = 1,
    DELAY_TASK_CUSTOM = 2,
};

// 待缓存报文结构
struct pkt_cache {
    uint32_t psn;
    uint64_t recv_stamp;
    struct rte_mbuf *mbuf;
};

// IPv4连接键（五元组+QP信息）
struct cache_key_v4 {
    uint32_t src_ip;
    uint32_t dst_ip;
    uint32_t src_qp;
    uint32_t dst_qp;
    uint16_t pkey;
    uint16_t resv;
} __rte_packed;

// IPv4_流_键（用于流表）
struct flow_key_v4 {
    uint32_t src_ip;
    uint32_t dst_ip;
    uint32_t dst_qp;
    uint16_t pkey;
    uint16_t resv;
} __rte_packed;

// 流表条目
struct flow_entry_v4 {
    struct flow_key_v4 key;
    uint32_t src_qp;
    enum gateway_role role;
    struct flow_entry_v4 *next;
    pthread_spinlock_t lock;
} __rte_packed;

// 流表
struct flow_hash_v4 {
    struct flow_entry_v4 **buckets;
    pthread_spinlock_t *bucket_locks;
    uint32_t num_buckets;
};

// 连接表条目
struct cache_entry_v4 {
    struct cache_key_v4 key;
    struct pkt_cache *mbuf_array[MAX_PSN_ARRAY];
    uint32_t start_psn;
    uint32_t end_psn;
    uint64_t timestamp;
    uint32_t packet_count;
    enum gateway_role role;

    // rnr延时nak相关
    bool receiver_not_ready;
    uint64_t rnr_timer;
    int rnr_timer_fd;
    uint32_t rnr_retry_psn;
    uint32_t rnr_retry_count;

    // nak序列号错误相关
    int nak_timer_fd;
    uint32_t nak_psn;

    struct cache_entry_v4 *next;
    pthread_spinlock_t lock;
} __rte_packed;

// 连接表
struct cache_hash_v4 {
    struct cache_entry_v4 **buckets;
    pthread_spinlock_t *bucket_locks;
    uint32_t num_buckets;
    uint32_t count;
};

// 延时任务结构体
struct delay_task_info {
    enum delay_task_type type;
    struct cache_key_v4 key;
    int timer_fd;
    uint32_t psn;
    uint32_t nak_code;
};

#endif // DATA_STRUCTURES_H
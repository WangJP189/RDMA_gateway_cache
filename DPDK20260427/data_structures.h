#ifndef DATA_STRUCTURES_H
#define DATA_STRUCTURES_H

#include "config.h"
#include <rte_mbuf.h>
#include <rte_spinlock.h>
#include <rte_timer.h>
#include <stdint.h>

enum gateway_role { SRC_GATEWAY = 0, DST_GATEWAY = 1, UNKNOWN_GATEWAY };

// 缓存报文结构
struct pkt_cache {
    uint32_t psn;
    uint64_t recv_stamp;
    struct rte_mbuf *mbuf;
};

// ==========================================
// 流表(Flow Table)数据结构
// ==========================================

// 流表Key(精确的16字节)
struct flow_key_v4 {
    uint32_t src_ip;
    uint32_t dst_ip;
    uint32_t dst_qp;
    uint16_t pkey;
    uint16_t resv;
}; // __rte_packed; // 可选

// 流表Data(rte_hash的value)
struct flow_rule_v4 {
    uint32_t src_qp;
    enum gateway_role role;
};

// ==========================================
// 连接表(Connection Table)数据结构
// ==========================================

// 连接表Key(精确的20字节)
struct conn_key_v4 {
    uint32_t src_ip;
    uint32_t dst_ip;
    uint32_t src_qp;
    uint32_t dst_qp;
    uint16_t pkey;
    uint16_t resv;
}; // __rte_packed; // 可选

enum task_reason {
    REASON_NONE = 0,
    // REASON_RNR_RETRY
    REASON_GBN_RETRY,
    REASON_SR_REQ,
    REASON_SR_REP
};

//【新增2】延时任务结构体 (传键不传址)
// 必须是4的倍数
// 强制对齐到32字节，防止Cache Line割裂
struct __rte_aligned(32) retrans_task {
    struct conn_key_v4 key;
    enum task_reason reason;
    uint32_t target_psn;
};

// 连接表Data(rte_hash的value)
struct conn_ctx_v4 {
    // 动态分配
    // struct pkt_cache *mbuf_array[MAX_MBUF_ARRAY];

    // 预分配
    struct pkt_cache mbuf_array[MAX_MBUF_ARRAY];

    uint32_t start_psn;
    uint32_t end_psn;
    uint64_t packet_count;
    uint64_t last_active_tsc; // 用于资源老化
    enum gateway_role role;

    rte_spinlock_t lock;

    //【新增2】内嵌原生定时器对象
    // 定时器物理上长在ctx内存块里，0分配开销
    struct rte_timer retry_timer;
    // 延时任务用
    struct conn_key_v4 conn_key;
    enum task_reason current_reason;
    uint32_t nak_psn;

    // ===================== 缓存模块新增字段 =====================
    enum conn_state state; // 连接状态（控制是否允许缓存）
    uint32_t rto_ms;       // 从CM报文获取的RTO（动态老化用）
    uint8_t rnr_timer_idx; // RNR定时器索引（动态老化用）
};

// 仅新增：连接状态（控制缓存是否允许）
enum conn_state {
    CONN_STATE_INIT = 0,         // 初始状态，禁止缓存
    CONN_STATE_ESTABLISHED = 1,  // 已建连，允许缓存
    CONN_STATE_DISCONNECTING = 2 // 断开中，禁止缓存
};

#endif // DATA_STRUCTURES_H
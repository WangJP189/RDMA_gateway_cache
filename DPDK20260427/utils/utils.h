#ifndef UTILS_H
#define UTILS_H

#include "rdma_defs.h"
#include <arpa/inet.h>
#include <stdbool.h>
#include <unistd.h> // 修复 usleep 隐式声明

// DPDK 必需头文件
#include <rte_hash.h>
#include <rte_mempool.h>
#include <rte_spinlock.h>
#include <rte_timer.h>

// 项目头文件
#include "config.h"
#include "data_structures.h"
#include "global.h"

// ===================== 修复日志宏缺失（你项目未定义 pr_xxx）
// =====================
#define pr_err(fmt, ...) printf("[ERROR] " fmt, ##__VA_ARGS__)
#define pr_info(fmt, ...) printf("[INFO]  " fmt, ##__VA_ARGS__)
#define pr_debug(fmt, ...) printf("[DEBUG] " fmt, ##__VA_ARGS__)

// ===================== 静态内联函数（保留不动，允许写在 .h 中）
// =====================
static inline uint16_t get_bth_pkey(const struct ib_bth *bth) {
    return rte_be_to_cpu_16(bth->pkey);
}

static inline uint32_t get_bth_dst_qp(const struct ib_bth *bth) {
    return rte_be_to_cpu_32(bth->reserved_qpn) & 0x00FFFFFF;
}

static inline uint8_t get_bth_ack_req(const struct ib_bth *bth) {
    return (rte_be_to_cpu_32(bth->ack_psn) >> 31) & 0x1;
}

static inline uint32_t get_bth_psn(const struct ib_bth *bth) {
    return rte_be_to_cpu_32(bth->ack_psn) & 0x00FFFFFF;
}

static inline uint8_t get_aeth_syndrome(const struct ib_aeth *aeth) {
    return (rte_be_to_cpu_32(aeth->syndrome_msn) >> 24) & 0xFF;
}

static inline uint32_t get_aeth_msn(const struct ib_aeth *aeth) {
    return rte_be_to_cpu_32(aeth->syndrome_msn) & 0x00FFFFFF;
}

static inline bool is_data_packet(const struct ib_bth *bth) {
    return bth->opcode <= ROCE_OPCODE_RC_RDMA_READ_RESPONSE_ONLY;
}

static inline bool is_control_packet(const struct ib_bth *bth) {
    return bth->opcode == ROCE_OPCODE_RC_ACK;
}

static inline uint8_t get_aeth_type(const struct ib_aeth *aeth) {
    uint8_t syndrome = get_aeth_syndrome(aeth);
    return ((syndrome >> 5) & 0x07);
}

static inline uint8_t get_rnr_timer_index(const struct ib_aeth *aeth) {
    uint8_t syndrome = get_aeth_syndrome(aeth);
    return syndrome & 0x1F;
}

static inline uint32_t ip_str_to_host_v4(const char *ip_str) {
    struct in_addr addr;
    if (inet_pton(AF_INET, ip_str, &addr) != 1) {
        return 0;
    }
    return ntohl(addr.s_addr);
}

// ===================== 普通函数声明（仅声明，实现放在 utils.c）
// =====================
int psn_less_than(uint32_t a, uint32_t b);
int psn_greater_than(uint32_t a, uint32_t b);
int is_psn_expired(struct conn_ctx_v4 *conn, uint32_t psn, uint64_t current_ts);
int is_conn_idle_expired(struct conn_ctx_v4 *ctx, uint64_t current_tsc,
                         uint64_t timeout_tsc);
int clean_global_idle_entry(struct rte_hash *conn_hash);
void free_conn_ctx_cb(void *data, void *arg);

// 外部函数声明（缓存批量清理）
int batch_clean_psn_range(struct conn_ctx_v4 *ctx, uint32_t start,
                          uint32_t end);

#endif // UTILS_H
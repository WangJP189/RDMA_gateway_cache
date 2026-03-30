#include "processing/packet_processing.h"
#include "config.h"
#include "rdma_defs.h"
#include "tables/conn_table.h"
#include "tables/flow_table.h"
#include "utils/debug.h"
#include "utils/utils.h"
#include <rte_cycles.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_udp.h>

extern enum gateway_role self_role; // 当前网关角色

// #include <rte_malloc.h> // /*【定时器malloc版】

// ==========================================
//【新增2】定时器超时回调函数
// ==========================================
// 延时处理函数
static void retry_timer_cb(struct rte_timer *tim, void *arg) {
    // /*【定时器ctx版】
    // 传进来的arg就是启动定时器时绑定的conn_ctx
    struct conn_ctx_v4 *ctx = (struct conn_ctx_v4 *)arg;

    rte_spinlock_lock(&ctx->lock); // 获取锁

    // 1. 如果定时器已经被取消，直接返回
    if (ctx->current_reason == REASON_NONE) {
        rte_spinlock_unlock(&ctx->lock);
        return;
    }

    // 2. 组装task
    struct retrans_task task = {.key = ctx->conn_key,
                                .reason = ctx->current_reason,
                                .target_psn = ctx->nak_psn};

    // 3. 状态复位：表示该延时事件已经进入待处理队列
    ctx->current_reason = REASON_NONE;

    rte_spinlock_unlock(&ctx->lock); // 解锁

    // 4. 任务环入队
    // 操作软件是无锁的，要在释放ctx->lock之后进行
    int ret = rte_ring_enqueue_elem(g_data.task_ring, &task, sizeof(task));
    if (unlikely(ret != 0)) {
        // 如果task ring满了，任务会被丢弃。等待触发RoCEv2协议超时机制
        dbg_err("任务软件环已满，任务请求(psn:%d)丢弃\n", task.target_psn);
    }
    // else {
    //     dbg("task enqueue:%d\n", task.target_psn);
    // }
    // */

    /*【定时器malloc版】
    struct retrans_task *task = (struct retrans_task *)arg;

    struct conn_ctx_v4 *ctx = lookup_conn_ctx(&task->key);
    if (unlikely(!ctx)) {
        rte_free(task);
        dbg_err("销毁对应task\n");
        return;
    }

    rte_spinlock_lock(&ctx->lock); // 获取锁

    // 1. 如果定时器已经被取消，直接返回
    if (ctx->current_reason == REASON_NONE) {
        rte_free(task);
        dbg_err("定时器被取消,销毁对应task\n");
        return;
    }

    // 2. 状态复位：表示该延时事件已经进入待处理队列
    ctx->current_reason = REASON_NONE;

    rte_spinlock_unlock(&ctx->lock); // 解锁

    // 3. 任务环入队
    int ret = rte_ring_enqueue(g_data.task_ring, task);
    if (unlikely(ret != 0)) {
        dbg_err("任务软件环已满，任务请求(psn:%d)丢弃\n", task->target_psn);
        rte_free(task);
    }
    // else {
    //     dbg("task enqueue:%d\n", task->target_psn);
    // }
    */
}

static inline void schedule_retry_timer(struct conn_ctx_v4 *ctx,
                                        uint64_t delay_ms) {
    uint64_t ticks = (rte_get_timer_hz() / 1000) * delay_ms;
    // /*【定时器ctx版】
    int ret = rte_timer_reset(&ctx->retry_timer, ticks, SINGLE, rte_lcore_id(),
                              retry_timer_cb, ctx);
    if (ret != 0) {
        dbg_err("物理核%d定时器调度失败(psn:%d)\n", rte_lcore_id(),
                ctx->nak_psn);
    }
    // dbg("schedule:%d\n", ctx->nak_psn);
    // */

    /*【定时器malloc版】
    struct retrans_task *task =
        rte_zmalloc("RETRANS_TASK", sizeof(struct retrans_task), 0);

    task->key = ctx->conn_key;
    task->reason = ctx->current_reason;
    task->target_psn = ctx->nak_psn;

    int ret = rte_timer_reset(&ctx->retry_timer, ticks, SINGLE, rte_lcore_id(),
                    retry_timer_cb, task);
    if (ret != 0) {
        dbg_err("物理核%d定时器调度失败(psn:%d)\n", rte_lcore_id(),
                ctx->nak_psn);
    }
    // dbg("schedule: %d\n", task->target_psn);
    */
}

// 报文头提取、验证
static int extract_and_validate_headers(struct rte_mbuf *mbuf,
                                        struct rte_ether_hdr **eth_hdr,
                                        struct rte_ipv4_hdr **ip_hdr,
                                        struct rte_udp_hdr **udp_hdr,
                                        struct ib_bth **bth) {
    uint32_t offset = 0;
    // 获取报文的总有效数据长度
    uint32_t pkt_len = rte_pktmbuf_pkt_len(mbuf);

    // 1. 提取以太网头
    if (unlikely(pkt_len < offset + sizeof(struct rte_ether_hdr))) {
        dbg_err("包过小，Ethernet Header残缺\n");
        return ERR_PACKET_WRONG; // 这是一个残缺的烂包，直接拒绝
    }
    *eth_hdr = rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr *);

    // 检查是否为IPv4包(rte_cpu_to_be_16处理网络字节序)
    if ((*eth_hdr)->ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4)) {
        dbg("非IPv4包(ether_type:0x%04X)\n",
            rte_be_to_cpu_16((*eth_hdr)->ether_type));
        return ERR_PACKET_WRONG;
    }

    offset += sizeof(struct rte_ether_hdr);

    // 2. 提取以太网头
    if (unlikely(pkt_len < offset + sizeof(struct rte_ipv4_hdr))) {
        dbg_err("包过小，IP Header残缺\n");
        return ERR_PACKET_WRONG;
    }
    *ip_hdr = rte_pktmbuf_mtod_offset(mbuf, struct rte_ipv4_hdr *, offset);

    // 检查是否为UDP包
    if ((*ip_hdr)->next_proto_id != IPPROTO_UDP) {
        dbg("非UDP包(version:%u, proto:%u)\n", (*ip_hdr)->version_ihl >> 4,
            (*ip_hdr)->next_proto_id);
        return ERR_PACKET_WRONG;
    }

    // IPv4头部长度是可变的(IHL字段，通常是20字节)
    uint8_t ip_hlen = ((*ip_hdr)->version_ihl & 0x0f) * 4;
    offset += ip_hlen;

    // 3. 提取UDP头
    if (unlikely(pkt_len < offset + sizeof(struct rte_udp_hdr))) {
        dbg_err("包过小，UDP Header残缺\n");
        return ERR_PACKET_WRONG;
    }
    *udp_hdr = rte_pktmbuf_mtod_offset(mbuf, struct rte_udp_hdr *, offset);

    // 检查是否为RoCEv2的目标端口(4791)
    if ((*udp_hdr)->dst_port != rte_cpu_to_be_16(ROCEV2_UDP_PORT)) {
        dbg("非RoCEv2端口(dst_port:%u)\n",
            rte_be_to_cpu_16((*udp_hdr)->dst_port));
        return ERR_PACKET_WRONG;
    }

    offset += sizeof(struct rte_udp_hdr);

    // 4. 提取 RoCEv2 BTH
    if (unlikely(pkt_len < offset + sizeof(struct ib_bth))) {
        dbg_err("包过小，RoCEv2 BTH残缺\n");
        return ERR_PACKET_WRONG;
    }
    *bth = rte_pktmbuf_mtod_offset(mbuf, struct ib_bth *, offset);

    return offset;
}

// 数据包处理
static void process_data_packet(struct rte_mbuf *mbuf, const struct ib_bth *bth,
                                struct conn_ctx_v4 *ctx) {
    uint32_t psn = get_bth_psn(bth);

    rte_spinlock_lock(&ctx->lock); // 获取锁

    // ==========================================
    // 【新增2】：定时器干预
    // ==========================================
    if (unlikely(ctx->current_reason == REASON_SR_REQ)) {
        if (psn == ctx->nak_psn) {
            dbg("SR等待期内收到了PSN:%u，取消SR请求\n", psn);
            ctx->current_reason = REASON_NONE;
            ctx->nak_psn = PSN_INVALID;
            // 强行停止定时器，防止它下发无效的SR任务
            rte_timer_stop(&ctx->retry_timer);
        } else { // 定时器复位
            schedule_retry_timer(ctx, SR_REQ_INTERVAL);
        }
    }

    /*
    // ==========================================
    // TX发完包后，mbuf会自动free
    // 增加引用计数(refcnt变为2)，这样tx后包不会被销毁 (防止野指针)
    // ==========================================
    rte_mbuf_refcnt_update(mbuf, 1);

    // 如果该位置之前有老旧的残留包（回绕覆盖情况），先释放掉
    if (unlikely(ctx->mbuf_array[index].mbuf != NULL)) {
        dbg("PSN%u发生覆盖，释放旧mbuf\n", psn);
        rte_pktmbuf_free(ctx->mbuf_array[index].mbuf);
    }

    // 零拷贝指针赋值
    ctx->mbuf_array[index].mbuf = mbuf;
    ctx->mbuf_array[index].psn = psn;
    ctx->mbuf_array[index].recv_stamp = rte_get_timer_cycles(); // 记录 TSC 周期

    // 更新连接状态机
    if (ctx->start_psn == PSN_INVALID) {
        ctx->start_psn = psn; // 记录第一个包
    }
    // 这里暂时用简单判断更新 end_psn，实际需考虑 24位 PSN 回绕
    if (ctx->end_psn == 0xFFFFFFFF || psn > ctx->end_psn) {
        ctx->end_psn = psn;
    }
    */

    ctx->last_active_tsc = rte_get_timer_cycles();
    ctx->packet_count++;

    rte_spinlock_unlock(&ctx->lock); // 解锁
}

// ACK处理
static void process_ack_pakcet(struct ib_bth *bth, struct conn_ctx_v4 *ctx) {
    uint32_t psn = get_bth_psn(bth);

    rte_spinlock_lock(&ctx->lock); // 获取锁

    // if (psn >= ctx->nak_psn) {
    //     ctx->current_reason = REASON_NONE;
    //     ctx->nak_psn = PSN_INVALID;
    // } else {
    //     rte_spinlock_unlock(&ctx->lock); // 解锁
    //     return;
    // }
    ctx->current_reason = REASON_NONE;
    ctx->nak_psn = PSN_INVALID;

    // 缓存清理 ToDo
    // cleanup();

    rte_spinlock_unlock(&ctx->lock); // 解锁
}

// NAK处理
static void process_nak_packet(struct ib_bth *bth, struct conn_ctx_v4 *ctx) {
    uint32_t psn = get_bth_psn(bth);

    rte_spinlock_lock(&ctx->lock); // 获取锁

    // 无关网关角色，先清缓存 ToDo
    // cleanup();

    if (ctx->role == DST_GATEWAY) { // 目的网关动作
        // 如果缓存为空，向源网关要包 (SR请求)
        // 判断要改，找psn对应的index ToDo
        if (ctx->mbuf_array[psn & ARRAY_INDEX_MASK].mbuf == NULL) {
            if (psn > ctx->nak_psn || ctx->nak_psn == PSN_INVALID) {
                ctx->nak_psn = psn;
            } else {
                rte_spinlock_unlock(&ctx->lock); // 解锁
                return;
            }
            ctx->current_reason = REASON_SR_REQ;

            // 定时器调度
            schedule_retry_timer(ctx, SR_REQ_INTERVAL);
        } else {
            // 如果有缓存，直接下发本地GBN任务(无需定时器)
            struct retrans_task task = {.key = ctx->conn_key,
                                        .reason = REASON_GBN_RETRY,
                                        .target_psn = psn};
            // /* 【定时器ctx版】
            rte_ring_enqueue_elem(g_data.task_ring, &task, sizeof(task));
            // */
            /*【定时器malloc版】
            rte_ring_enqueue(g_data.task_ring, &task);
            */
        }
    } else if (ctx->role == SRC_GATEWAY) { // 源网关动作
        //
    }

    rte_spinlock_unlock(&ctx->lock); // 解锁
}

/* RNR处理 ToDo
static void process_rnr_packet(const struct ib_aeth *aeth,
                               struct conn_ctx_v4 *ctx) {

    // RNR时延是端侧QP建联时决定的
    // AETH Syndrome的低5位携带了min_rnr_timer的索引值
    uint8_t timer_index = get_rnr_timer_index(aeth);
    uint32_t delay_ms = RNR_TIMER_US_TABLE[timer_index] / 1000;
    schedule_retry_timer(ctx, REASON_RNR_RETRY, delay_ms);
}
*/

void process_packet(struct rte_mbuf *mbuf) {
    struct rte_ether_hdr *eth_hdr;
    struct rte_ipv4_hdr *ip_hdr;
    struct rte_udp_hdr *udp_hdr;
    struct ib_bth *bth;

    // 提取并校验头部
    int offset =
        extract_and_validate_headers(mbuf, &eth_hdr, &ip_hdr, &udp_hdr, &bth);
    if (offset == ERR_PACKET_WRONG) {
        // 杂包、ARP或是其他流量，直接丢弃，释放内存池资源
        rte_pktmbuf_free(mbuf);
        return;
    }

    // [DEBUG]
    // print_bth(bth);

    // 查流表
    uint32_t src_ip = rte_be_to_cpu_32(ip_hdr->src_addr);
    uint32_t dst_ip = rte_be_to_cpu_32(ip_hdr->dst_addr);
    uint32_t dst_qp = get_bth_dst_qp(bth);
    uint16_t pkey = get_bth_pkey(bth);
    struct flow_key_v4 recv_key = {.src_ip = src_ip,
                                   .dst_ip = dst_ip,
                                   .dst_qp = dst_qp,
                                   .pkey = pkey,
                                   .resv = 0};

    struct flow_rule_v4 *flow_ctx = lookup_flow_rule(&recv_key);
    if (unlikely(!flow_ctx)) {
        dbg("丢弃无流表匹配报文 DstQP: %u\n", dst_qp);
        rte_pktmbuf_free(mbuf);
        return;
    }

    if (is_data_packet(bth)) {
        // [DEBUG]
        g_data.rx_pkt_count++;

        struct conn_key_v4 forward_key = {.src_ip = src_ip,
                                          .dst_ip = dst_ip,
                                          .src_qp = flow_ctx->src_qp,
                                          .dst_qp = dst_qp,
                                          .pkey = pkey,
                                          .resv = 0};

        struct conn_ctx_v4 *ctx = lookup_conn_ctx(&forward_key);
        if (unlikely(!ctx)) {
            // ROLE的配置需要显式下发 ToDo
            ctx = create_conn_ctx(&forward_key, self_role);
            if (!ctx) {
                rte_pktmbuf_free(mbuf);
                dbg_err("销毁对应data pktmbuf\n");
                return;
            }
        }

        // 将包缓存到ctx
        process_data_packet(mbuf, bth, ctx);

    } else if (is_control_packet(bth)) {
        // [DEBUG]
        g_data.rx_ack_count++;

        // 构建反向连接键
        struct conn_key_v4 reverse_key = {
            .src_ip = dst_ip,           // 目的变源
            .dst_ip = src_ip,           // 源变目的
            .src_qp = dst_qp,           // 目的QP变源QP
            .dst_qp = flow_ctx->src_qp, // 流表中存的源QP变目的QP
            .pkey = pkey,
            .resv = 0};

        struct conn_ctx_v4 *ctx = lookup_conn_ctx(&reverse_key);
        if (unlikely(!ctx)) {
            // 如果连反向连接都不存在，说明是无效控制包，直接丢弃
            rte_pktmbuf_free(mbuf);
            dbg_err("销毁对应control pktmbuf\n");
            return;
        }

        offset += RDMA_BTH_LEN;
        struct ib_aeth *aeth =
            rte_pktmbuf_mtod_offset(mbuf, struct ib_aeth *, offset);

        uint8_t type = get_aeth_type(aeth);
        switch (type) {
        case AETH_TYPE_ACK:
            process_ack_pakcet(bth, ctx);
            break;
        case AETH_TYPE_NAK:
            process_nak_packet(bth, ctx);
            break;
        case AETH_TYPE_RNR:
            // process_rnr_packet(aeth, ctx);
            break;
        default:
            break;
        }

        rte_pktmbuf_free(mbuf);
    }
}

// DEBUG用 打印BTH解析结果
static inline void print_bth(const struct ib_bth *bth) {
    uint32_t dst_qp = get_bth_dst_qp(bth);
    uint16_t pkey = get_bth_pkey(bth);
    uint32_t psn = get_bth_psn(bth); // 提取并转换字节序

    dbg("******** ******** ********\n");
    dbg("\tOPCode=\t0x%02x \t | %u\n", bth->opcode, bth->opcode);
    dbg("\tPKey=\t0x%04x\t | %u\n", pkey, pkey);
    dbg("\tDstQP=\t0x%06X | %u\n", dst_qp, dst_qp);
    dbg("\tPSN=\t0x%06X | %u\n", psn, psn);
    dbg("******** ******** ********\n");
}
#include "threads/thread_functions.h"
#include "config.h"
#include "debug.h"
#include "global.h"
#include "processing/packet_processing.h"
#include <rte_cycles.h>
#include <rte_ethdev.h>
#include <unistd.h>

#include <rte_malloc.h> // /*【定时器malloc版】

void *rx_thread_func(void *arg) {
    struct global_data *gd = (struct global_data *)arg;
    struct rte_mbuf *mbufs[RX_BURST_SIZE];
    uint16_t nb_rx;

    // 注册非EAL线程(原生pthread)到DPDK上下文
    rte_thread_register();
    unsigned int lcore_id = rte_lcore_id();
    dbg("接收线程在物理核(Lcore)%d注册(原线程ID:%lu)\n", lcore_id,
        pthread_self());

    while (!gd->stop) {
        nb_rx = rte_eth_rx_burst(gd->port_id, gd->rx_queue_id, mbufs,
                                 RX_BURST_SIZE);

        gd->rx_thread_last_active = rte_get_timer_cycles();

        if (unlikely(nb_rx == 0)) {
            // 移除rte_delay_us，直接continue进行空转轮询
            // 或替换为rte_pause()降低功耗，但不会引起线程上下文切换
            rte_pause();
            continue;
        }

        // [DEBUG]
        gd->rx_count += nb_rx;

        unsigned int nb_enq =
            rte_ring_enqueue_burst(gd->rx_ring, (void **)mbufs, nb_rx, NULL);

        if (unlikely(nb_enq < nb_rx)) {
            uint16_t dropped = nb_rx - nb_enq;

            // 释放无法入队的报文
            // for (unsigned int i = nb_enq; i < nb_rx; i++) {
            //     rte_pktmbuf_free(mbufs[i])
            // }

            // 一次性批量归还给内存池
            rte_pktmbuf_free_bulk(&mbufs[nb_enq], dropped);

            // [DEBUG]
            gd->rx_dropped += dropped;
        }
    }

    dbg("接收线程在物理核(Lcore)%d注销\n", lcore_id);
    rte_thread_unregister();

    return NULL;
}

void *parse_thread_func(void *arg) {
    struct global_data *gd = (struct global_data *)arg;
    struct rte_mbuf *mbufs[RX_BURST_SIZE];
    uint16_t nb_deq;

    // 注册非EAL线程(原生pthread)到DPDK上下文
    rte_thread_register();
    unsigned int lcore_id = rte_lcore_id();
    dbg("解析线程在物理核(Lcore)%d注册(原线程ID:%lu)\n", lcore_id,
        pthread_self());

    // ==========================================
    // 【新增1】：RCU线程上线注册
    // ==========================================
    // 将当前线程注册到连接表的RCU变量中
    rte_rcu_qsbr_thread_register(gd->conn_rcu_var, lcore_id);
    // 报告线程在线，正式进入RCU读侧临界区监控
    rte_rcu_qsbr_thread_online(gd->conn_rcu_var, lcore_id);

    while (!gd->stop) {
        nb_deq = rte_ring_dequeue_burst(gd->rx_ring, (void **)mbufs,
                                        RX_BURST_SIZE, NULL);

        gd->parse_thread_last_active = rte_get_timer_cycles();

        // ==========================================
        // 【新增2】驱动DPDK定时器网络
        // ==========================================
        // API极其高效地检查当前核名下的所有定时器。
        // 如果时间没到，它瞬间返回（耗时几纳秒）
        // 如果时间到了，它会直接在这个线程里同步执行retry_timer_cb
        rte_timer_manage();

        if (unlikely(nb_deq == 0)) {
            // ==========================================
            // 【新增1】：空闲时的打卡
            // ==========================================
            // 如果没收到包，说明当前线程绝对没有持有任何哈希表内的指针。
            // 必须在这里向RCU报告静止状态！否则后台的老化清理工会被一直阻塞！
            rte_rcu_qsbr_quiescent(gd->conn_rcu_var, lcore_id);

            rte_pause();
            continue;
        }

        for (int16_t i = 0; i < nb_deq; i++) {
            struct rte_mbuf *mbuf = mbufs[i];

            process_packet(mbuf);

            // [debug] delete
            // rte_pktmbuf_free(mbuf);
        }

        // ==========================================
        // 【新增1】：批处理结束，报告静止状态
        // ==========================================
        // 这批包处理完了，意味着当前线程之前查到的conn_ctx指针都已经用完了
        // 向RCU汇报：我安全了，如果有待删除的内存，你们可以清理了。
        rte_rcu_qsbr_quiescent(gd->conn_rcu_var, lcore_id);
    }

    // ==========================================
    // 【新增1】：RCU线程下线注销
    // ==========================================
    rte_rcu_qsbr_thread_offline(gd->conn_rcu_var, lcore_id);
    rte_rcu_qsbr_thread_unregister(gd->conn_rcu_var, lcore_id);

    dbg("解析线程在物理核(Lcore)%d注销\n", lcore_id);
    rte_thread_unregister();

    return NULL;
}

// ==========================================
// 【新增4】任务调度线程
// ==========================================
void *task_thread_func(void *arg) {
    struct global_data *gd = (struct global_data *)arg;
    // /*【定时器ctx版】
    struct retrans_task tasks[TASK_BURST_SIZE];
    // */
    /*【定时器malloc版】
    struct retrans_task *tasks[TASK_BURST_SIZE];
    */
    uint16_t nb_tasks;

    // 注册非EAL线程(原生pthread)到DPDK上下文
    rte_thread_register();
    unsigned int lcore_id = rte_lcore_id();
    dbg("任务调度线程在物理核(Lcore)%d注册(原线程ID:%lu)\n", lcore_id,
        pthread_self());

    // RCU注册上线
    rte_rcu_qsbr_thread_register(gd->conn_rcu_var, lcore_id);
    rte_rcu_qsbr_thread_online(gd->conn_rcu_var, lcore_id);

    while (!gd->stop) {
        // 批量无锁出队
        // /*【定时器ctx版】
        nb_tasks = rte_ring_dequeue_burst_elem(gd->task_ring, tasks,
                                               sizeof(struct retrans_task),
                                               TASK_BURST_SIZE, NULL);
        // */
        /*【定时器malloc版】
        nb_tasks =
            rte_ring_dequeue_burst(gd->task_ring, tasks, RX_BURST_SIZE, NULL);
        */

        gd->task_thread_last_active = rte_get_timer_cycles();

        if (unlikely(nb_tasks == 0)) {
            // 空闲时打卡
            rte_rcu_qsbr_quiescent(gd->conn_rcu_var, lcore_id);
            // 稍作休眠，降低CPU功耗，等待新任务到来
            rte_pause();
            continue;
        }

        // 遍历取出的每一个任务
        for (int i = 0; i < nb_tasks; i++) {
            // /*【定时器ctx版】
            struct retrans_task *task = &tasks[i];
            enum task_reason reason = task->reason;
            // */
            /*【定时器malloc版】
            enum task_reason reason = tasks[i]->reason;
            */

            switch (reason) {
            case REASON_GBN_RETRY:
                break;
            case REASON_SR_REQ:
                break;
            case REASON_SR_REP:
                break;
            default:
                break;
            }

            /*【定时器malloc版】
            rte_free(tasks[i]);
            */
        }

        // 批任务处理完成，再次向RCU打卡
        rte_rcu_qsbr_quiescent(gd->conn_rcu_var, lcore_id);
    }

    // RCU下线注销
    rte_rcu_qsbr_thread_offline(gd->conn_rcu_var, lcore_id);
    rte_rcu_qsbr_thread_unregister(gd->conn_rcu_var, lcore_id);

    dbg("任务调度线程在物理核(Lcore)%d注销\n", lcore_id);
    rte_thread_unregister();

    return NULL;
}

// ==========================================
// 【新增3】老化线程(自由调度)
// ==========================================
void *aging_thread_func(void *arg) {
    struct global_data *gd = (struct global_data *)arg;

    // 连接老化时间(TSC周期)
    uint64_t aging_timeout_tsc = (rte_get_timer_hz() / 1000) * AGING_INTERVAL;
    uint64_t last_aging_tsc = rte_get_timer_cycles();

    while (!gd->stop) {
        // 每隔50ms醒来一次，期间线程处于睡眠状态
        usleep(50 * 1000);
        // rte_delay_ms(50);

        uint64_t current_tsc = rte_get_timer_cycles();

        // 检查距离上次清理是否已经过了 AGING_INTERVAL ms
        if (current_tsc - last_aging_tsc < aging_timeout_tsc)
            continue;

        last_aging_tsc = current_tsc;
        uint32_t aged_count = 0; // 记录本次干掉了多少个连接
        uint32_t iterate_count = 0;

        // 用于DPDK哈希表迭代的内部变量
        const void *next_key;
        void *next_data;
        uint32_t iter = 0; // 必须初始化为0

        while (rte_hash_iterate(gd->conn_hash, &next_key, &next_data, &iter) >=
               0) {

            struct conn_ctx_v4 *ctx = (struct conn_ctx_v4 *)next_data;
            const struct conn_key_v4 *key =
                (const struct conn_key_v4 *)next_key;

            if (unlikely(ctx == NULL))
                continue;

            // 寿命检查：当前时间-最后活跃时间>老化阈值
            if (current_tsc - ctx->last_active_tsc > aging_timeout_tsc) {
                // API只是把连接从哈希表里隐藏并放入RCU延迟队列。
                // 等到解析线程打卡后，底层会自动调用free_conn_ctx_cb
                rte_hash_del_key(gd->conn_hash, key);
                aged_count++;
                iterate_count++;
            }

            if (iterate_count >= 500) {
                iterate_count = 0;
                usleep(100);
            }
        }

        gd->aging_thread_last_active = rte_get_timer_cycles();

        if (aged_count > 0) {
            dbg("本轮老化线程清理完成，共移除%u个闲置连接\n", aged_count);
        }
    }

    return NULL;
}

// ==========================================
// 【新增5】重传线程 (自由调度)
// ==========================================
void *retrans_thread_func(void *arg) {
    struct global_data *gd = (struct global_data *)arg;

    dbg("重传线程启动(原线程ID:%lu)\n", pthread_self());

    // TODO: socket(),bind(),listen()等初始化逻辑

    while (!gd->stop) {
        gd->retrans_thread_last_active = rte_get_timer_cycles();
        // TODO: 监听TCP请求
        // TODO: 收到请求后解析数据，组装retrans_task
        // TODO: 调用rte_ring_enqueue()将任务压入gd->task_ring下发给调度线程

        // 在尚未实现阻塞逻辑前，加一个睡眠防止把CPU吃满
        usleep(100 * 1000); // 休眠100ms
    }

    return NULL;
}
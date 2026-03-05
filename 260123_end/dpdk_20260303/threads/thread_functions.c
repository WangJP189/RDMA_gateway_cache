#include "threads/thread_functions.h"
#include "global.h"
#include "processing/packet_processing.h"
#include "utils/utils.h"
#include <rte_ethdev.h>
#include <stdio.h>
#include <unistd.h>

// 线程1：DPDK收包线程
void *rx_thread_func(void *arg) {
    struct global_data *gd = (struct global_data *)arg;
    struct rte_mbuf *mbufs[BURST_SIZE];
    uint16_t nb_rx;
    uint64_t last_print_time = rte_get_timer_cycles();

    // 设置线程CPU亲和性
    rte_thread_set_affinity(rte_lcore_id());
    printf("RX Thread started on core %d\n", rte_lcore_id());

    while (!gd->stop) {
        // 从网卡批量收包
        nb_rx =
            rte_eth_rx_burst(gd->port_id, gd->rx_queue_id, mbufs, BURST_SIZE);

        // 更新线程活动时间
        gd->rx_thread_last_active = rte_get_timer_cycles();
        if (unlikely(nb_rx == 0)) {
            // 没有收到报文，短暂休眠
            rte_delay_us(1);
            continue;
        }

        // 批量放入无锁环形队列
        unsigned int nb_enq =
            rte_ring_enqueue_burst(gd->rx_ring, (void **)mbufs, nb_rx, NULL);
        if (unlikely(nb_enq < nb_rx)) {
            // 队列满，丢包
            uint16_t dropped = nb_rx - nb_enq;
            printf("RX thread: ring full, dropping %u packets\n", dropped);

            // 释放无法入队的报文
            for (unsigned int i = nb_enq; i < nb_rx; i++) {
                rte_pktmbuf_free(mbufs[i]);
            }
        }
    }
    printf("RX Thread exited\n");
    return NULL;
}

// 解析线程主函数
void *parse_thread_func(void *arg) {
    struct global_data *gd = (struct global_data *)arg;
    struct rte_mbuf *mbufs[BURST_SIZE];
    uint16_t nb_deq;
    uint64_t last_print_time = rte_get_timer_cycles();

    printf("Parse Thread started\n");
    while (!gd->stop) {
        // 从环形队列批量取包
        nb_deq = rte_ring_dequeue_burst(gd->rx_ring, (void **)mbufs, BURST_SIZE,
                                        NULL);
        gd->parse_thread_last_active = rte_get_timer_cycles();
        if (unlikely(nb_deq == 0)) {
            rte_delay_us(1);
            continue;
        }
        for (uint16_t i = 0; i < nb_deq; i++) {
            struct rte_mbuf *mbuf = mbufs[i];
            if (!validate_packet_integrity(mbuf)) {
                rte_pktmbuf_free(mbuf);
                continue;
            }

            struct rte_ether_hdr *eth_hdr;
            struct rte_ipv4_hdr *ip_hdr;
            struct rte_udp_hdr *udp_hdr;
            struct ib_bth *bth;
            struct cache_key_v4 key;
            if (!extract_and_validate_headers(mbuf, &eth_hdr, &ip_hdr, &udp_hdr,
                                              &bth)) {
                rte_pktmbuf_free(mbuf);
                continue;
            }

            if (is_data_packet(bth)) {
                int ret = build_data_key_v4(&key, ip_hdr, bth);
                if (ret != OK) {
                    printf("Parse thread: failed to build data key v4\n");
                    rte_pktmbuf_free(mbuf);
                    continue;
                }
                process_data_packet(gd, &key, mbuf);
            } else if (is_control_packet(bth)) {
                int ret = build_control_key_v4(&key, ip_hdr, bth);
                if (ret != OK) {
                    printf("Parse thread: failed to build control key v4\n");
                    rte_pktmbuf_free(mbuf);
                    continue;
                }
                process_control_packet(gd, &key, mbuf, bth);
            } else {
                printf("Parse thread: unknown RoCEv2 opcode: 0x%02X\n",
                       bth->opcode);
                rte_pktmbuf_free(mbuf);
            }
        }
    }
    printf("Parse Thread exited\n");
    return NULL;
}

// 支持PSN翻转的遍历函数
uint32_t traverse_psn_with_wrap(struct cache_entry_v4 *entry,
                                struct rte_mbuf **tx_burst, uint16_t max_burst,
                                uint64_t current_time) {
    if (!entry || entry->packet_count == 0) {
        return 0;
    }

    uint32_t start_psn = entry->start_psn;
    uint32_t end_psn = entry->end_psn;
    uint32_t processed_count = 0;

    // 检查RNR状态
    if (entry->receiver_not_ready) {
        if (current_time >= entry->rnr_timer) {
            // RNR超时，恢复发送
            entry->receiver_not_ready = false;
            printf("Connection: RNR timeout expired, resuming transmission\n");
        } else {
            // 仍在RNR状态，跳过重传
            return 0;
        }
    }

    // 计算需要遍历的PSN数量
    uint32_t count;
    if (end_psn >= start_psn) {
        count = end_psn - start_psn + 1;
    } else {
        // 溢出情况：从start到最大值，再从0到end
        count = (PSN_MASK - start_psn + 1) + (end_psn + 1);
    }

    // 限制遍历数量，避免处理过多报文
    uint32_t max_to_process = RTE_MIN(count, max_burst);
    max_to_process = RTE_MIN(max_to_process, MAX_PSN_ARRAY / 4);

    printf("Traverse PSN: start=0x%06X, end=0x%06X, count=%u, max=%u\n",
           start_psn, end_psn, count, max_to_process);

    // 遍历所有PSN
    uint64_t aging_threshold = AGING_INTERVAL * rte_get_timer_hz() / 1000;
    for (uint32_t i = 0; i < max_to_process && processed_count < max_burst;
         i++) {
        uint32_t cur_psn = (start_psn + i) & PSN_MASK;
        uint32_t index = cur_psn % MAX_PSN_ARRAY;
        if (!entry->mbuf_array[index]) {
            continue; // 该PSN没有报文
        }

        struct pkt_cache *pc = entry->mbuf_array[index];
        if (!pc || !pc->mbuf) {
            continue; // pkt_cache无效或mbuf为空
        }

        // 获取报文
        struct rte_mbuf *mbuf = pc->mbuf;
        const uint8_t *pkt_data = rte_pktmbuf_mtod(mbuf, const uint8_t *);
        const struct rocev2_packet *roce_pkt =
            (const struct rocev2_packet *)pkt_data;
        uint32_t cached_psn = get_psn_from_bth(&roce_pkt->bth);
        if (cached_psn != cur_psn) {
            printf("Warning: PSN mismatch at index %u (cached:0x%06X, "
                   "expected:0x%06X)\n",
                   index, cached_psn, cur_psn);
            destroy_pkt_cache(pc);
            entry->mbuf_array[index] = NULL;
            entry->packet_count--;
            continue;
        }

        // 检查报文是否过期
        uint64_t packet_age = current_time - pc->recv_stamp;
        if (packet_age > aging_threshold) {
            printf("Packet expired: PSN=0x%06X, age=%lu us\n", cur_psn,
                   packet_age * 1000000 / rte_get_timer_hz());
            destroy_pkt_cache(pc);
            entry->mbuf_array[index] = NULL;
            entry->packet_count--;
            continue;
        }

        // 克隆报文用于发送
        struct rte_mbuf *tx_mbuf =
            rte_pktmbuf_copy(mbuf, g_data.mbuf_pool, 0, UINT32_MAX);
        if (!tx_mbuf) {
            fprintf(stderr, "Failed to clone mbuf for retransmission\n");
            continue;
        }

        tx_burst[processed_count++] = tx_mbuf;
        entry->timestamp = current_time;
        printf("  Prepared PSN 0x%06X for retransmission\n", cur_psn);
    }

    return processed_count;
}

// 老化线程主函数
void *aging_thread_func(void *arg) {
    struct global_data *gd = (struct global_data *)arg;
    uint64_t current_time;
    uint64_t aging_threshold = AGING_INTERVAL * rte_get_timer_hz() / 1000;
    uint64_t rnr_timeout = RNR_TIMEOUT * rte_get_timer_hz() / 1000;
    uint64_t total_removed = 0;
    uint64_t last_aging_time = rte_get_timer_cycles();
    uint64_t last_print_time = rte_get_timer_cycles();

    printf("Aging Thread started, interval: %d ms\n", AGING_INTERVAL);

    while (!gd->stop) {
        // 等待老化间隔
        rte_delay_ms(AGING_INTERVAL);

        current_time = rte_get_timer_cycles();
        gd->aging_thread_last_active = current_time;

        for (uint32_t i = 0; i < gd->cache_tbl_v4->num_buckets; i++) {
            pthread_spin_lock(&gd->cache_tbl_v4->bucket_locks[i]);

            struct cache_entry_v4 **ppentry = &gd->cache_tbl_v4->buckets[i];
            while (*ppentry) {
                struct cache_entry_v4 *entry = *ppentry;
                pthread_spin_lock(&entry->lock);

                // 检查RNR超时
                if (entry->receiver_not_ready &&
                    current_time >= entry->rnr_timer) {
                    entry->receiver_not_ready = false;
                    entry->rnr_timer = 0;
                    printf("Connection: RNR timeout in aging thread\n");
                }

                if (current_time - entry->timestamp < aging_threshold) {
                    ppentry = &entry->next;
                    pthread_spin_unlock(&entry->lock);
                    continue;
                }

                cleanup_entry_timer(entry, gd);

                uint32_t packets_in_entry = entry->packet_count;
                for (int j = 0; j < MAX_PSN_ARRAY; j++) { // 释放所有缓存的报文
                    if (entry->mbuf_array[j]) {
                        destroy_pkt_cache(entry->mbuf_array[j]);
                        entry->mbuf_array[j] = NULL;
                    }
                }

                // 从哈希表中移除连接
                *ppentry = entry->next;
                pthread_spin_unlock(&entry->lock);
                pthread_spin_destroy(&entry->lock);
                rte_free(entry);

                gd->cache_tbl_v4->count--;
                total_removed++;
                printf("Aging: removed connection with %u packets\n",
                       packets_in_entry);
            }
            pthread_spin_unlock(&gd->cache_tbl_v4->bucket_locks[i]);

            // 每处理一个哈希桶老化，临时放权一下
            rte_delay_ms(CONN_AGE_PER_BUCKET_DELAY);
        }
    }

    printf("Aging Thread exited, total connections removed: %u\n",
           total_removed);
    return NULL;
}

// 重传线程主函数
void *sr_retrans_thread_func(void *arg) {
    struct global_data *gd = (struct global_data *)arg;
    uint64_t current_time;
    uint64_t retransmit_threshold =
        RETRANSMIT_INTERVAL * rte_get_timer_hz() / 1000;
    struct rte_mbuf *tx_burst[BURST_SIZE];
    uint16_t tx_count = 0;
    uint64_t total_retransmitted = 0;
    uint64_t last_print_time = rte_get_timer_cycles();

    printf("Retransmit Thread started, interval: %d ms\n", RETRANSMIT_INTERVAL);

    while (!gd->stop) {
        // 等待重传间隔
        rte_delay_ms(RETRANSMIT_INTERVAL);

        current_time = rte_get_timer_cycles();
        gd->sr_retrans_thread_last_active = current_time;
        tx_count = 0;

        // 查找需要重传的连接和报文
        for (uint32_t i = 0;
             i < gd->cache_tbl_v4->num_buckets && tx_count < BURST_SIZE; i++) {
            pthread_spin_lock(&gd->cache_tbl_v4->bucket_locks[i]);

            struct cache_entry_v4 *entry = gd->cache_tbl_v4->buckets[i];
            while (entry && tx_count < BURST_SIZE) {
                pthread_spin_lock(&entry->lock);

                // 检查是否需要重传（超时或标记需要重传）
                bool needs_retransmit = false;

                // 1. 检查超时重传
                if (current_time - entry->timestamp > retransmit_threshold &&
                    entry->packet_count > 0) {
                    needs_retransmit = true;
                }

                // 2. 检查是否被标记为需要重传（如收到NAK）
                if (entry->receiver_not_ready) {
                    // RNR状态，检查是否超时
                    if (current_time >= entry->rnr_timer) {
                        needs_retransmit = true;
                        entry->receiver_not_ready = false;
                        printf("Retransmit: RNR timeout, resending\n");
                    }
                }

                if (needs_retransmit) {
                    // 使用支持PSN翻转的遍历函数
                    uint32_t processed = traverse_psn_with_wrap(
                        entry, &tx_burst[tx_count], BURST_SIZE - tx_count,
                        current_time);

                    tx_count += processed;

                    if (processed > 0) {
                        printf(
                            "Retransmit: prepared %u packets from connection\n",
                            processed);
                    }
                }

                pthread_spin_unlock(&entry->lock);
                entry = entry->next;
            }

            pthread_spin_unlock(&gd->cache_tbl_v4->bucket_locks[i]);
        }

        // 批量发送报文
        if (tx_count > 0) {
            uint16_t nb_tx = rte_eth_tx_burst(gd->port_id, gd->tx_queue_id,
                                              tx_burst, tx_count);

            total_retransmitted += nb_tx;

            // 释放未发送成功的报文
            for (uint16_t j = nb_tx; j < tx_count; j++) {
                rte_pktmbuf_free(tx_burst[j]);
            }

            // 打印重传统计
            if (nb_tx > 0) {
                printf("Retransmit: sent %u/%u packets\n", nb_tx, tx_count);
            }
        }

        // 定期打印统计信息
        if (current_time - last_print_time >
            (rte_get_timer_hz() * 10)) { // 10秒
            printf("Retransmit thread: total retransmitted packets: %lu\n",
                   total_retransmitted);
            last_print_time = current_time;
        }
    }

    printf("Retransmit Thread exited, total retransmitted: %lu\n",
           total_retransmitted);
    return NULL;
}

// 等待所有线程结束
void wait_for_threads_exit(void) {
    // 等待线程的顺序很重要，应该按照依赖关系等待
    // 1. 首先等待处理线程

    if (g_data.rx_thread) {
        pthread_join(g_data.rx_thread, NULL);
        printf("RX thread joined\n");
        g_data.rx_thread = 0;
    }

    if (g_data.parse_thread) {
        pthread_join(g_data.parse_thread, NULL);
        printf("Parse thread joined\n");
        g_data.parse_thread = 0;
    }

    if (g_data.aging_thread) {
        pthread_join(g_data.aging_thread, NULL);
        printf("Aging thread joined\n");
        g_data.aging_thread = 0;
    }

    if (g_data.sr_retrans_thread) {
        pthread_join(g_data.sr_retrans_thread, NULL);
        printf("Retransmit thread joined\n");
        g_data.sr_retrans_thread = 0;
    }

    if (g_data.delay_proc_thread) {
        pthread_join(g_data.delay_proc_thread, NULL);
        printf("Delay processing thread joined\n");
        g_data.delay_proc_thread = 0;
    }

    if (g_data.timer_epoll_thread) {
        pthread_join(g_data.timer_epoll_thread, NULL);
        printf("Timer epoll thread joined\n");
        g_data.timer_epoll_thread = 0;
    }

    printf("All threads have been joined\n");
}

// 线程健康检查
int check_thread_health(void) {
    uint64_t current_time = rte_get_timer_cycles();
    uint64_t inactivity_threshold =
        THREAD_CHECK_INTERVAL * rte_get_timer_hz() / 1000;

    int healthy = 1;

    // 检查RX线程
    if (current_time - g_data.rx_thread_last_active > inactivity_threshold) {
        printf("WARNING: RX thread inactive for %lu seconds\n",
               (current_time - g_data.rx_thread_last_active) /
                   rte_get_timer_hz());
        healthy = 0;
    }

    // 检查解析线程
    if (current_time - g_data.parse_thread_last_active > inactivity_threshold) {
        printf("WARNING: Parse thread inactive for %lu seconds\n",
               (current_time - g_data.parse_thread_last_active) /
                   rte_get_timer_hz());
        healthy = 0;
    }

    // 检查老化线程
    if (current_time - g_data.aging_thread_last_active > inactivity_threshold) {
        printf("WARNING: Aging thread inactive for %lu seconds\n",
               (current_time - g_data.aging_thread_last_active) /
                   rte_get_timer_hz());
        healthy = 0;
    }

    // 检查重传线程
    if (current_time - g_data.sr_retrans_thread_last_active >
        inactivity_threshold) {
        printf("WARNING: Retransmit thread inactive for %lu seconds\n",
               (current_time - g_data.sr_retrans_thread_last_active) /
                   rte_get_timer_hz());
        healthy = 0;
    }

    // 检查延时处理线程
    if (g_data.delay_proc_thread) {
        if (current_time - g_data.delay_proc_thread_last_active >
            inactivity_threshold) {
            printf(
                "WARNING: Delay processing thread inactive for %lu seconds\n",
                (current_time - g_data.delay_proc_thread_last_active) /
                    rte_get_timer_hz());
            healthy = 0;
        }
    }

    return healthy;
}

// 检查线程是否存活
bool is_thread_alive(pthread_t thread) {
    // 尝试向线程发送信号0（不实际发送信号，只检查线程是否存在）
    int result = pthread_kill(thread, 0);

    if (result == 0) {
        return true; // 线程存活
    } else if (result == ESRCH) {
        return false; // 线程不存在
    } else {
        printf("Error checking thread status: %d\n", result);
        return false;
    }
}

// 重启失败的线程
static int restart_thread(pthread_t *thread, void *(*start_routine)(void *),
                          const char *thread_name, cpu_set_t *cpuset) {
    pthread_attr_t attr;

    if (pthread_attr_init(&attr) != 0) {
        fprintf(stderr, "Failed to init thread attr for %s\n", thread_name);
        return ERR;
    }

    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

    if (cpuset) {
        pthread_attr_setaffinity_np(&attr, sizeof(cpu_set_t), cpuset);
    }

    int ret = pthread_create(thread, &attr, start_routine, &g_data);
    pthread_attr_destroy(&attr);

    if (ret != 0) {
        fprintf(stderr, "Failed to restart %s thread\n", thread_name);
        return ERR;
    }

    printf("Successfully restarted %s thread\n", thread_name);
    return OK;
}

// 监控并重启失败的线程
int monitor_and_restart_threads(void) {
    int restarted_count = 0;
    cpu_set_t cpuset;

    // 检查并重启RX线程
    if (g_data.rx_thread && !is_thread_alive(g_data.rx_thread)) {
        printf("RX thread died, attempting to restart...\n");
        CPU_ZERO(&cpuset);
        CPU_SET(0, &cpuset);

        if (restart_thread(&g_data.rx_thread, rx_thread_func, "RX", &cpuset) ==
            OK) {
            restarted_count++;
        }
    }

    // 检查并重启解析线程
    if (g_data.parse_thread && !is_thread_alive(g_data.parse_thread)) {
        printf("Parse thread died, attempting to restart...\n");
        CPU_ZERO(&cpuset);
        CPU_SET(1, &cpuset);

        if (restart_thread(&g_data.parse_thread, parse_thread_func, "Parse",
                           &cpuset) == OK) {
            restarted_count++;
        }
    }

    // 检查并重启老化线程
    if (g_data.aging_thread && !is_thread_alive(g_data.aging_thread)) {
        printf("Aging thread died, attempting to restart...\n");
        CPU_ZERO(&cpuset);
        CPU_SET(2, &cpuset);

        if (restart_thread(&g_data.aging_thread, aging_thread_func, "Aging",
                           &cpuset) == OK) {
            restarted_count++;
        }
    }

    // 检查并重启重传线程
    if (g_data.sr_retrans_thread &&
        !is_thread_alive(g_data.sr_retrans_thread)) {
        printf("Retransmit thread died, attempting to restart...\n");
        CPU_ZERO(&cpuset);
        CPU_SET(3, &cpuset);

        if (restart_thread(&g_data.sr_retrans_thread, sr_retrans_thread_func,
                           "Retransmit", &cpuset) == OK) {
            restarted_count++;
        }
    }

    return restarted_count;
}

// 获取线程CPU使用率（简化版本）
float get_thread_cpu_usage(pthread_t thread, const char *thread_name) {
    // TODO: 实现实际的CPU使用率计算
    // 这里返回一个占位符值
    static int counter = 0;
    counter++;

    // 模拟不同的CPU使用率
    float usage = 0.0f;

    if (strcmp(thread_name, "RX") == 0) {
        usage = 15.0f + (counter % 10); // 15-25%
    } else if (strcmp(thread_name, "Parse") == 0) {
        usage = 20.0f + (counter % 15); // 20-35%
    } else if (strcmp(thread_name, "Aging") == 0) {
        usage = 5.0f + (counter % 5); // 5-10%
    } else if (strcmp(thread_name, "Retransmit") == 0) {
        usage = 10.0f + (counter % 10); // 10-20%
    }

    return usage;
}

// 打印线程状态信息
void print_thread_status(void) {
    printf("\n=== Thread Status ===\n");

    printf("RX Thread: ");
    if (g_data.rx_thread && is_thread_alive(g_data.rx_thread)) {
        uint64_t inactive_time =
            (rte_get_timer_cycles() - g_data.rx_thread_last_active) /
            rte_get_timer_hz();
        printf("ALIVE, inactive for %lu seconds\n", inactive_time);
    } else {
        printf("DEAD or NOT STARTED\n");
    }

    printf("Parse Thread: ");
    if (g_data.parse_thread && is_thread_alive(g_data.parse_thread)) {
        uint64_t inactive_time =
            (rte_get_timer_cycles() - g_data.parse_thread_last_active) /
            rte_get_timer_hz();
        printf("ALIVE, inactive for %lu seconds\n", inactive_time);
    } else {
        printf("DEAD or NOT STARTED\n");
    }

    printf("Aging Thread: ");
    if (g_data.aging_thread && is_thread_alive(g_data.aging_thread)) {
        uint64_t inactive_time =
            (rte_get_timer_cycles() - g_data.aging_thread_last_active) /
            rte_get_timer_hz();
        printf("ALIVE, inactive for %lu seconds\n", inactive_time);
    } else {
        printf("DEAD or NOT STARTED\n");
    }

    printf("Retransmit Thread: ");
    if (g_data.sr_retrans_thread && is_thread_alive(g_data.sr_retrans_thread)) {
        uint64_t inactive_time =
            (rte_get_timer_cycles() - g_data.sr_retrans_thread_last_active) /
            rte_get_timer_hz();
        printf("ALIVE, inactive for %lu seconds\n", inactive_time);
    } else {
        printf("DEAD or NOT STARTED\n");
    }

    printf("Delay Processing Thread: ");
    if (g_data.delay_proc_thread && is_thread_alive(g_data.delay_proc_thread)) {
        uint64_t inactive_time =
            (rte_get_timer_cycles() - g_data.delay_proc_thread_last_active) /
            rte_get_timer_hz();
        printf("ALIVE, inactive for %lu seconds\n", inactive_time);
    } else {
        printf("DEAD or NOT STARTED\n");
    }

    printf("Timer Epoll Thread: ");
    if (g_data.timer_epoll_thread &&
        is_thread_alive(g_data.timer_epoll_thread)) {
        printf("ALIVE\n");
    } else {
        printf("DEAD or NOT STARTED\n");
    }

    printf("=====================\n\n");
}

// 优雅停止所有线程
void graceful_stop_all_threads(void) {
    printf("Initiating graceful shutdown of all threads...\n");

    // 设置停止标志
    g_data.stop = 1;

    // 给线程一些时间完成当前工作
    sleep(1);

    // 强制终止任何仍然运行的线程（最后的手段）
    if (g_data.rx_thread && is_thread_alive(g_data.rx_thread)) {
        printf("RX thread still alive, forcing termination...\n");
        pthread_cancel(g_data.rx_thread);
    }

    if (g_data.parse_thread && is_thread_alive(g_data.parse_thread)) {
        printf("Parse thread still alive, forcing termination...\n");
        pthread_cancel(g_data.parse_thread);
    }

    if (g_data.aging_thread && is_thread_alive(g_data.aging_thread)) {
        printf("Aging thread still alive, forcing termination...\n");
        pthread_cancel(g_data.aging_thread);
    }

    if (g_data.sr_retrans_thread && is_thread_alive(g_data.sr_retrans_thread)) {
        printf("Retransmit thread still alive, forcing termination...\n");
        pthread_cancel(g_data.sr_retrans_thread);
    }

    if (g_data.delay_proc_thread && is_thread_alive(g_data.delay_proc_thread)) {
        printf("Delay processing thread still alive, forcing termination...\n");
        pthread_cancel(g_data.delay_proc_thread);
    }

    if (g_data.timer_epoll_thread &&
        is_thread_alive(g_data.timer_epoll_thread)) {
        printf("Timer epoll thread still alive, forcing termination...\n");
        pthread_cancel(g_data.timer_epoll_thread);
    }

    // 等待线程真正结束
    wait_for_threads_exit();

    printf("All threads have been stopped\n");
}
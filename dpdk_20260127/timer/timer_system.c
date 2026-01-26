#include "timer/timer_system.h"
#include "global.h"
#include "processing/packet_processing.h"
#include "tables/connection_table.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <unistd.h>

#define MAX_EPOLL_EVENTS 128

// 创建定时器（分配独立内存携带key和任务信息）
int create_timer_with_info(struct global_data *gd,
                           enum delay_task_type task_type,
                           const struct cache_key_v4 *key, uint32_t psn,
                           uint32_t delay_ms) {
    // 分配任务信息内存（独立于entry）
    struct delay_task_info *task_info =
        rte_zmalloc("DELAY_TASK_INFO", sizeof(struct delay_task_info), 0);
    if (!task_info) {
        fprintf(stderr, "Failed to allocate delay task info\n");
        return ERR;
    }

    // 初始化任务信息
    task_info->type = task_type;
    memcpy(&task_info->key, key, sizeof(struct cache_key_v4));

    // 创建timerfd（一次性定时器，非周期循环定时器）
    int timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (timer_fd < 0) {
        perror("timerfd_create failed");
        rte_free(task_info);
        return ERR;
    }
    task_info->timer_fd = timer_fd;
    task_info->psn = psn;

    // 设置定时器时间
    struct itimerspec its = {0};
    its.it_value.tv_sec = delay_ms / 1000;
    its.it_value.tv_nsec = (delay_ms % 1000) * 1000000L;

    if (timerfd_settime(timer_fd, 0, &its, NULL) < 0) {
        perror("timerfd_settime failed");
        close(timer_fd);
        rte_free(task_info);
        return ERR;
    }

    // 将timerfd加入epoll监控（携带任务信息指针）
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET;
    ev.data.ptr = task_info;

    if (epoll_ctl(gd->timer_epoll_fd, EPOLL_CTL_ADD, timer_fd, &ev) < 0) {
        perror("epoll_ctl add failed");
        close(timer_fd);
        rte_free(task_info);
        return ERR;
    }

    printf("Created timer: type=%d, delay=%ums, timer_fd=%d, param1=0x%08X\n",
           task_type, delay_ms, timer_fd, param1);
    return timer_fd; // 返回timer_fd，调用者需要保存到entry中
}

// 修改定时器时间
int modify_timer_time(int timer_fd, uint32_t new_delay_ms) {
    if (timer_fd < 0) {
        return ERR;
    }

    struct itimerspec its = {0};
    its.it_value.tv_sec = new_delay_ms / 1000;
    its.it_value.tv_nsec = (new_delay_ms % 1000) * 1000000L;

    if (timerfd_settime(timer_fd, 0, &its, NULL) < 0) {
        perror("timerfd_settime modify failed");
        return ERR;
    }
    return OK;
}

// 为entry创建RNR重传定时器（线程安全）
int create_rnr_timer(struct global_data *gd, struct cache_entry_v4 *entry,
                     uint32_t nak_psn) {
    // 如果已有定时器，先清理
    if (entry->rnr_timer_fd >= 0) {
        epoll_ctl(gd->timer_epoll_fd, EPOLL_CTL_DEL, entry->rnr_timer_fd, NULL);
        close(entry->rnr_timer_fd);
        entry->rnr_timer_fd = -1;
    }

    // 创建定时器（携带独立的任务信息）
    int timer_fd = create_timer_with_info(gd, DELAY_TASK_RNR_RETRY, &entry->key,
                                          nak_psn, RNR_TIMEOUT);
    if (timer_fd < 0) {
        return ERR;
    }

    // 保存到entry中
    entry->rnr_timer_fd = timer_fd;
    entry->rnr_retry_psn = entry->cur_psn;
    entry->rnr_retry_count++;

    printf("RNR timer created: entry=%p, fd=%d, delay=%ums, retry_count=%u, "
           "PSN=0x%06X\n",
           (void *)entry, timer_fd, delay_ms, entry->rnr_retry_count,
           entry->rnr_retry_psn);
    return OK;
}

// 定时器事件处理线程（处理所有定时器到期事件）
void *timer_epoll_thread_func(void *arg) {
    struct global_data *gd = (struct global_data *)arg;
    struct epoll_event events[MAX_EPOLL_EVENTS];
    uint64_t event_count = 0;

    printf("Timer epoll thread started (epoll_fd=%d)\n", gd->timer_epoll_fd);

    while (!gd->stop) {
        // 等待定时器事件，超时时间100ms
        int nfds =
            epoll_wait(gd->timer_epoll_fd, events, MAX_EPOLL_EVENTS, 100);

        if (nfds < 0) {
            if (errno == EINTR) {
                // 被信号中断，继续循环
                continue;
            }
            perror("epoll_wait failed");
            break;
        }

        for (int i = 0; i < nfds; i++) {
            struct delay_task_info *task_info =
                (struct delay_task_info *)events[i].data.ptr;
            if (!task_info)
                continue;

            // 读取定时器到期事件
            uint64_t expirations;
            ssize_t s =
                read(task_info->timer_fd, &expirations, sizeof(expirations));

            if (s == sizeof(expirations)) {
                event_count++;

                // 将任务放入延时任务队列
                int ret = rte_ring_enqueue(gd->delay_task_ring, task_info);
                if (ret != 0) {
                    // 队列满，记录日志并释放任务信息
                    printf("WARNING: Delay task ring full, discarding task "
                           "type=%d\n",
                           task_info->type);

                    // 从epoll中移除定时器
                    epoll_ctl(gd->timer_epoll_fd, EPOLL_CTL_DEL,
                              task_info->timer_fd, NULL);
                    close(task_info->timer_fd);
                    rte_free(task_info);
                } else {
                    printf("Timer event %lu: task_type=%d added to queue\n",
                           event_count, task_info->type);
                }
            } else if (s < 0) {
                // 读取错误
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // 非阻塞读取，没有数据
                    continue;
                }
                printf("ERROR: Failed to read timer expiration: %s\n",
                       strerror(errno));

                // 清理资源
                epoll_ctl(gd->timer_epoll_fd, EPOLL_CTL_DEL,
                          task_info->timer_fd, NULL);
                close(task_info->timer_fd);
                rte_free(task_info);
            } else {
                // 读取到不完整的字节数
                printf("WARNING: Incomplete read from timer_fd %d\n",
                       task_info->timer_fd);
                epoll_ctl(gd->timer_epoll_fd, EPOLL_CTL_DEL,
                          task_info->timer_fd, NULL);
                close(task_info->timer_fd);
                rte_free(task_info);
            }
        }
    }

    printf("Timer epoll thread exited, total events: %lu\n", event_count);
    return NULL;
}

// 处理延时的RNR重传任务
static void process_delayed_rnr_retry(struct global_data *gd,
                                      struct delay_task_info *task_info) {
    struct cache_key_v4 *key = &task_info->key;
    uint32_t hash = cache_hash_v4_func(key);
    uint32_t bucket_idx = hash % gd->cache_tbl_v4->num_buckets;
    struct cache_entry_v4 *found_entry = NULL;

    pthread_spin_lock(&gd->cache_tbl_v4->bucket_locks[bucket_idx]);

    // 查找连接条目
    struct cache_entry_v4 *entry = gd->cache_tbl_v4->buckets[bucket_idx];
    while (entry) {
        if (memcmp(&entry->key, key, sizeof(struct cache_key_v4)) == 0) {
            found_entry = entry;
            break;
        }
        entry = entry->next;
    }

    if (!found_entry) {
        printf("  Connection aged out, discarding delayed RNR retry task\n");
        pthread_spin_unlock(&gd->cache_tbl_v4->bucket_locks[bucket_idx]);
        return;
    }

    pthread_spin_lock(&found_entry->lock);

    // 检查连接是否仍然需要RNR重传
    if (!found_entry->receiver_not_ready) {
        printf("  Connection no longer in RNR state, ignoring delayed retry\n");
        pthread_spin_unlock(&found_entry->lock);
        pthread_spin_unlock(&gd->cache_tbl_v4->bucket_locks[bucket_idx]);
        return;
    }

    // 检查定时器是否匹配（防止处理旧定时器）
    if (found_entry->rnr_timer_fd != task_info->timer_fd) {
        printf("  Timer fd mismatch (entry_fd=%d, task_fd=%d), ignoring "
               "delayed RNR retry\n",
               found_entry->rnr_timer_fd, task_info->timer_fd);
        pthread_spin_unlock(&found_entry->lock);
        pthread_spin_unlock(&gd->cache_tbl_v4->bucket_locks[bucket_idx]);
        return;
    }
    // 清理entry中的定时器fd
    found_entry->rnr_timer_fd = -1;

    // 执行GBN重传
    struct rte_mbuf *tx_burst[BURST_SIZE];
    uint32_t processed =
        build_gbn_retrans(found_entry, tx_burst, BURST_SIZE, task_info->param1);

    if (processed > 0) {
        uint16_t nb_tx =
            rte_eth_tx_burst(gd->port_id, gd->tx_queue_id, tx_burst, processed);

        // 释放未发送成功的报文
        for (uint16_t i = nb_tx; i < processed; i++) {
            rte_pktmbuf_free(tx_burst[i]);
        }
        printf("  Delayed RNR retransmission: sent %u/%u packets\n", nb_tx,
               processed);

        // 更新连接状态
        if (nb_tx > 0) {
            found_entry->receiver_not_ready = false;
            found_entry->timestamp = rte_get_timer_cycles();
        }
    } else {
        printf("  No packets to retransmit for delayed RNR retry\n");
    }

    pthread_spin_unlock(&found_entry->lock);
    pthread_spin_unlock(&gd->cache_tbl_v4->bucket_locks[bucket_idx]);
}

// 处理延时的NAK重传任务
static void process_delayed_nak_retry(struct global_data *gd,
                                      struct delay_task_info *task_info) {
    // 后面扩展V6时，下面内容封装成V4子函数，根据类型调用V4/V6子函数
    struct cache_key_v4 *key = &task_info->key;
    uint32_t hash = cache_hash_v4_func(key);
    uint32_t bucket_idx = hash % gd->cache_tbl_v4->num_buckets;
    struct cache_entry_v4 *found_entry = NULL;

    pthread_spin_lock(&gd->cache_tbl_v4->bucket_locks[bucket_idx]);

    // 查找连接条目
    struct cache_entry_v4 *entry = gd->cache_tbl_v4->buckets[bucket_idx];
    while (entry) {
        if (memcmp(&entry->key, key, sizeof(struct cache_key_v4)) == 0) {
            found_entry = entry;
            break;
        }
        entry = entry->next;
    }

    if (!found_entry) {
        printf("  Connection aged out, discarding delayed NAK retry task\n");
        pthread_spin_unlock(&gd->cache_tbl_v4->bucket_locks[bucket_idx]);
        return;
    }

    pthread_spin_lock(&found_entry->lock);

    send_sr_request_v4(gd, entry, task_info->nak_psn);

    found_entry->timestamp = rte_get_timer_cycles();
    pthread_spin_unlock(&found_entry->lock);
    pthread_spin_unlock(&gd->cache_tbl_v4->bucket_locks[bucket_idx]);
}

// 统一延时处理线程（处理所有类型的延时任务）
void *delay_proc_thread_func(void *arg) {
    struct global_data *gd = (struct global_data *)arg;
    struct delay_task_info *task_infos[BURST_SIZE]; // 批量读取
    unsigned int nb_deq;
    uint64_t processed_count = 0;
    uint64_t rnr_tasks = 0;
    uint64_t nak_tasks = 0;

    printf("Delay processing thread started\n");
    while (!gd->stop) {
        // 从延时任务队列批量读取任务
        nb_deq = rte_ring_dequeue_burst(gd->delay_task_ring,
                                        (void **)task_infos, BURST_SIZE, NULL);

        // 更新线程活动时间
        gd->delay_proc_thread_last_active = rte_get_timer_cycles();
        if (nb_deq == 0) {
            usleep(2000); // 2ms
            continue;
        }

        processed_count += nb_deq;
        for (unsigned int i = 0; i < nb_deq; i++) {
            struct delay_task_info *task_info = task_infos[i];
            if (!task_info)
                continue;

            printf("Processing delay task %lu: type=%d\n", processed_count,
                   task_info->type);

            switch (task_info->type) {
            case DELAY_TASK_RNR_RETRY:
                process_delayed_rnr_retry(gd, task_info);
                rnr_tasks++;
                break;
            case DELAY_TASK_NAK_RETRY:
                process_delayed_nak_retry(gd, task_info);
                nak_tasks++;
                break;
            default:
                printf("WARNING: Unknown delay task type: %d\n",
                       task_info->type);
                break;
            }

            if (task_info->timer_fd >= 0) {
                close(task_info->timer_fd);
            }
            rte_free(task_info);
        }
    }
    printf("Delay processing thread exited, total tasks: %lu\n",
           processed_count);
    return NULL;
}

// 初始化延时处理系统
int init_delay_processing_system(struct global_data *gd) {
    // 1. 创建延时任务无锁队列
    gd->delay_task_ring =
        rte_ring_create("delay_task_ring", USR_RING_SIZE, rte_socket_id(),
                        RING_F_SP_ENQ | RING_F_SC_DEQ);
    if (!gd->delay_task_ring) {
        fprintf(stderr, "Failed to create delay task ring\n");
        return ERR;
    }

    // 2. 创建epoll实例用于定时器
    gd->timer_epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (gd->timer_epoll_fd < 0) {
        perror("epoll_create1 failed");
        rte_ring_free(gd->delay_task_ring);
        gd->delay_task_ring = NULL;
        return ERR;
    }

    gd->timer_system_inited = 1;
    printf("Delay processing system initialized: epoll_fd=%d, ring_size=%d\n",
           gd->timer_epoll_fd, USR_RING_SIZE);
    return OK;
}

// 清理延时处理系统
void cleanup_delay_processing_system(void) {
    if (!g_data.timer_system_inited)
        return;

    printf("Cleaning up delay processing system...\n");

    // 1. 清理任务队列中剩余的任务
    struct delay_task_info *task_info;
    unsigned int count = 0;
    while (rte_ring_dequeue(g_data.delay_task_ring, (void **)&task_info) == 0) {
        if (task_info) {
            // 关闭关联的定时器fd（如果还存在）
            if (task_info->timer_fd >= 0) {
                close(task_info->timer_fd);
            }
            rte_free(task_info);
            count++;
        }
    }
    if (count > 0) {
        printf("Cleaned %u pending delay tasks from ring\n", count);
    }

    // 2. 关闭epoll实例
    if (g_data.timer_epoll_fd >= 0) {
        close(g_data.timer_epoll_fd);
        g_data.timer_epoll_fd = -1;
    }

    // 3. 释放环形队列
    if (g_data.delay_task_ring) {
        rte_ring_free(g_data.delay_task_ring);
        g_data.delay_task_ring = NULL;
    }

    g_data.timer_system_inited = 0;
    printf("Delay processing system cleanup completed\n");
}

// 取消定时器
int cancel_timer(struct global_data *gd, int timer_fd) {
    if (timer_fd < 0) {
        return ERR;
    }

    // 从epoll中移除定时器
    if (epoll_ctl(gd->timer_epoll_fd, EPOLL_CTL_DEL, timer_fd, NULL) < 0) {
        // 如果定时器已经被移除，这不是错误
        if (errno != ENOENT) {
            perror("epoll_ctl del failed");
            return ERR;
        }
    }

    // 关闭定时器文件描述符
    if (close(timer_fd) < 0) {
        perror("close timer_fd failed");
        return ERR;
    }

    printf("Timer cancelled: fd=%d\n", timer_fd);
    return OK;
}

// 取消连接的所有定时器
int cancel_all_timers_for_entry(struct cache_entry_v4 *entry,
                                struct global_data *gd) {
    if (!entry)
        return ERR;

    int cancelled_count = 0;

    // 取消RNR定时器
    if (entry->rnr_timer_fd >= 0) {
        if (cancel_timer(gd, entry->rnr_timer_fd) == OK) {
            entry->rnr_timer_fd = -1;
            entry->rnr_retry_count = 0;
            cancelled_count++;
        }
    }

    printf("Cancelled %d timers for connection entry\n", cancelled_count);
    return cancelled_count > 0 ? OK : ERR;
}

// 检查定时器是否有效
bool is_timer_valid(int timer_fd) {
    if (timer_fd < 0)
        return false;

    // 尝试读取定时器状态
    struct itimerspec curr_value;
    if (timerfd_gettime(timer_fd, &curr_value) == 0) {
        return true;
    }

    return false;
}

// 获取定时器剩余时间（毫秒）
uint32_t get_timer_remaining_time(int timer_fd) {
    if (timer_fd < 0)
        return 0;

    struct itimerspec curr_value;
    if (timerfd_gettime(timer_fd, &curr_value) < 0) {
        return 0;
    }

    // 计算剩余时间
    uint64_t remaining_ns =
        curr_value.it_value.tv_sec * 1000000000UL + curr_value.it_value.tv_nsec;

    return (uint32_t)(remaining_ns / 1000000UL); // 转换为毫秒
}

// 打印定时器系统状态
void print_timer_system_status(struct global_data *gd) {
    printf("\n=== Timer System Status ===\n");

    printf("Initialized: %s\n", gd->timer_system_inited ? "Yes" : "No");
    printf("Epoll FD: %d\n", gd->timer_epoll_fd);

    if (gd->delay_task_ring) {
        uint32_t count = rte_ring_count(gd->delay_task_ring);
        uint32_t free_count = rte_ring_free_count(gd->delay_task_ring);
        printf("Delay Task Ring: count=%u, free=%u, capacity=%u\n", count,
               free_count, USR_RING_SIZE);
    } else {
        printf("Delay Task Ring: Not initialized\n");
    }

    // 统计活动定时器数量（通过遍历连接表）
    uint32_t active_timers = 0;
    if (gd->cache_tbl_v4) {
        for (uint32_t i = 0; i < gd->cache_tbl_v4->num_buckets; i++) {
            pthread_spin_lock(&gd->cache_tbl_v4->bucket_locks[i]);

            struct cache_entry_v4 *entry = gd->cache_tbl_v4->buckets[i];
            while (entry) {
                pthread_spin_lock(&entry->lock);
                if (entry->rnr_timer_fd >= 0 &&
                    is_timer_valid(entry->rnr_timer_fd)) {
                    active_timers++;
                }
                pthread_spin_unlock(&entry->lock);
                entry = entry->next;
            }

            pthread_spin_unlock(&gd->cache_tbl_v4->bucket_locks[i]);
        }
    }

    printf("Active Timers: %u\n", active_timers);
    printf("============================\n\n");
}

// 创建延时处理NAK定时器
int create_nak_delay_timer(struct global_data *gd,
                           const struct cache_entry_v4 *entry,
                           uint32_t nak_psn) {
    // 如果已有定时器，先清理
    if (entry->delay_nak_timer_fd >= 0) {
        epoll_ctl(gd->timer_epoll_fd, EPOLL_CTL_DEL, entry->delay_nak_timer_fd,
                  NULL);
        close(entry->delay_nak_timer_fd);
        entry->delay_nak_timer_fd = -1;
    }

    int timer_fd = create_timer_with_info(gd, DELAY_TASK_NAK_RETRY, entry->key,
                                          nak_psn, DELAY_NAK_INTERVAL);
    if (timer_fd >= 0) {
        printf("NAK delay retry scheduled: delay=%ums, PSN=0x%06X\n", delay_ms,
               nak_psn);
        return OK;
    }
    return ERR;
}

void update_nak_delay_timer(struct cache_entry_v4 *entry) {
    // 只有目的网关才会发送SR重传请求
    if (entry->role != DST_GATEWAY || entry->delay_nak_timer_fd <= 0) {
        return;
    }
    modify_timer_time(entry->delay_nak_timer_fd, DELAY_NAK_INTERVAL);
}

// 清理所有待处理的定时器任务
void cleanup_all_pending_timers(struct global_data *gd) {
    printf("Cleaning up all pending timers...\n");

    if (!gd->timer_system_inited) {
        printf("Timer system not initialized\n");
        return;
    }

    // 1. 首先清理epoll中的所有定时器
    struct epoll_event events[MAX_EPOLL_EVENTS];
    int nfds = epoll_wait(gd->timer_epoll_fd, events, MAX_EPOLL_EVENTS, 0);

    for (int i = 0; i < nfds; i++) {
        struct delay_task_info *task_info =
            (struct delay_task_info *)events[i].data.ptr;
        if (task_info) {
            epoll_ctl(gd->timer_epoll_fd, EPOLL_CTL_DEL, task_info->timer_fd,
                      NULL);
            close(task_info->timer_fd);
            rte_free(task_info);
        }
    }

    // 2. 清理延时任务队列
    struct delay_task_info *task_info;
    uint32_t count = 0;
    while (rte_ring_dequeue(gd->delay_task_ring, (void **)&task_info) == 0) {
        if (task_info) {
            if (task_info->timer_fd >= 0) {
                close(task_info->timer_fd);
            }
            rte_free(task_info);
            count++;
        }
    }

    printf("Cleaned up %u pending timers\n", count);
}

// 为定时器系统设置性能参数
int set_timer_system_parameters(struct global_data *gd, uint32_t max_timers,
                                uint32_t timer_precision_ms) {
    // 这里可以扩展实现定时器系统的性能调优参数
    // 例如调整epoll的超时时间、定时器的精度等

    printf("Timer system parameters: max_timers=%u, precision=%ums\n",
           max_timers, timer_precision_ms);

    // TODO: 实际设置定时器系统的性能参数
    // 这可能涉及调整内核参数或使用更高效的定时器实现

    return OK;
}
#include "global.h"
#include "tables/connection_table.h"
#include "utils/utils.h"
#include <stdio.h>
#include <string.h>

// 创建pkt_cache结构
struct pkt_cache *create_pkt_cache(uint32_t psn, struct rte_mbuf *mbuf) {
    struct pkt_cache *pc =
        rte_zmalloc("pkt_cache", sizeof(struct pkt_cache), 0);
    if (!pc) {
        return NULL;
    }

    pc->psn = psn;
    pc->recv_stamp = rte_get_timer_cycles();
    pc->mbuf = mbuf;

    return pc;
}

// 销毁pkt_cache结构
void destroy_pkt_cache(struct pkt_cache *pc) {
    if (!pc)
        return;

    if (pc->mbuf) {
        rte_pktmbuf_free(pc->mbuf);
        pc->mbuf = NULL;
    }

    rte_free(pc);
}

// 创建新的连接条目
static struct cache_entry_v4 *
create_cache_entry_v4(const struct cache_key_v4 *key) {
    struct cache_entry_v4 *entry = rte_zmalloc(
        "cache_entry_v4", sizeof(struct cache_entry_v4), RTE_CACHE_LINE_SIZE);
    if (!entry)
        return NULL;

    memcpy(&entry->key, key, sizeof(struct cache_key_v4));
    memset(entry->mbuf_array, 0, sizeof(entry->mbuf_array));

    entry->start_psn = PSN_INVALID;
    entry->end_psn = PSN_INVALID;
    entry->cur_psn = PSN_INVALID;
    entry->timestamp = rte_get_timer_cycles();
    entry->packet_count = 0;
    entry->receiver_not_ready = false;
    entry->rnr_timer = 0;
    entry->rnr_timer_fd = -1;
    entry->rnr_retry_psn = PSN_INVALID;
    entry->rnr_retry_count = 0;
    entry->next = NULL;

    if (pthread_spin_init(&entry->lock, PTHREAD_PROCESS_PRIVATE) != 0) {
        fprintf(stderr, "Failed to init entry spinlock\n");
        rte_free(entry);
        return NULL;
    }

    return entry;
}

// 插入报文到连接条目的PSN数组（考虑PSN翻转）
static int insert_packet_to_cache_v4(struct cache_entry_v4 *entry,
                                     struct rte_mbuf *mbuf, uint32_t psn) {
    uint32_t array_idx = psn % MAX_PSN_ARRAY;

    // 检查是否已存在该PSN的报文
    if (entry->mbuf_array[array_idx]) {
        // 已存在，释放旧报文缓存，创建新的pkt_cache
        destroy_pkt_cache(entry->mbuf_array[array_idx]);
        struct pkt_cache *pc = create_pkt_cache(psn, mbuf);
        if (!pc) {
            return ERR;
        }

        entry->mbuf_array[array_idx] = pc;
        return OK; // 更新成功
    }

    // 新PSN，创建pkt_cache并插入
    struct pkt_cache *pc = create_pkt_cache(psn, mbuf);
    if (!pc) {
        return ERR;
    }

    entry->mbuf_array[array_idx] = pc;
    entry->packet_count++;

    // 处理PSN范围更新（考虑翻转）
    if (entry->packet_count == 1) {
        entry->start_psn = psn;
        entry->end_psn = psn;
        entry->cur_psn = psn;
    } else {
        // 判断PSN是否在现有范围内（考虑翻转）
        int32_t diff_from_start = (psn - entry->start_psn) & PSN_MASK;
        int32_t diff_from_end = (psn - entry->end_psn) & PSN_MASK;

        if (diff_from_start > PSN_HALF_CYCLE) {
            // psn在start_psn之前（考虑翻转）
            entry->start_psn = psn;
        } else if (diff_from_end < PSN_HALF_CYCLE && diff_from_end > 0) {
            // psn在end_psn之后（考虑翻转）
            entry->end_psn = psn;
        }
        // else: psn在范围内
        entry->cur_psn = psn; // 更新当前PSN
    }
    return OK;
}

// 连接老化检查（递归二分法批量老化）
static uint32_t aging_check_binary(struct cache_entry_v4 *entry,
                                   uint32_t start_psn, uint32_t end_psn,
                                   uint64_t aging_threshold) {
    if (start_psn == end_psn) {
        // 单个PSN检查
        uint32_t idx = start_psn % MAX_PSN_ARRAY;
        if (entry->mbuf_array[idx]) {
            uint64_t packet_age =
                rte_get_timer_cycles() - entry->mbuf_array[idx]->recv_stamp;
            if (packet_age > aging_threshold) {
                destroy_pkt_cache(entry->mbuf_array[idx]);
                entry->mbuf_array[idx] = NULL;
                entry->packet_count--;
                return 1;
            }
        }
        return 0;
    }

    // 计算中间PSN
    uint32_t distance;
    if (end_psn >= start_psn) {
        distance = end_psn - start_psn;
    } else {
        distance = (PSN_MASK - start_psn + 1) + (end_psn + 1);
    }

    uint32_t mid_psn = (start_psn + distance / 2) & PSN_MASK;

    // 检查中间PSN是否需要老化
    uint32_t idx = mid_psn % MAX_PSN_ARRAY;
    uint32_t aged_count = 0;

    if (entry->mbuf_array[idx]) {
        uint64_t packet_age =
            rte_get_timer_cycles() - entry->mbuf_array[idx]->recv_stamp;
        if (packet_age > aging_threshold) {
            // 批量老化 start_psn ~ mid_psn
            uint32_t current_psn = start_psn;
            for (uint32_t i = 0; i <= distance / 2; i++) {
                idx = current_psn % MAX_PSN_ARRAY;
                if (entry->mbuf_array[idx]) {
                    destroy_pkt_cache(entry->mbuf_array[idx]);
                    entry->mbuf_array[idx] = NULL;
                    entry->packet_count--;
                    aged_count++;
                }
                current_psn = (current_psn + 1) & PSN_MASK;
            }
            return aged_count;
        }
    }

    // 继续二分检查剩余部分
    uint32_t next_mid_psn = (mid_psn + 1) & PSN_MASK;
    if (next_mid_psn != end_psn) {
        aged_count +=
            aging_check_binary(entry, next_mid_psn, end_psn, aging_threshold);
    }

    return aged_count;
}

// 初始化连接表
struct cache_hash_v4 *cache_tbl_v4_create(uint32_t num_buckets) {
    struct cache_hash_v4 *ht = rte_zmalloc(
        "cache_tbl_v4", sizeof(struct cache_hash_v4), RTE_CACHE_LINE_SIZE);
    if (!ht) {
        fprintf(stderr, "Failed to allocate cache_hash_v4\n");
        return NULL;
    }

    ht->buckets = rte_zmalloc("buckets_cache_v4",
                              sizeof(struct cache_entry_v4 *) * num_buckets,
                              RTE_CACHE_LINE_SIZE);
    if (!ht->buckets) {
        fprintf(stderr, "Failed to allocate buckets\n");
        rte_free(ht);
        return NULL;
    }

    ht->bucket_locks = rte_zmalloc("bucket_locks_cache_v4",
                                   sizeof(pthread_spinlock_t) * num_buckets,
                                   RTE_CACHE_LINE_SIZE);
    if (!ht->bucket_locks) {
        fprintf(stderr, "Failed to allocate bucket_locks\n");
        rte_free(ht->buckets);
        rte_free(ht);
        return NULL;
    }

    ht->num_buckets = num_buckets;
    ht->count = 0;
    for (uint32_t i = 0; i < num_buckets; i++) {
        if (pthread_spin_init(&ht->bucket_locks[i], PTHREAD_PROCESS_PRIVATE) !=
            0) {
            fprintf(stderr, "Failed to init spinlock for bucket %u\n", i);
            // 清理已初始化的锁
            for (uint32_t j = 0; j < i; j++) {
                pthread_spin_destroy(&ht->bucket_locks[j]);
            }
            rte_free(ht->bucket_locks);
            rte_free(ht->buckets);
            rte_free(ht);
            return NULL;
        }
    }
    printf("Created connection table with %u buckets\n", num_buckets);
    return ht;
}

static void age_expired_packets(struct cache_entry_v4 *entry) {
    if (entry->start_psn == PSN_INVALID || entry->end_psn == PSN_INVALID) {
        printf("[AGE-PERIODIC] 老化处理：无有效PSN范围，无需清理数据包\n");
        return;
    }

    uint64_t aging_threshold =
        SESSION_AGING_INTERVAL * rte_get_timer_hz() / 1000;
    uint32_t idx = entry->start_psn % MAX_PSN_ARRAY;
    if (entry->mbuf_array[idx]->recv_stamp < aging_threshold) {
        return;
    }

    uint32_t aged = aging_check_binary(entry, entry->start_psn, entry->end_psn,
                                       aging_threshold);
    if (aged > 0) {
        printf("Connection aging: removed %u old packets\n", aged);
    }
}

// 从IPv4缓存表插入数据报文
int cache_tbl_v4_insert_data(struct cache_hash_v4 *ht,
                             const struct cache_key_v4 *key,
                             struct rte_mbuf *mbuf) {
    uint32_t hash = cache_hash_v4_func(key);
    uint32_t bucket_idx = hash % ht->num_buckets;

    // 获取BTH头
    const uint8_t *pkt_data = rte_pktmbuf_mtod(mbuf, const uint8_t *);
    const struct rocev2_packet *roce_pkt =
        (const struct rocev2_packet *)pkt_data;
    uint32_t psn = get_psn_from_bth(&roce_pkt->bth);

    pthread_spin_lock(&ht->bucket_locks[bucket_idx]);

    // 查找是否已存在该连接
    struct cache_entry_v4 *entry = ht->buckets[bucket_idx];
    struct cache_entry_v4 *prev = NULL;

    while (entry) {
        if (memcmp(&entry->key, key, sizeof(struct cache_key_v4)) == 0) {
            pthread_spin_lock(&entry->lock);

            // 连接级老化检查
            // TODO: 调整成基于start_psn报文时间戳是否大于100ms时启动二分老化
            if (entry->packet_count > 100) { // 阈值可配置
                uint64_t aging_threshold =
                    SESSION_AGING_INTERVAL * rte_get_timer_hz() / 1000;
                uint32_t aged = aging_check_binary(
                    entry, entry->start_psn, entry->end_psn, aging_threshold);
                if (aged > 0) {
                    printf("Connection aging: removed %u old packets\n", aged);
                }
            }

            // 插入报文
            int ret = insert_packet_to_cache_v4(entry, mbuf, psn);
            entry->timestamp = rte_get_timer_cycles();

            // TODO: 收到NAK后再收到数据报文时刷新延时ACK定时器时间
            // 收到最后一个数据报文后的5ms时，源网关给目的发送SR重传请求
            // 连接级数据缓存表中还需要保存网关角色，此时需要使用
            update_nak_delay_timer(entry);

            pthread_spin_unlock(&entry->lock);
            pthread_spin_unlock(&ht->bucket_locks[bucket_idx]);
            return ret;
        }
        prev = entry;
        entry = entry->next;
    }

    // 创建新的连接条目
    entry = create_cache_entry_v4(key);
    if (!entry) {
        pthread_spin_unlock(&ht->bucket_locks[bucket_idx]);
        rte_pktmbuf_free(mbuf);
        return ERR;
    }

    // 插入报文到新连接
    pthread_spin_lock(&entry->lock);
    int ret = insert_packet_to_cache_v4(entry, mbuf, psn);
    pthread_spin_unlock(&entry->lock);

    if (ret == ERR) {
        pthread_spin_unlock(&ht->bucket_locks[bucket_idx]);
        pthread_spin_destroy(&entry->lock);
        rte_free(entry);
        rte_pktmbuf_free(mbuf);
        return ERR;
    }

    // 插入到哈希表桶
    if (prev) {
        prev->next = entry;
    } else {
        ht->buckets[bucket_idx] = entry;
    }

    ht->count++;
    pthread_spin_unlock(&ht->bucket_locks[bucket_idx]);
    return OK;
}

// 销毁连接表
void destroy_cache_table(void) {
    if (!g_data.cache_tbl_v4) {
        return;
    }

    printf("Destroying connection table (count: %u)...\n",
           g_data.cache_tbl_v4->count);

    // 遍历所有桶，释放所有连接条目
    for (uint32_t i = 0; i < g_data.cache_tbl_v4->num_buckets; i++) {
        if (g_data.cache_tbl_v4->bucket_locks) {
            pthread_spin_lock(&g_data.cache_tbl_v4->bucket_locks[i]);
        }

        struct cache_entry_v4 *entry = g_data.cache_tbl_v4->buckets[i];
        while (entry) {
            struct cache_entry_v4 *next = entry->next;

            pthread_spin_lock(&entry->lock);

            // 清理定时器（如果存在）
            if (entry->rnr_timer_fd >= 0) {
                // 注意：这里不能直接关闭，因为定时器可能由定时器系统管理
                // 标记为无效，由定时器系统清理
                entry->rnr_timer_fd = -1;
            }

            // 释放所有缓存的报文
            for (int j = 0; j < MAX_PSN_ARRAY; j++) {
                if (entry->mbuf_array[j]) {
                    destroy_pkt_cache(entry->mbuf_array[j]);
                    entry->mbuf_array[j] = NULL;
                }
            }

            pthread_spin_unlock(&entry->lock);
            pthread_spin_destroy(&entry->lock);
            rte_free(entry);
            entry = next;
        }

        g_data.cache_tbl_v4->buckets[i] = NULL;

        if (g_data.cache_tbl_v4->bucket_locks) {
            pthread_spin_unlock(&g_data.cache_tbl_v4->bucket_locks[i]);
            pthread_spin_destroy(&g_data.cache_tbl_v4->bucket_locks[i]);
        }
    }

    // 释放桶数组和锁数组
    if (g_data.cache_tbl_v4->buckets) {
        rte_free(g_data.cache_tbl_v4->buckets);
        g_data.cache_tbl_v4->buckets = NULL;
    }

    if (g_data.cache_tbl_v4->bucket_locks) {
        rte_free(g_data.cache_tbl_v4->bucket_locks);
        g_data.cache_tbl_v4->bucket_locks = NULL;
    }

    rte_free(g_data.cache_tbl_v4);
    g_data.cache_tbl_v4 = NULL;

    printf("Connection table destroyed\n");
}

// 根据key查找连接条目
struct cache_entry_v4 *find_cache_entry_v4(struct cache_hash_v4 *ht,
                                           const struct cache_key_v4 *key) {
    uint32_t hash = cache_hash_v4_func(key);
    uint32_t bucket_idx = hash % ht->num_buckets;

    pthread_spin_lock(&ht->bucket_locks[bucket_idx]);

    struct cache_entry_v4 *entry = ht->buckets[bucket_idx];
    while (entry) {
        if (memcmp(&entry->key, key, sizeof(struct cache_key_v4)) == 0) {
            pthread_spin_unlock(&ht->bucket_locks[bucket_idx]);
            return entry;
        }
        entry = entry->next;
    }

    pthread_spin_unlock(&ht->bucket_locks[bucket_idx]);
    return NULL;
}

// 删除连接条目
int delete_cache_entry_v4(struct cache_hash_v4 *ht,
                          const struct cache_key_v4 *key) {
    uint32_t hash = cache_hash_v4_func(key);
    uint32_t bucket_idx = hash % ht->num_buckets;

    pthread_spin_lock(&ht->bucket_locks[bucket_idx]);

    struct cache_entry_v4 **ppentry = &ht->buckets[bucket_idx];
    while (*ppentry) {
        struct cache_entry_v4 *entry = *ppentry;
        if (memcmp(&entry->key, key, sizeof(struct cache_key_v4)) == 0) {
            *ppentry = entry->next;

            pthread_spin_lock(&entry->lock);

            // 清理定时器
            if (entry->rnr_timer_fd >= 0) {
                // 注意：这里不能直接关闭，因为定时器可能由定时器系统管理
                // 标记为无效，由定时器系统清理
                entry->rnr_timer_fd = -1;
            }

            // 释放所有缓存的报文
            for (int j = 0; j < MAX_PSN_ARRAY; j++) {
                if (entry->mbuf_array[j]) {
                    destroy_pkt_cache(entry->mbuf_array[j]);
                    entry->mbuf_array[j] = NULL;
                }
            }

            pthread_spin_unlock(&entry->lock);
            pthread_spin_destroy(&entry->lock);
            rte_free(entry);
            ht->count--;

            pthread_spin_unlock(&ht->bucket_locks[bucket_idx]);
            return OK;
        }
        ppentry = &entry->next;
    }

    pthread_spin_unlock(&ht->bucket_locks[bucket_idx]);
    return ERR;
}

// 获取连接表统计信息
void get_cache_table_stats(struct cache_hash_v4 *ht, uint32_t *total_entries,
                           uint32_t *total_packets,
                           uint32_t *max_bucket_depth) {
    *total_entries = 0;
    *total_packets = 0;
    *max_bucket_depth = 0;

    if (!ht)
        return;

    for (uint32_t i = 0; i < ht->num_buckets; i++) {
        pthread_spin_lock(&ht->bucket_locks[i]);

        uint32_t bucket_depth = 0;
        struct cache_entry_v4 *entry = ht->buckets[i];

        while (entry) {
            (*total_entries)++;
            pthread_spin_lock(&entry->lock);
            *total_packets += entry->packet_count;
            pthread_spin_unlock(&entry->lock);

            entry = entry->next;
            bucket_depth++;
        }

        if (bucket_depth > *max_bucket_depth) {
            *max_bucket_depth = bucket_depth;
        }

        pthread_spin_unlock(&ht->bucket_locks[i]);
    }
}

// 清理过期的连接（供老化线程调用）
uint32_t cleanup_expired_connections(struct cache_hash_v4 *ht,
                                     uint64_t aging_threshold) {
    uint32_t removed_count = 0;
    uint64_t current_time = rte_get_timer_cycles();

    if (!ht)
        return 0;

    for (uint32_t i = 0; i < ht->num_buckets; i++) {
        pthread_spin_lock(&ht->bucket_locks[i]);

        struct cache_entry_v4 **ppentry = &ht->buckets[i];
        while (*ppentry) {
            struct cache_entry_v4 *entry = *ppentry;
            pthread_spin_lock(&entry->lock);

            // 检查是否超时
            if (current_time - entry->timestamp > aging_threshold) {
                // 移除连接
                *ppentry = entry->next;

                // 清理定时器
                if (entry->rnr_timer_fd >= 0) {
                    // 注意：这里不能直接关闭，因为定时器可能由定时器系统管理
                    // 标记为无效，由定时器系统清理
                    entry->rnr_timer_fd = -1;
                }

                // 释放所有缓存的报文
                for (int j = 0; j < MAX_PSN_ARRAY; j++) {
                    if (entry->mbuf_array[j]) {
                        destroy_pkt_cache(entry->mbuf_array[j]);
                        entry->mbuf_array[j] = NULL;
                    }
                }

                pthread_spin_unlock(&entry->lock);
                pthread_spin_destroy(&entry->lock);
                rte_free(entry);
                ht->count--;
                removed_count++;

                // 继续检查下一个条目，不需要更新ppentry
            } else {
                pthread_spin_unlock(&entry->lock);
                ppentry = &entry->next;
            }
        }

        pthread_spin_unlock(&ht->bucket_locks[i]);
    }

    return removed_count;
}
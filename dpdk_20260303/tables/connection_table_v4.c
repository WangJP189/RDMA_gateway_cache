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
    entry->timestamp = rte_get_timer_cycles();
    entry->packet_count = 0;
    entry->receiver_not_ready = false;
    entry->rnr_timer = 0;
    entry->rnr_timer_fd = -1;
    entry->rnr_retry_psn = PSN_INVALID;
    entry->rnr_retry_count = 0;
    entry->nak_timer_fd = -1;
    entry->nak_psn = PSN_INVALID;
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
    } else {
        if (is_psn_less(psn, entry->start_psn)) {
            entry->start_psn = psn;
        } else if (is_psn_greater(psn, entry->end_psn)) {
            entry->end_psn = psn;
        }
        // psn在范围内则不更新
    }
    return OK;
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

// 判断单个PSN是否过期
int is_psn_expired(struct cache_entry_v4 *entry, uint32_t psn,
                   uint64_t cur_stamp, uint64 age_threshold) {
    uint32_t idx = psn % MAX_PSN_ARRAY;

    // 空指针/PSN不匹配 → 视为已过期（无有效数据）
    if (!entry->mbuf_array[idx])
        return 0;

    // PSN不匹配，发生覆盖情况
    if (entry->mbuf_array[idx]->psn != psn)
        return 1;

    // 数据包有效并且未过期返回0，过期返回1
    uint64_t survival_time = cur_stamp - entry->mbuf_array[idx]->recv_stamp;
    return (survival_time > age_threshold) ? 1 : 0;
}

// 批量清理指定PSN区间的数据包（单段遍历优化版，兼容回绕）
int batch_clean_psn_range(struct cache_entry_v4 *entry, uint32_t start_psn,
                          uint32_t end_psn) {
    uint32_t aged_count = 0;
    uint32_t distance;

    // 步骤1：计算需要遍历的PSN总数量（兼容回绕场景）
    if (end_psn >= start_psn) {
        // 非回绕：直接计算区间长度
        distance = end_psn - start_psn + 1;
    } else {
        // 回绕：(start→PSN_MASK的数量) + (0→end的数量)
        distance = (PSN_MASK - start_psn + 1) + (end_psn + 1);
    }

    // 步骤2：单段循环遍历所有需要清理的PSN（自动处理回绕）
    uint32_t cur_psn = start_psn;
    for (uint32_t i = 0; i < distance; i++) {
        uint32_t idx = cur_psn % MAX_PSN_ARRAY;
        if (entry->mbuf_array[idx]) {
            destroy_pkt_cache(entry->mbuf_array[idx]);
            entry->mbuf_array[idx] = NULL;
            entry->packet_count--;
            aged_count++;
        }
        cur_psn = (cur_psn + 1) & PSN_MASK;
    }
    return aged_count;
}

// 二分法老化处理指定PSN区间的数据包
void binary_age_psn(struct cache_entry_v4 *entry, uint32_t start_psn,
                    uint32_t end_psn, uint64_t cur_stamp,
                    uint64 age_threshold) {

    printf("[AGE-BINARY] 二分老化处理：PSN缓存范围=[0x%06X~0x%06X]\n",
           start_psn, end_psn);
    int clean_count = 0;

    // 分支1：全量过期
    // 核心准则：end_psn是当前区间最大PSN，只要它过期 → 整个区间所有PSN都过期
    if (is_psn_expired(entry, end_psn, cur_stamp, age_threshold)) {
        printf("[AGE-BINARY] 判定：全量PSN过期，清理整个区间[0x%06X~0x%06X]\n",
               start_psn, end_psn);
        clean_count = batch_clean_psn_range(entry, start_psn, end_psn);
        entry->start_psn = PSN_INVALID;
        entry->end_psn = PSN_INVALID;
        printf("[AGE-BINARY] 全量老化完成，共清理=%d个数据包，已重置PSN参数\n",
               clean_count);
        return;
    }

    // 分支2：计算范围，判断是否只有1个PSN且未过期
    uint32_t range_size;
    if (end_psn >= start_psn) {
        range_size = end_psn - start_psn + 1;
    } else {
        range_size = (PSN_MASK - start_psn + 1) + (end_psn + 1);
    }
    // 仅1个PSN且未过期 → 无过期数据
    if (range_size <= 1) {
        printf("[AGE-BINARY] 判定：无过期PSN数据包\n");
        return;
    }

    // 分支3：部分过期
    uint32_t current_start = start_psn;
    uint32_t current_end = end_psn;
    uint32_t last_expired_psn = PSN_INVALID;
    while (1) {
        uint32_t current_range_size;
        if (current_end >= current_start) {
            current_range_size = current_end - current_start + 1;
        } else {
            current_range_size =
                (PSN_MASK - current_start + 1) + (current_end + 1);
        }

        if (current_range_size <= 1) {
            break;
        }

        uint32_t half_size = current_range_size / 2;
        uint32_t mid_psn = (current_start + half_size - 1) & PSN_MASK;
        if (is_psn_expired(entry, mid_psn, cur_stamp, age_threshold)) {
            last_expired_psn = mid_psn;
            break;
        } else {
            current_end = mid_psn;
        }
    }

    // 部分过期：查找到最大过期PSN，批量清理
    if (last_expired_psn != PSN_INVALID) {
        printf("[AGE-BINARY] "
               "判定：部分PSN过期，最大过期PSN=0x%06X，执行批量清理\n",
               last_expired_psn);
        clean_count = batch_clean_psn_range(entry, start_psn, last_expired_psn);
        // 部分过期后，更新起始PSN（无需重置，仅推进start_psn）
        conn->start_psn = (last_expired_psn + 1) & PSN_MASK;
        printf("[AGE-BINARY] "
               "部分老化完成，共清理=%d个数据包，新start_psn=0x%06X\n",
               clean_count, conn->start_psn);
    } else {
        printf("[AGE-BINARY] 判定：无过期PSN数据包\n");
    }
}

// 连接级缓存报文节点老化
static void age_expired_packets(struct cache_entry_v4 *entry) {
    if (entry->start_psn >= PSN_INVALID || entry->end_psn >= PSN_INVALID) {
        printf("[AGE-PERIODIC] 老化处理：无有效PSN范围，无需清理数据包\n");
        return;
    }

    // 判断start_psn对应报文是否过期
    uint64_t cur_stamp = rte_get_timer_cycles();
    uint64_t age_threshold = SESSION_AGING_INTERVAL * rte_get_timer_hz() / 1000;
    uint32_t idx = entry->start_psn % MAX_PSN_ARRAY;
    if (cur_stamp - entry->mbuf_array[idx]->recv_stamp < age_threshold) {
        return;
    }

    // start_psn报文过期后启动二分老化
    uint32_t aged = binary_age_psn(entry, entry->start_psn, entry->end_psn,
                                   cur_stamp, age_threshold);
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
            age_expired_packets(entry);

            // 插入报文
            int ret = insert_packet_to_cache_v4(entry, mbuf, psn);
            entry->timestamp = rte_get_timer_cycles();

            // 收到NAK后再收到数据报文时刷新延时ACK定时器时间
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

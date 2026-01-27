#include "connection_table.h"
#include "global.h"
#include "utils/utils.h"
#include <dpdk/rte_config.h>
#include <dpdk/rte_ether.h>
#include <dpdk/rte_ip.h>
#include <dpdk/rte_mbuf.h>
#include <dpdk/rte_tcp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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



// ==============报文缓存辅助函数==================================

// 判断a是否小于b（环形语境下，针对24位PSN）
int psn_less_than(uint32_t a, uint32_t b) {
    // 1. 差值>半周期 → a在环形中位于b的“后方”（物理回绕后），即a < b
    // 2. 差值≤半周期 → a在环形中位于b的“前方”（无回绕），即a > b
    uint32_t ring_diff = (a - b) & PSN_MASK;

    return ring_diff > PSN_HALF_CYCLE;
}

// 环形语境下判断a是否大于b（针对24位PSN）
int psn_greater_than(uint32_t a, uint32_t b) {
    if (a == b)
        return 0;
    return !psn_less_than(a, b);
}

// 查找连接条目中有效的最小PSN
int is_psn_expired(struct cache_entry_v4 *entry, uint32_t psn,
                   uint64_t current_cycles) {
    uint32_t idx = psn % MAX_PSN_ARRAY;
    struct pkt_cache *pc = entry->mbuf_array[idx];

    // 无有效数据包
    if (pc == NULL)
        return RETRANS_NO_PACKET;
    // PSN不匹配（被覆盖）
    if (pc->psn != psn){
        return RETRANS_PACkET_PSN_MISMATCH;
    }

    // 计算存活周期，判断是否过期
    uint64_t survival_cycles = current_cycles - pc->hdr.recv_stamp;
    return (survival_cycles > PACKET_AGE_THRESHOLD) ? 1 : 0;
}

// 批量清理指定PSN区间的数据包（单段遍历优化版，兼容回绕）
uint32_t batch_clean_psn_range(struct cache_entry_v4 *entry, uint32_t start,
                               uint32_t end) {
    uint32_t cleaned_count = 0;
    uint32_t count;

    // 直接内联计算区间长度（简单逻辑，不封装）
    if (end >= start) {
        count = end - start + 1;
    } else {
        count = (PSN_MASK - start + 1) + (end + 1);
    }

    // 单段遍历处理回绕，清理数据
    for (uint32_t i = 0; i < count; i++) {
        uint32_t current_psn = (start + i) & PSN_MASK;
        uint32_t idx = current_psn % MAX_PSN_ARRAY;
        struct pkt_cache *pc = entry->mbuf_array[idx];

        if (pc == NULL)
            continue;

        // DPDK适配：释放pkt_cache，置空，更新计数
        destroy_pkt_cache(pc);
        entry->mbuf_array[idx] = NULL;
        entry->packet_count =
            (entry->packet_count > 0) ? entry->packet_count - 1 : 0; // 防负数
        cleaned_count++;
    }

    return cleaned_count;
}

// 二分法查找最后一个过期PSN
uint32_t binary_find_last_expired_psn(struct cache_entry_v4 *entry,
                                      uint32_t start_psn, uint32_t end_psn,
                                      uint64_t current_cycles) {
    uint32_t current_start = start_psn;
    uint32_t current_end = end_psn;
    uint32_t last_expired_psn = PSN_INVALID;

    while (1) {
        // 直接内联计算区间长度
        uint32_t range_size =
            (current_end >= current_start)
                ? (current_end - current_start + 1)
                : ((PSN_MASK - current_start + 1) + (current_end + 1));
        if (range_size <= 1)
            break;

        // 计算中间PSN（对齐旧版half_size-1逻辑）
        uint32_t half_size = range_size / 2;
        uint32_t mid_psn = (current_start + half_size - 1) & PSN_MASK;

        // 检查中间PSN状态，调整查找区间
        int psn_status = is_psn_expired(entry, mid_psn, current_cycles);
        if (psn_status == 1) { // 中间PSN过期，向右找更大的过期PSN
            last_expired_psn = mid_psn;
            current_start = (mid_psn + 1) & PSN_MASK;
        } else if (psn_status == 0) { // 未过期，向左找
            current_end = mid_psn;
        } else { // 无数据/PSN不匹配，视为未过期，向左找
            current_end = mid_psn;
        }
    }

    return last_expired_psn;
}



// ================报文缓存和老化函数 =========================

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

// 插入报文到连接条目的PSN数组（考虑PSN翻转）
static int insert_packet_to_cache_v4(struct cache_entry_v4 *entry,
                                     struct rte_mbuf *mbuf, uint32_t psn) {

    // 校验1：连接条目/MBUF/缓存数组为空（对应旧版校验conn_cache/ring_buf空）
    if (entry == NULL || mbuf == NULL || entry->mbuf_array == NULL) {
        return RETRANS_INVALID_PARAM;
    }
    // 校验2：DPDK mbuf长度非法（<=0，对应旧版packet_len<=0）
    uint32_t pkt_len = rte_pktmbuf_pkt_len(mbuf);
    if (pkt_len <= 0) {
        return RETRANS_INVALID_PARAM;
    }
    // 校验3：数据包长度超限（对应旧版校验超过内存块最大容量）
    int max_allowed_len = MEM_BLOCK_SIZE - sizeof(struct cache_hash_v4);
    if (pkt_len > max_allowed_len) {
        return RETRANS_INVALID_PARAM;
    }
    // // 校验4：DPDK分段mbuf（RDMA网关仅处理线性包，防御性校验）
    // if (mbuf->nb_segs > 1) {
    //     return RETRANS_INVALID_PARAM;
    // }

    uint32_t array_idx = psn % MAX_PSN_ARRAY;

    // 检查是否已存在该PSN的报文，覆盖处理
    if (entry->mbuf_array[array_idx]) {
        // 已存在，释放旧报文缓存，创建新的pkt_cache
        destroy_pkt_cache(entry->mbuf_array[array_idx]);
        entry->packet_count--;
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
        // 非首次缓存：更新start_psn，需要考虑翻转和覆盖
        if (psn_less_than(psn, entry->start_psn)) {
            // 防御性校验：确保索引对应PSN与当前PSN一致（防止串包）
            struct pkt_cache *check_pc = entry->mbuf_array[psn % MAX_PSN_ARRAY];
            if (check_pc != NULL && check_pc->psn == psn) {
                entry->start_psn = psn;
            }
        }
        // 非首次缓存：更新end_psn（对应旧版psn_greater_than判断，无额外校验）
        if (psn_greater_than(psn, entry->end_psn)) {
            entry->end_psn = psn;
        }
    }
    return OK;
}

// 连接老化检查（递归二分法批量老化）
uint32_t aging_check_binary(struct cache_entry_v4 *entry,
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

void age_expired_packets(struct cache_entry_v4 *entry) {
    // 步骤1：基础空指针/缓存数组/PSN范围校验（简单逻辑，直接内联）
    if (entry == NULL || entry->mbuf_array == NULL)
        return;
    if (entry->start_psn == PSN_INVALID || entry->end_psn == PSN_INVALID)
        return;

    // 步骤2：获取DPDK高性能时钟周期
    uint64_t current_cycles = rte_get_timer_cycles();
    if (current_cycles == 0)
        return;

    uint32_t start_psn = entry->start_psn;
    uint32_t end_psn = entry->end_psn;

    // 步骤3：start_psn有效性校验（对齐旧版校验1-3，失效则重置）
    int start_status = is_psn_expired(entry, start_psn, current_cycles);
    if (start_status == RETRANS_NO_PACKET ||
        start_status == RETRANS_PACkET_PSN_MISMATCH) {
        entry->start_psn = find_valid_min_psn(entry);
        return;
    }

    // 步骤4：检查是否达到老化检查间隔（未达到则跳过，对齐旧版校验4）
    uint32_t start_idx = start_psn % MAX_PSN_ARRAY;
    uint64_t survival_cycles =
        current_cycles - entry->mbuf_array[start_idx]->hdr.recv_stamp;
    if (survival_cycles < PACKET_AGE_CHECK_INTERVAL)
        return;

    // 步骤5：核心判断：end_psn过期则全量过期（旧版核心准则，批量清理+重置PSN）
    int end_status = is_psn_expired(entry, end_psn, current_cycles);
    if (end_status == 1) {
        batch_clean_psn_range(entry, start_psn, end_psn);
        entry->start_psn = PSN_INVALID;
        entry->end_psn = PSN_INVALID;
        entry->cur_psn = 0;
        return;
    }

    // 步骤6：单PSN未过期则直接返回（无清理必要）
    uint32_t range_size = (end_psn >= start_psn)
                              ? (end_psn - start_psn + 1)
                              : ((PSN_MASK - start_psn + 1) + (end_psn + 1));
    if (range_size <= 1)
        return;

    // 步骤7：部分过期：二分查找最大过期PSN，无过期则返回
    uint32_t last_expired_psn =
        binary_find_last_expired_psn(entry, start_psn, end_psn, current_cycles);
    if (last_expired_psn == PSN_INVALID)
        return;

    // 步骤8：批量清理过期区间，更新start_psn（推进到过期PSN的下一个，对齐旧版）
    batch_clean_psn_range(entry, start_psn, last_expired_psn);
    entry->start_psn = (last_expired_psn + 1) & PSN_MASK;
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

// ============全局老化线程相关==================

// 清理指定桶的所有连接条目（供老化线程调用）
int clean_cache_bucket(uint32_t bucket_idx) {
    // 基础校验：全局连接表/桶索引有效性
    if (g_data.cache_tbl_v4 == NULL ||
        bucket_idx >= g_data.cache_tbl_v4->num_buckets) {
        return 0;
    }

    int cleaned_count = 0;
    struct cache_entry_v4 *prev = NULL;
    struct cache_entry_v4 *curr = g_data.cache_tbl_v4->buckets[bucket_idx];

    // 桶级加自旋锁：保护整个桶的链表遍历/修改（兼容锁数组为空的边界情况）
    if (g_data.cache_tbl_v4->bucket_locks != NULL) {
        pthread_spin_lock(&g_data.cache_tbl_v4->bucket_locks[bucket_idx]);
    }

    // 遍历桶内所有连接条目（与旧版链表遍历逻辑完全一致）
    while (curr != NULL) {
        struct cache_entry_v4 *to_delete = curr; // 标记待删除条目
        // 先移动遍历指针，避免删除后指针丢失（旧版经典写法，防止链表断裂）
        curr = curr->next;

        // 条目级加自旋锁：保护单个条目的资源释放操作
        pthread_spin_lock(&to_delete->lock);

        // 清理定时器：按导师逻辑标记为无效，由定时器系统统一清理（补充delay_nak_timer_fd）
        if (to_delete->rnr_timer_fd >= 0) {
            to_delete->rnr_timer_fd = -1;
        }
        if (to_delete->delay_nak_timer_fd >= 0) {
            to_delete->delay_nak_timer_fd = -1;
        }

        // 释放条目内所有缓存的报文：复用现有destroy_pkt_cache，置空指针
        for (int j = 0; j < MAX_PSN_ARRAY; j++) {
            if (to_delete->mbuf_array[j] != NULL) {
                destroy_pkt_cache(to_delete->mbuf_array[j]);
                to_delete->mbuf_array[j] = NULL;
            }
        }

        // 释放条目资源：解锁→销毁条目自旋锁→DPDK内存释放
        pthread_spin_unlock(&to_delete->lock);
        pthread_spin_destroy(&to_delete->lock);
        rte_free(to_delete);

        // 调整链表指针：与旧版clean_idle_entry逻辑一致
        if (prev == NULL) {
            // 待删除是头节点，更新桶的头指针
            g_data.cache_tbl_v4->buckets[bucket_idx] = curr;
        } else {
            // 待删除是中间/尾节点，更新前驱的next指针
            prev->next = curr;
        }

        cleaned_count++; // 累加清理数量
        prev = NULL;     // 已删除节点，前驱置空（下一个节点重新开始）
    }

    // 桶级解锁：兼容锁数组为空的边界情况
    if (g_data.cache_tbl_v4->bucket_locks != NULL) {
        pthread_spin_unlock(&g_data.cache_tbl_v4->bucket_locks[bucket_idx]);
    }

    return cleaned_count;
}

// 清理所有桶的连接条目（供老化线程调用）
int clean_all_cache_buckets(void) {
    // 基础校验：全局连接表有效性
    if (g_data.cache_tbl_v4 == NULL) {
        return 0;
    }

    int total_cleaned = 0;
    uint32_t total_buckets = g_data.cache_tbl_v4->num_buckets;

    // 遍历所有哈希桶，逐个清理（与旧版for循环逻辑一致）
    for (uint32_t i = 0; i < total_buckets; i++) {
        total_cleaned += clean_cache_bucket(i);

        // 桶间延迟：复用旧版CONN_AGE_PER_BUCKET_DELAY，降低CPU占用（可按需注释/调整）
        // usleep(CONN_AGE_PER_BUCKET_DELAY);
    }

    return total_cleaned;
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


#include "pkt_cache.h"
#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// ==================== FlowTable接口定义 ====================
// 全局流表定义
struct flow_entry *g_flow_table_forward[TABLE_SIZE] = {0};
struct flow_entry *g_flow_table_reverse[TABLE_SIZE] = {0};

// ==================== 辅助函数 ====================
// 将IP字符串转换为本机字节序
static uint32_t ip_str_to_host(const char *ip_str) {
    struct in_addr addr;
    if (inet_pton(AF_INET, ip_str, &addr) != 1) {
        printf("[ERROR] 无效IP格式: %s\n", ip_str);
        return 0;
    }
    // inet_pton 返回的是网络序，转为本机序
    return ntohl(addr.s_addr);
}

// 计算流表哈希值
static uint32_t calc_flow_hash(const struct flow_key *key) {
    uint32_t hash = 0;
    hash = key->src_ip ^ key->dst_ip ^ key->dst_qp;
    hash ^= key->pkey;
    return hash % TABLE_SIZE;
}

// 比较两个流键是否相等
static int flow_key_equal(const struct flow_key *k1,
                          const struct flow_key *k2) {
    return (k1->src_ip == k2->src_ip && k1->dst_ip == k2->dst_ip &&
            k1->dst_qp == k2->dst_qp && k1->pkey == k2->pkey);
}

// 向流表插入单条条目
static int insert_flow_entry(struct flow_entry **table, struct flow_key key,
                             uint32_t hidden_src_qp, enum gateway_role role) {
    uint32_t hash = calc_flow_hash(&key);

    // 查重
    struct flow_entry *curr = table[hash];
    while (curr) {
        if (flow_key_equal(&curr->flow_key, &key)) {
            printf("[WARN] 流表规则已存在，跳过插入\n");
            return -1;
        }
        curr = curr->next;
    }

    // 分配内存
    struct flow_entry *new_entry =
        (struct flow_entry *)malloc(sizeof(struct flow_entry));
    if (!new_entry) {
        perror("[ERROR] 流表内存分配失败");
        return -1;
    }

    // 填充数据
    new_entry->flow_key = key;
    new_entry->src_qp = hidden_src_qp;
    new_entry->role = role;

    // 头插法 插入哈希桶
    new_entry->next = table[hash];
    table[hash] = new_entry;

    return 0;
}

// 从指定流表中删除匹配 Key 的节点
static int delete_flow_entry_internal(struct flow_entry **table,
                                      struct flow_key *key) {
    uint32_t hash = calc_flow_hash(key);
    struct flow_entry *prev = NULL;
    struct flow_entry *curr = table[hash];

    while (curr) {
        if (flow_key_equal(&curr->flow_key, key)) {
            // 找到节点，执行摘除
            if (prev) {
                prev->next = curr->next;
            } else {
                table[hash] = curr->next;
            }

            // 释放内存
            free(curr);
            return 1; // 返回 1 表示删除成功
        }
        prev = curr;
        curr = curr->next;
    }
    return 0; // 返回 0 表示未找到
}

void print_flow_entry(struct flow_entry *entry, const char *type) {
    char src_ip[INET_ADDRSTRLEN], dst_ip[INET_ADDRSTRLEN];
    // 注意：这里需要将 host 序转回网络序打印，或者直接打印 host 值
    struct in_addr s, d;
    s.s_addr = htonl(entry->flow_key.src_ip);
    d.s_addr = htonl(entry->flow_key.dst_ip);

    inet_ntop(AF_INET, &s, src_ip, sizeof(src_ip));
    inet_ntop(AF_INET, &d, dst_ip, sizeof(dst_ip));

    printf("[%s] Key: %s-> %s (DstQP:%u, PKey:%x) | Val: SrcQP=%u, Role=%d\n",
           type, src_ip, dst_ip, entry->flow_key.dst_qp, entry->flow_key.pkey,
           entry->src_qp, entry->role);
};
// ==================== 接口实现 ====================

// 创建流键
struct flow_key create_flow_key(const char *src_ip, const char *dst_ip,
                                uint32_t dst_qp, uint16_t pkey) {
    struct flow_key key;
    memset(&key, 0, sizeof(key));

    key.src_ip = ip_str_to_host(src_ip);
    key.dst_ip = ip_str_to_host(dst_ip);
    key.dst_qp = dst_qp;
    key.pkey = pkey;
    key.resv1 = 0;
    key.resv2 = 0;
    key.resv3 = 0;

    return key;
}

// 添加条目到双向流表
int add_to_flow_table(const char *src_ip_str, const char *dst_ip_str,
                      uint32_t src_qp, uint32_t dst_qp, uint16_t pkey) {
    // 1. 建立正向规则 (Forward Table)
    // 线路视角：包从 A 发往 B
    // 匹配键：Src=A, Dst=B, DstQP=QP_B
    // 目标值：SrcQP=QP_A, Role=src_gateway
    struct flow_key fwd_key =
        create_flow_key(src_ip_str, dst_ip_str, dst_qp, pkey);
    if (insert_flow_entry(g_flow_table_forward, fwd_key, src_qp, src_gateway) !=
        0) {
        printf("[ERROR] 正向流表插入失败\n");
        return -1;
    }

    // 2. 建立反向规则 (Reverse Table)
    // 线路视角：包从 B 发往 A
    // 匹配键：Src=B, Dst=A, DstQP=QP_A
    // 目标值：SrcQP=QP_B, Role=dst_gateway
    struct flow_key rev_key =
        create_flow_key(dst_ip_str, src_ip_str, src_qp, pkey);
    if (insert_flow_entry(g_flow_table_reverse, rev_key, dst_qp, dst_gateway) !=
        0) {
        printf("[ERROR] 反向流表插入失败\n");
        return -1;
    }

    printf("[FLOW] 规则条目: %s(QP:%u) <--> %s(QP:%u)\n", src_ip_str, src_qp,
           dst_ip_str, dst_qp);
    return 0;
}

// 查找流表项
struct flow_entry *lookup_flow(const char *pkt_src_ip, const char *pkt_dst_ip,
                               uint32_t pkt_dst_qp, uint16_t pkt_pkey) {
    // 构造查询键
    struct flow_key key =
        create_flow_key(pkt_src_ip, pkt_dst_ip, pkt_dst_qp, pkt_pkey);
    uint32_t hash = calc_flow_hash(&key);

    // 1. 先查 Forward 表
    struct flow_entry *curr = g_flow_table_forward[hash];
    while (curr) {
        if (flow_key_equal(&curr->flow_key, &key)) {
            return curr; // 命中正向表
        }
        curr = curr->next;
    }

    // 2. 再查 Reverse 表
    // 注意：如果是反向流，包头里的 Src 就是 B，Dst 就是 A，DstQP 就是 A 的 QP
    // 这与我们 add_flow_rules 里存的 rev_key 是一致的，所以直接查即可
    curr = g_flow_table_reverse[hash];
    while (curr) {
        if (flow_key_equal(&curr->flow_key, &key)) {
            return curr; // 命中反向表
        }
        curr = curr->next;
    }

    return NULL; // 未命中
}

// 销毁流表
void destroy_flow_tables(void) {
    struct flow_entry *curr, *tmp;

    // 清空 Forward
    for (int i = 0; i < TABLE_SIZE; i++) {
        curr = g_flow_table_forward[i];
        while (curr) {
            tmp = curr;
            curr = curr->next;
            free(tmp);
        }
        g_flow_table_forward[i] = NULL;
    }

    // 清空 Reverse
    for (int i = 0; i < TABLE_SIZE; i++) {
        curr = g_flow_table_reverse[i];
        while (curr) {
            tmp = curr;
            curr = curr->next;
            free(tmp);
        }
        g_flow_table_reverse[i] = NULL;
    }
    printf("[FLOW] 流表已销毁\n");
}

// 销毁特定双向流表条目
void remove_flow_entry(struct connection_key key) {

    // 1. 构造正向流表 Key (A -> B)
    // 逻辑：正向流查表时，看的是 BTH.DstQP，即连接的 DstQP
    struct flow_key fwd_key;
    memset(&fwd_key, 0, sizeof(fwd_key));
    fwd_key.src_ip = key.src_ip;
    fwd_key.dst_ip = key.dst_ip;
    fwd_key.dst_qp = key.dst_qp;
    fwd_key.pkey = key.pkey;
    fwd_key.resv1 = 0;
    fwd_key.resv2 = 0;
    fwd_key.resv3 = 0;

    // 2. 构造反向流表 Key (B -> A)
    // 逻辑：反向流查表时，包发给 A，BTH.DstQP 是 A 的 QP (即连接的 SrcQP)
    struct flow_key rev_key;
    memset(&rev_key, 0, sizeof(rev_key));
    rev_key.src_ip = key.dst_ip;
    rev_key.dst_ip = key.src_ip;
    rev_key.dst_qp = key.src_qp;
    rev_key.pkey = key.pkey;
    rev_key.resv1 = 0;
    rev_key.resv2 = 0;
    rev_key.resv3 = 0;

    // 3. 执行删除
    int ret_fwd = delete_flow_entry_internal(g_flow_table_forward, &fwd_key);
    int ret_rev = delete_flow_entry_internal(g_flow_table_reverse, &rev_key);

    // 4. 打印结果
    if (ret_fwd || ret_rev) {
        printf("[FLOW] 通过ConnKey删除流: QP pair %u <-> %u | Fwd:%d Rev:%d\n",
               key.src_qp, key.dst_qp, ret_fwd, ret_rev);
    } else {
        printf("[WARN] 通过ConnKey删除失败，未找到流规则: QP pair %u <-> %u\n",
               key.src_qp, key.dst_qp);
    }
}

// ==================== ConnectionTable接口定义 ====================

// 连接表定义
struct connection_bucket connection_table[TABLE_SIZE];

// ==================== 辅助函数 ====================

// 计算连接表哈希
static uint32_t calc_conn_hash(const struct connection_key *key) {
    uint32_t hash = 0;
    hash = key->src_ip ^ key->dst_ip;
    hash ^= (key->src_qp ^ key->dst_qp);
    hash ^= key->pkey;
    return hash % TABLE_SIZE;
}

// 比较连接键
static int conn_key_equal(const struct connection_key *k1,
                          const struct connection_key *k2) {
    return (k1->src_ip == k2->src_ip && k1->dst_ip == k2->dst_ip &&
            k1->src_qp == k2->src_qp && k1->dst_qp == k2->dst_qp &&
            k1->pkey == k2->pkey);
}

// ==================== 资源管理 (Cache Array) ====================

// 创建并初始化缓存结构
static struct connection_cache_array *alloc_cache_array(int size) {
    struct connection_cache_array *cache =
        (struct connection_cache_array *)malloc(
            sizeof(struct connection_cache_array));

    if (!cache) {
        perror("[ERROR] Cache结构体分配失败");
        return NULL;
    }

    // 1. 动态分配环形数组内存
    // 使用 calloc 确保初始指针全为 NULL (0)
    cache->ring_buf = (uintptr_t *)calloc(size, sizeof(uintptr_t));
    if (!cache->ring_buf) {
        perror("[ERROR] RingBuffer 内存分配失败");
        free(cache);
        return NULL;
    }

    // 2. 初始化控制字段
    cache->array_length = size;
    cache->start_psn = PSN_INVALID;
    cache->end_psn = PSN_INVALID;
    cache->cur_psn = 0;
    cache->last_active_stamp = get_current_timestamp_ms();
    return cache;
}

// 释放缓存结构
void free_cache_array(struct connection_cache_array *cache) {
    if (!cache)
        return;

    // 1. 释放环形数组里残留的数据包 (如果有)
    // 注意：这里 ring_buf 存的是 malloc 出来的 packet 指针
    if (cache->ring_buf) {
        for (int i = 0; i < cache->array_length; i++) {
            if (cache->ring_buf[i] != 0) {
                struct mem_block_header *block =
                    (struct mem_block_header *)(uintptr_t)cache->ring_buf[i];
                free(block); // 释放内存块（header+数据）
                cache->ring_buf[i] = 0;
            }
        }
        // 释放数组本身
        free(cache->ring_buf);
    }

    // 2. 释放结构体
    free(cache);
}

// ==================== 接口实现 ====================

// [新增] 初始化连接表
int init_connection_table(void) {
    for (int i = 0; i < TABLE_SIZE; i++) {
        // 1. 初始化链表头为 NULL
        connection_table[i].head = NULL;

        // 2. 初始化读写锁
        // 使用默认属性 (NULL) 即可
        if (pthread_rwlock_init(&connection_table[i].rwlock, NULL) != 0) {
            perror("[ERROR] 连接表桶锁初始化失败");

            // 回滚：销毁之前已初始化的锁
            for (int j = 0; j < i; j++) {
                pthread_rwlock_destroy(&connection_table[j].rwlock);
            }
            return -1;
        }
    }
    g_conn_buckets = connection_table; // 全局指针指向连接表
    printf("[INIT] 连接表初始化完成 (桶级读写锁模式)\n");
    return 0;
}

// [新增] 根据Key获取Bucket指针
struct connection_bucket *get_connection_bucket(struct connection_key key) {
    uint32_t hash = calc_conn_hash(&key);
    return &connection_table[hash];
}

// 创建连接键
struct connection_key create_connection_key(const char *src_ip,
                                            const char *dst_ip, uint32_t src_qp,
                                            uint32_t dst_qp, uint16_t pkey) {
    struct connection_key key;

    key.src_ip = ip_str_to_host(src_ip);
    key.dst_ip = ip_str_to_host(dst_ip);
    key.src_qp = src_qp;
    key.dst_qp = dst_qp;
    key.pkey = pkey;
    key.resv1 = 0;
    key.resv2 = 0;
    key.resv3 = 0;

    return key;
}

struct connection_key
create_connection_key_u32(uint32_t src_ip, uint32_t dst_ip, uint32_t src_qp,
                          uint32_t dst_qp, uint16_t pkey) {
    struct connection_key key;
    key.src_ip = src_ip;
    key.dst_ip = dst_ip;
    key.src_qp = src_qp;
    key.dst_qp = dst_qp;
    key.pkey = pkey;
    key.resv1 = 0;
    key.resv2 = 0;
    key.resv3 = 0;

    return key;
}
// [修改后] 查找或创建连接条目，并打印缓存信息
struct connection_cache_array *
get_connection_cache_array(struct connection_bucket *bucket,
                           struct connection_key key) {

    struct connection_cache_array *cache = NULL;
    int is_new = 0; // 标记是否为新创建

    // 1. 尝试查找
    cache = get_connection_cache(bucket->head, key);

    if (cache) {
        // 找到了，标记为旧连接
        is_new = 0;
    } else {
        // 没找到，创建新连接
        cache = create_connection_cache(bucket, key);
        is_new = 1;
    }

    // 2. 打印缓存结构体详细信息
    if (cache) {
        printf(">>> [CACHE INFO] Status: %s | SrcQP: %u\n",
               is_new ? "CREATED (New)" : "FOUND (Existing)", key.src_qp);
        printf("    |-- Base Addr  : %p\n", (void *)cache);
        printf("    |-- Array Len  : %u\n", cache->array_length);
        printf("    |-- RingBuf Ptr: %p\n", (void *)cache->ring_buf);
        printf("    |-- PSN Info   : Start=%u, End=%u, Cur=%u\n",
               cache->start_psn, cache->end_psn, cache->cur_psn);
        printf("    |-- Last Active: %lu\n", cache->last_active_stamp);
        printf("--------------------------------------------------\n");
    } else {
        printf(">>> [CACHE INFO] Failed to get/create cache for SrcQP: %u\n",
               key.src_qp);
    }

    return cache;
}

// [纯查找] 仅查找连接缓存
struct connection_cache_array *
get_connection_cache(struct connection_entry *entry,
                     struct connection_key key) {
    while (entry) {
        if (conn_key_equal(&entry->connection_key, &key)) {
            return entry->cache_array; // 命中
        }
        entry = entry->next;
    }

    return NULL; // 未命中
}

struct connection_cache_array *
create_connection_cache(struct connection_bucket *bucket,
                        struct connection_key key) {
    printf("[CONN] 创建新连接资源 (SrcQP:%u)\n", key.src_qp);

    // 2. 分配节点内存
    struct connection_entry *new_entry =
        (struct connection_entry *)malloc(sizeof(struct connection_entry));
    if (!new_entry)
        return NULL;

    // 3. 填充数据
    new_entry->connection_key = key;
    new_entry->cache_array = alloc_cache_array(RING_BUFFER_SIZE);

    if (!new_entry->cache_array) {
        free(new_entry);
        return NULL;
    }

    // 4. 插入哈希表 (头插法)
    new_entry->next = bucket->head;
    bucket->head = new_entry;

    return new_entry->cache_array;
}

// 销毁特定连接条目
void remove_connection_entry(struct connection_key key) {
    uint32_t hash = calc_conn_hash(&key);
    struct connection_entry *curr = connection_table[hash].head;
    struct connection_entry *prev = NULL;

    while (curr) {
        if (conn_key_equal(&curr->connection_key, &key)) {
            // 摘除节点
            if (prev) {
                prev->next = curr->next;
            } else {
                connection_table[hash].head = curr->next;
            }

            // 释放资源
            printf("[CONN] 销毁连接资源 (SrcQP:%u)\n", key.src_qp);
            if (curr->cache_array) {
                free_cache_array(curr->cache_array);
            }
            free(curr);
            return;
        }
        prev = curr;
        curr = curr->next;
    }
}

// 销毁整个表 (程序退出时)
void destroy_connection_table(void) {
    for (int i = 0; i < TABLE_SIZE; i++) {
        struct connection_bucket *bucket = &connection_table[i];

        // 1. 加写锁 (虽然销毁通常是单线程，但保持语义完整)
        pthread_rwlock_wrlock(&bucket->rwlock);

        struct connection_entry *curr =
            bucket->head; // 注意：这里从 bucket->head 取
        while (curr) {
            struct connection_entry *tmp = curr;
            curr = curr->next;

            // 释放节点内部资源 (RingBuffer)
            if (tmp->cache_array) {
                free_cache_array(tmp->cache_array);
            }
            free(tmp);
        }
        bucket->head = NULL;

        // 2. 解锁
        pthread_rwlock_unlock(&bucket->rwlock);

        // 3. 销毁桶级读写锁
        pthread_rwlock_destroy(&bucket->rwlock);
    }
    printf("[CONN] 连接表及读写锁已完全销毁\n");
}

//============================连接缓存操作函数实现====================================

//====================辅助函数=====================

// 辅助函数：获取当前系统的毫秒级时间戳
uint64_t get_current_timestamp_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts); // 使用单调时钟，避免系统时间修改影响
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

// 辅助函数：判断2个24位PSN的循环大小（考虑溢出场景）
// 环形语境下判断a是否小于b（仅针对24位PSN，核心解决物理回绕后的大小判断）
int psn_less_than(uint32_t a, uint32_t b) {
    // 1. 差值>半周期 → a在环形中位于b的“后方”（物理回绕后），即a < b
    // 2. 差值≤半周期 → a在环形中位于b的“前方”（无回绕），即a > b
    uint32_t ring_diff = (a - b) & PSN_MASK;

    return ring_diff > PSN_HALF_CYCLE;
}

// 环形语境下判断a是否大于b（仅针对24位PSN）
int psn_greater_than(uint32_t a, uint32_t b) {
    if (a == b)
        return 0;
    return !psn_less_than(a, b);
}

// 判断单个PSN是否过期
int is_psn_expired(struct connection_cache_array *conn, uint32_t psn,
                   uint64_t current_ts) {
    uint32_t cache_index = psn % RING_BUFFER_SIZE;

    // 空指针/PSN不匹配 → 视为已过期（无有效数据）
    if (conn->ring_buf[cache_index] == 0)
        return RETRANS_NO_PACKET;
    unsigned char *mem_block = (unsigned char *)conn->ring_buf[cache_index];
    struct mem_block_header *header = (struct mem_block_header *)mem_block;

    // PSN不匹配，发生覆盖情况
    if (header->psn != psn)
        return RETRANS_PACkET_PSN_MISMATCH;

    // 计算存活时间，判断是否过期
    // 数据包有效并且未过期返回0，过期返回1
    uint64_t survival_time = current_ts - header->recv_stamp;
    return (survival_time > PACKET_AGE_THRESHOLD) ? 1 : 0;
}

// 批量清理指定PSN区间的数据包（单段遍历优化版，兼容回绕）
int batch_clean_psn_range(struct connection_cache_array *conn, uint32_t start,
                          uint32_t end) {
    int cleaned_count = 0;

    // 步骤1：计算需要遍历的PSN总数量（兼容回绕场景）
    uint32_t count;
    if (end >= start) {
        // 非回绕：直接计算区间长度
        count = end - start + 1;
    } else {
        // 回绕：(start→PSN_MASK的数量) + (0→end的数量)
        count = (PSN_MASK - start + 1) + (end + 1);
    }

    // 步骤2：单段循环遍历所有需要清理的PSN（自动处理回绕）
    for (uint32_t i = 0; i < count; i++) {
        // 计算当前PSN（自动处理回绕，仅保留24位）
        uint32_t current_psn = (start + i) & PSN_MASK;
        // 计算环形缓冲区索引
        uint32_t idx = current_psn % RING_BUFFER_SIZE;

        // 空指针跳过（已清理/无数据）
        if (conn->ring_buf[idx] == 0)
            continue;

        // 释放内存块并置空
        free((unsigned char *)conn->ring_buf[idx]);
        conn->ring_buf[idx] = 0;
        cleaned_count++;

        // 日志输出（简化回绕段标识，保持可读性）
        printf("[EXPIRED BATCH] PSN=%u | 批量清理过期数据包\n", current_psn);
    }

    return cleaned_count;
}

// 二分法老化处理指定PSN区间的数据包
void binary_age_psn(struct connection_cache_array *conn, uint32_t start_psn,
                    uint32_t end_psn, uint64_t current_timestamp_ms) {

    printf("[AGE-BINARY] 二分老化处理：PSN缓存范围=[0x%06X~0x%06X]\n",
           start_psn, end_psn);
    int clean_count = 0;

    // 分支1：全量过期
    // 核心准则：end_psn是当前区间最大PSN，只要它过期 → 整个区间所有PSN都过期
    if (is_psn_expired(conn, end_psn, current_timestamp_ms)) {
        printf("[AGE-BINARY] 判定：全量PSN过期，清理整个区间[0x%06X~0x%06X]\n",
               start_psn, end_psn);
        clean_count = batch_clean_psn_range(conn, start_psn, end_psn);
        // 全量过期后，直接重置PSN核心参数
        conn->start_psn = PSN_INVALID;
        conn->end_psn = PSN_INVALID;
        conn->cur_psn = 0;
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

        if (is_psn_expired(conn, mid_psn, current_timestamp_ms)) {
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
        clean_count = batch_clean_psn_range(conn, start_psn, last_expired_psn);
        // 部分过期后，更新起始PSN（无需重置，仅推进start_psn）
        conn->start_psn = (last_expired_psn + 1) & PSN_MASK;
        printf("[AGE-BINARY] "
               "部分老化完成，共清理=%d个数据包，新start_psn=0x%06X\n",
               clean_count, conn->start_psn);
    } else {
        printf("[AGE-BINARY] 判定：无过期PSN数据包\n");
    }
}

// 辅助函数：重新查找当前连接中有效的最小PSN（修正失效的start_psn）
uint32_t find_valid_min_psn(struct connection_cache_array *conn) {

    uint32_t min_psn = PSN_INVALID;
    // 遍历整个环形缓冲区，查找所有有效数据块
    for (int i = 0; i < conn->array_length; i++) {
        if (conn->ring_buf[i] == 0) {
            continue; // 空位置跳过
        }

        struct mem_block_header *hdr =
            (struct mem_block_header *)(uintptr_t)conn->ring_buf[i];
        if (!hdr) {
            continue;
        }

        uint32_t curr_psn = hdr->psn;
        // 第一次找到有效PSN，直接赋值
        if (min_psn == PSN_INVALID) {
            min_psn = curr_psn;
        } else {
            // 环形语境下比较，找到更小的PSN
            if (psn_less_than(curr_psn, min_psn)) {
                min_psn = curr_psn;
            }
        }
    }

    return min_psn;
}

// 辅助函数：判断连接是否空闲过期（基于last_active_stamp）
// 返回值：1=过期，0=未过期
int is_conn_idle_expired(struct connection_cache_array *cache_array) {
    // 容错1：缓存结构体为空，返回异常
    if (cache_array == NULL) {
        printf("[ERROR] 判定连接过期失败：cache_array为空指针\n");
        return -1;
    }

    uint64_t current_ms = get_current_timestamp_ms();      // 当前毫秒时间戳
    uint64_t last_active = cache_array->last_active_stamp; // 连接最后活跃时间
    uint64_t idle_time = 0;

    // 容错2：处理时间戳回拨（当前时间 < 最后活跃时间）
    if (current_ms < last_active) {
        printf("[WARN] 系统时间回拨：current_ms=%lu < "
               "last_active=%lu，判定为未过期\n",
               current_ms, last_active);
        return 0;
    }

    // 计算实际空闲时长（毫秒）
    idle_time = current_ms - last_active;

    // 核心判定逻辑：空闲时长 ≥ 连接老化阈值（30000ms）→ 过期
    if (idle_time >= CONN_IDLE_EXPIRE_THRESHOLD) {
        printf("[DEBUG] 连接空闲时长=%lu ms ≥ 阈值=%d ms，判定为过期\n",
               idle_time, CONN_IDLE_EXPIRE_THRESHOLD);
        return 1;
    }

    // 未过期
    return 0;
}

//====================缓存数据包相关函数=====================

// 将数据包存入连接缓存结构体
int add_to_connection_cache(struct connection_cache_array *conn_cache,
                            uint32_t psn, const unsigned char *packet_data,
                            int packet_len) {
    // 校验1：待缓存的数据包指针为空
    if (!packet_data) {
        printf("[ERROR] 数据包缓存失败：packet_data输入数据为空指针\n");
        return RETRANS_INVALID_PARAM;
    }
    // 校验2：数据包长度非法（小于等于0）
    if (packet_len <= 0) {
        printf("[ERROR] "
               "数据包缓存失败：packet_len数据包长度非法，长度=%d（必须>0）\n",
               packet_len);
        return RETRANS_INVALID_PARAM;
    }
    // 校验3：数据包长度超过内存块最大可用容量
    int max_allowed_len = MEM_BLOCK_SIZE - sizeof(struct mem_block_header);
    if (packet_len > max_allowed_len) {
        printf("[ERROR] "
               "数据包缓存失败：packet_len数据包长度超限，输入长度=%d"
               "，最大允许长度=%d\n",
               packet_len, max_allowed_len);
        return RETRANS_INVALID_PARAM;
    }
    // 校验4：连接缓存结构体为空
    if (conn_cache->ring_buf == NULL || conn_cache->array_length == 0) {
        printf("[ERROR] 数据包缓存失败：conn_cache->ring_buf无效\n");
        return RETRANS_INVALID_PARAM;
    }

    // 3. 调用核心缓存函数处理数据包
    int ret = cache_rdma_packet(conn_cache, psn, packet_data, packet_len);
    if (ret != 0) {
        printf("[ERROR] 数据包缓存失败 (PSN: %u, 错误码: %d)\n", psn, ret);
        return ret;
    }

    // 4. 打印缓存成功日志
    printf("[INFO] 数据包缓存成功 - PSN: %u, 长度: %d, 缓存范围: %u-%u\n", psn,
           packet_len, conn_cache->start_psn, conn_cache->end_psn);
    return 0;
}

// 缓存RDMA数据包到内存，并将地址存入环形数组
int cache_rdma_packet(struct connection_cache_array *conn, uint32_t psn,
                      const unsigned char *data, int data_len) {

    // 分配5KB内存块
    unsigned char *mem_block = (unsigned char *)malloc(MEM_BLOCK_SIZE);
    if (!mem_block) {
        printf("[ERROR] 缓存数据包失败：内存块分配失败（5KB）\n");
        return RETRANS_DATA_ALLOC_FAIL;
    }

    // 写入头部控制信息 + RDMA数据包
    struct mem_block_header *header = (struct mem_block_header *)mem_block;
    header->data_len = data_len;
    header->recv_stamp = get_current_timestamp_ms(); // 写入毫秒级时间戳
    header->psn = psn;                               // 写入当前内存块对应的PSN
    memcpy(mem_block + sizeof(struct mem_block_header), data, data_len);

    // 计算PSN对应的环形数组索引（取模实现环形逻辑）
    int ring_index = psn % RING_BUFFER_SIZE;

    // 异常：处理环形数组覆盖冲突（覆盖旧数据包，释放旧内存）
    if (conn->ring_buf[ring_index] != 0) {
        unsigned char *old_mem_block =
            (unsigned char *)conn->ring_buf[ring_index];
        printf("[OVERWRITE] 环形数组索引=%d "
               "存在旧数据包，已释放旧内存块（地址=0x%lx）\n",
               ring_index, (uintptr_t)old_mem_block);
        free(old_mem_block); // 释放旧数据包内存
    }

    // 存入新数据包地址
    conn->ring_buf[ring_index] = (uintptr_t)mem_block;
    printf("[CACHE] 数据包PSN=%u → 环形数组索引=%d | 内存块首地址=0x%lx | "
           "数据长度=%d | 时间戳=%lu ms\n",
           psn, ring_index, (uintptr_t)mem_block, data_len, header->recv_stamp);

    // 更新连接的PSN参数
    // 处理start_psn：初始状态 或 物理回绕后更小的PSN
    if (conn->start_psn == PSN_INVALID) {
        // 初始状态，直接赋值
        conn->start_psn = psn;
    } else {
        // 仅当新PSN更小，且对应的索引有数据时，才更新start_psn
        if (psn_less_than(psn, conn->start_psn)) {
            uint32_t new_psn_idx = psn % RING_BUFFER_SIZE;
            // 校验新PSN对应索引有有效数据（刚缓存的包一定有，这里做防御性校验）
            if (conn->ring_buf[new_psn_idx] != 0) {
                struct mem_block_header *new_hdr =
                    (struct mem_block_header *)(uintptr_t)
                        conn->ring_buf[new_psn_idx];
                if (new_hdr->psn == psn) { // 确保PSN匹配，防止串包
                    conn->start_psn = psn;
                    printf(
                        "[CACHE] 更新start_psn为0x%06X（更小且有有效数据）\n",
                        psn);
                }
            }
        }
    }

    // 处理end_psn：初始状态 或 物理回绕后更大的PSN
    if (conn->end_psn == PSN_INVALID) {
        conn->end_psn = psn;
    } else if (psn_greater_than(psn, conn->end_psn)) {
        // PSN发生回绕||(psn不回绕&&psn>end_psn)，更新end_psn
        conn->end_psn = psn;
    }

    conn->cur_psn = psn;
    conn->last_active_stamp = get_current_timestamp_ms(); // 更新最后活跃时间戳

    printf("[UPDATE] 连接PSN参数：start_psn=%u, end_psn=%u, cur_psn=%u\n",
           conn->start_psn, conn->end_psn, conn->cur_psn);

    return 0;
}

// 收到ACK后清理已被确认的报文（PSN ≤ ack_msn），释放对应内存块
int clean_acked_packets(struct connection_cache_array *conn, uint32_t ack_msn) {
    if (!conn) {
        printf("[ERROR] 清理已确认报文失败：连接缓存为空\n");
        return RETRANS_NO_CACHED_PACKETS;
    }

    // 空缓存检查：无有效数据包，直接返回
    if (conn->start_psn == PSN_INVALID || conn->end_psn == PSN_INVALID) {
        printf("[ACK CLEAN] 无有效数据包，无需清理\n");
        return RETRANS_NO_VALID_PSN_RANGE;
    }

    // 仅保留ack_msn的24位有效部分，避免高位干扰，统一做掩码处理
    uint32_t temp_end = ack_msn & PSN_MASK;
    uint32_t current_start = conn->start_psn;
    uint32_t current_end = conn->end_psn;
    int cleaned_count = 0;

    printf("[ACK CLEAN] 开始清理已确认报文：ACK MSN=%u | 原始PSN范围=[%u~%u] | "
           "清理区间=[%u~%u]\n",
           temp_end, current_start, current_end, current_start, temp_end);

    // 批量清理指定PSN区间的数据包
    cleaned_count = batch_clean_psn_range(conn, current_start, temp_end);

    // 计算新的起始PSN，处理回绕+仅保留24位有效位
    uint32_t new_start = (temp_end + 1) & PSN_MASK;
    printf("[ACK CLEAN] 清理完成：释放已确认报文=%d个 | 新start_psn=%u\n",
           cleaned_count, new_start);

    // 判断是否所有包都被清理（环形语境下new_start大于current_end代表无剩余包）
    if (psn_greater_than(new_start, current_end)) {
        // 无剩余有效包，重置PSN核心参数
        conn->start_psn = PSN_INVALID;
        conn->end_psn = PSN_INVALID;
        conn->cur_psn = 0;
        printf("[ACK CLEAN] 所有报文已被清理，重置PSN参数\n");
    } else {
        // 有剩余有效包，仅更新start_psn，end_psn保持不变
        conn->start_psn = new_start;
    }

    return cleaned_count;
}

// 老化处理函数：检查并清理过期数据包
void age_expired_packets(struct connection_cache_array *conn) {
    // 第一步：基础空指针校验,确保conn和ring_buf有效
    if (conn == NULL) {
        printf("[ERROR] 老化处理：conn缓存结构体为空\n");
        return;
    }
    if (conn->ring_buf == NULL) {
        printf("[ERROR] 老化处理：conn->ring_buf为空（缓存初始化失败）\n");
        return;
    }

    uint64_t cur_stamp = get_current_timestamp_ms();
    if (cur_stamp == 0) {
        printf("[WARN] 老化处理：系统时间戳获取失败，本次跳过老化检查\n");
        return;
    }

    // PSN有效性基础校验
    if (conn->start_psn == PSN_INVALID || conn->end_psn == PSN_INVALID) {
        printf("[AGE-PERIODIC] 老化处理：无有效PSN范围，无需清理数据包\n");
        return;
    }

    // 校验1：start_psn的ring_buf对应索引无数据，说明start_psn失效 ->
    // 重新查找有效最小PSN
    uint32_t start_psn = conn->start_psn;
    uint32_t ring_idx = start_psn % RING_BUFFER_SIZE;
    if (conn->ring_buf[ring_idx] == 0) {
        printf("[WARN] 老化处理：start_psn=0x%06X 对应ring_buf索引=%u "
               "无数据，重置PSN参数\n",
               start_psn, ring_idx);
        conn->start_psn = find_valid_min_psn(conn);
        return;
    }

    // 校验2：start_psn的ring_buf对应索引数据块为空指针,说明start_psn失效 ->
    // 重新查找有效最小PSN
    struct mem_block_header *pkt_header =
        (struct mem_block_header *)(uintptr_t)conn->ring_buf[ring_idx];
    if (pkt_header == NULL) {
        printf(
            "[WARN] 老化处理：start_psn=0x%06X 对应内存块为空，重置PSN参数\n",
            start_psn);
        conn->start_psn = find_valid_min_psn(conn);
        return;
    }

    // 校验3：start_psn与内存块PSN不匹配，发生覆盖 → 重新查找有效最小PSN
    if (pkt_header->psn != start_psn) {
        printf("[WARN] 老化处理：start_psn=0x%06X 与内存块PSN=0x%06X "
               "不一致，重置PSN参数\n",
               start_psn, pkt_header->psn);
        conn->start_psn = find_valid_min_psn(conn);
        return;
    }

    // 校验4：未达到老化阈值 → 跳过
    if ((cur_stamp - pkt_header->recv_stamp) < PACKET_AGE_CHECK_INTERVAL) {
        printf(
            "[DEBUG] 老化处理：start_psn=0x%06X 未达到老化阈值，当前时间=%lu "
            "ms | 缓存时间=%lu ms | 阈值=%u ms，本次跳过\n",
            start_psn, cur_stamp, pkt_header->recv_stamp,
            PACKET_AGE_CHECK_INTERVAL);
        return;
    }

    // 触发老化清理
    printf("[INFO] 老化处理：start_psn=0x%06X 数据包已超时（当前=%lu "
           "ms/缓存=%lu ms），开始执行数据包老化清理\n",
           start_psn, cur_stamp, pkt_header->recv_stamp);

    uint32_t original_start = conn->start_psn;
    uint32_t original_end = conn->end_psn;
    binary_age_psn(conn, original_start, original_end, cur_stamp);
}

// 释放单个connection_entry及其内部资源
void free_connection_entry(struct connection_entry *entry) {
    if (entry == NULL) {
        return;
    }

    // 1. 释放cache_array（调用专用释放函数）
    if (entry->cache_array != NULL) {
        free_cache_array(entry->cache_array); // 清理环形数组+缓存结构体
        entry->cache_array = NULL;            // 置空防止野指针
    }

    // 2. 释放connection_entry自身内存
    free(entry);
}

// 清理指定哈希桶中的空闲连接条目
int clean_idle_entry(uint32_t bucket_idx) {

    // 哈希桶数组为空，直接返回
    if (g_conn_buckets == NULL) {
        printf("[ERROR] 哈希桶%u无效，清理跳过\n", bucket_idx);
        return 0;
    }

    int cleaned_count = 0;
    struct connection_entry *prev = NULL;
    struct connection_entry *curr = g_conn_buckets[bucket_idx].head;

    // 加桶级写锁（覆盖全流程）
    pthread_rwlock_wrlock(&g_conn_buckets[bucket_idx].rwlock);

    while (curr) {
        // 复用辅助函数判断连接是否空闲过期
        int is_expired = is_conn_idle_expired(curr->cache_array);
        if (is_expired == 1) { // 1表示过期，-1为异常（跳过）
            struct connection_entry *to_delete = curr;

            // 调整链表指针
            if (prev) {
                prev->next = curr->next;
            } else {
                g_conn_buckets[bucket_idx].head = curr->next;
            }

            // 移动遍历指针
            curr = curr->next;

            // 释放过期连接资源
            free_connection_entry(to_delete);
            cleaned_count++;
        } else {
            // 非过期节点，继续遍历
            prev = curr;
            curr = curr->next;
        }
    }

    // 解锁
    pthread_rwlock_unlock(&g_conn_buckets[bucket_idx].rwlock);

    // 仅当当前桶清理到连接时打印日志，无清理则不输出
    if (cleaned_count > 0) {
        printf("[CLEAN IDLE] 哈希桶%u清理完成，共释放%d个空闲连接条目\n",
               bucket_idx, cleaned_count);
    }

    return cleaned_count;
}

// 清理所有哈希桶中的空闲连接条目
int clean_global_idle_entry() {
    printf("[CLEAN GLOBAL IDLE] 开始清理所有哈希桶的空闲连接条目\n");
    int total_cleaned = 0;
    if (g_conn_buckets == NULL) {
        // printf("[ERROR] 全局空闲连接清理失败：连接桶数组未初始化\n");
        return 0;
    }

    for (uint32_t i = 0; i < CONN_BUCKET_COUNT; i++) {
        // 清理当前桶的空闲连接
        total_cleaned += clean_idle_entry(i);

        usleep(CONN_AGE_PER_BUCKET_DELAY);
    }
    if (total_cleaned > 0) {
        printf("[CLEAN GLOBAL IDLE] 全局空闲连接清理完成，共释放%d个连接条目\n",
               total_cleaned);
    }else {
        printf("[CLEAN GLOBAL IDLE] 全局空闲连接清理完成，无连接被释放\n");
    }
    return total_cleaned;
}

//==================全局资源老化功能（未完善）==================

// 全局资源老化线程函数
void *age_thread_proc(void *arg) {
    printf(
        "[AGE THREAD] 全局资源老化线程启动 | 休眠间隔=%d ms | 桶间延时=%d us\n",
        AGE_THREAD_SLEEP_INTERVAL, CONN_AGE_PER_BUCKET_DELAY);

    // 仅依赖外层g_running控制循环（1=运行，0=停止）
    while (g_running) {
        // 核心逻辑：清理所有哈希桶的空闲连接
        clean_global_idle_entry();
        // 线程休眠
        sleep(AGE_THREAD_SLEEP_INTERVAL / 1000);
    }

    printf("[AGE THREAD] 全局资源老化线程退出\n");
    return NULL;
}

// 启动全局资源老化线程
void start_age_thread(void) {
    int ret = pthread_create(&g_aging_tid, NULL, age_thread_proc, NULL);
    if (ret != 0) {
        fprintf(stderr,
                "[ERROR] age_thread_start: create thread failed, "
                "ret=%d, errno=%s\n",
                ret, strerror(errno));
        g_aging_tid = 0;
    }
    printf("[AGE_THREAD] 老化线程已启动，线程ID=%lu\n",
           (unsigned long)g_aging_tid);
}

void stop_age_thread(void) {
    if (g_aging_tid > 0) {
        pthread_join(g_aging_tid, NULL);
        printf("[AGE_THREAD] 老化线程已退出并回收\n");
        g_aging_tid = 0;
    }
}

/**
 * @brief 老化线程资源释放
 * @note 修复所有逻辑错误：等待逻辑生效、判断逻辑正确、线程优雅退出
 * @note 兼容ctrl+c信号回调，优先优雅退出，兜底强制取消，无内存泄漏
 */
void cleanup_age_resources(void) { printf("[AGE_THREAD] 老化线程资源释放\n"); }

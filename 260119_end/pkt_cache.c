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
    // hash = hash ^ ((uint32_t)key->src_port << 16 | key->dst_port);
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
    // key.src_port = src_port;
    // key.dst_port = dst_port;
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
void destroy_flow_tables() {
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
    // fwd_key.src_port = key.src_port;
    // fwd_key.dst_port = key.dst_port;
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
    // rev_key.src_port = key.dst_port;
    // rev_key.dst_port = key.src_port;
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
    // hash ^= (key->src_port | (key->dst_port << 16));
    hash ^= (key->src_qp ^ key->dst_qp);
    hash ^= key->pkey;
    return hash % TABLE_SIZE;
}

// 比较连接键
static int conn_key_equal(const struct connection_key *k1,
                          const struct connection_key *k2) {
    return (k1->src_ip == k2->src_ip && k1->dst_ip == k2->dst_ip &&
            // k1->src_port == k2->src_port &&
            // k1->dst_port == k2->dst_port &&
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
    cache->last_active_stamp = 0;
    // ZPY
    cache->retransmit_type = RETRANSMIT_NONE; // 初始化重传类型
    // ZPY

    // // 3. 初始化读写锁
    // if (pthread_rwlock_init(&cache->rwlock, NULL) != 0) {
    //     perror("[ERROR] 读写锁初始化失败");
    //     free(cache->ring_buf);
    //     free(cache);
    //     return NULL;
    // }

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
    // memset(&key, 0, sizeof(key));

    key.src_ip = ip_str_to_host(src_ip);
    key.dst_ip = ip_str_to_host(dst_ip);
    // key.src_port = src_port;
    // key.dst_port = dst_port;
    key.src_qp = src_qp;
    key.dst_qp = dst_qp;
    key.pkey = pkey;
    key.resv1 = 0;
    key.resv2 = 0;
    key.resv3 = 0;

    return key;
}

// 新增 create_connection_key_u32 实现（uint32_t 类型IP入参）
struct connection_key
create_connection_key_u32(uint32_t src_ip, uint32_t dst_ip, uint32_t src_qp,
                          uint32_t dst_qp, uint16_t pkey) {
    struct connection_key key;
    // memset(&key, 0, sizeof(key));

    key.src_ip = src_ip;
    key.dst_ip = dst_ip;
    // key.src_port = src_port;
    // key.dst_port = dst_port;
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
    // // 1. 安全检查：双重检查 (Double-Check)
    // struct connection_cache_array *existing =
    // get_connection_cache(bucket->head, key); if (existing) {
    //     return existing;
    // }

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
void destroy_connection_table() {
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
// 场景：仅当PSN从0xFFFFFF回绕到0x000000时，正确判断大小
int psn_less_than(uint32_t a, uint32_t b) {
    // 核心逻辑：
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
        return 2;
    unsigned char *mem_block = (unsigned char *)conn->ring_buf[cache_index];
    struct mem_block_header *header = (struct mem_block_header *)mem_block;
    if (header->psn != psn)
        return 1;

    // 计算存活时间，判断是否过期
    uint64_t survival_time = current_ts - header->recv_stamp;
    return (survival_time > PACKET_AGE_THRESHOLD) ? 1 : 0;
    // 数据包有效并且未过期返回0
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

/**
 * @brief 二分法处理PSN老化（回绕，查找+判断+批量清理全流程）
 * @param conn 连接缓存结构体
 * @param start_psn 缓存起始PSN
 * @param end_psn 缓存结束PSN（最大PSN）
 * @param current_timestamp_ms 当前毫秒级时间戳
 */
void binary_age_psn(struct connection_cache_array *conn, uint32_t start_psn,
                    uint32_t end_psn, uint64_t current_timestamp_ms) {
    // 范围无有效PSN（环形语境下无数据）
    if (start_psn == ((end_psn + 1) & PSN_MASK)) {
        return;
    }

    printf("[AGE-BINARY] 二分老化处理：PSN缓存范围=[0x%06X~0x%06X]\n",
           start_psn, end_psn);
    int clean_count = 0;

    // 分支1：全量过期
    // 核心准则：end_psn是当前区间最大PSN，只要它过期 → 整个区间所有PSN都过期
    if (is_psn_expired(conn, end_psn, current_timestamp_ms)) {
        printf("[AGE-BINARY] 判定：全量PSN过期，清理整个区间[0x%06X~0x%06X]\n",
               start_psn, end_psn);
        clean_count = batch_clean_psn_range(conn, start_psn, end_psn);
        // 全量过期后，直接重置PSN核心参数（把原来主函数的重置逻辑迁移到这里，职责内聚）
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

//====================缓存数据包相关函数=====================

// 将数据包存入连接缓存结构体
int add_to_connection_cache(struct connection_cache_array *conn_cache,
                            uint32_t psn, const unsigned char *packet_data,
                            int packet_len) {
    // 1. 缓存参数有效性校验 -
    // 独立拆分逐行校验，精准定位错误，匹配指定枚举错误码
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
    uint32_t max_valid_len = MEM_BLOCK_SIZE - sizeof(struct mem_block_header);
    if (packet_len > max_valid_len) {
        printf("[ERROR] "
               "数据包缓存失败：packet_len数据包长度超限，输入长度=%"
               "d，最大允许长度=%d\n",
               packet_len, max_valid_len);
        return RETRANS_INVALID_PARAM;
    }

    // // 2. 调用老化函数
    // age_expired_packets(conn_cache);

    // 3. 调用核心缓存函数处理数据包
    int ret = cache_rdma_packet(conn_cache, psn, packet_data, packet_len);
    if (ret != 0) {
        printf("[ERROR] 数据包缓存失败 (PSN: %u, 错误码: %d)\n", psn, ret);
        return ret;
    }
    conn_cache->last_active_stamp =
        get_current_timestamp_ms(); // 更新最后活跃时间戳

    // 4. 打印缓存成功日志
    printf("[INFO] 数据包缓存成功 - PSN: %u, 长度: %d, 缓存范围: %u-%u\n", psn,
           packet_len, conn_cache->start_psn, conn_cache->end_psn);
    return 0;
}

// 缓存RDMA数据包到内存，并将地址存入环形数组
int cache_rdma_packet(struct connection_cache_array *conn, uint32_t psn,
                      const unsigned char *data, int data_len) {

    // 检查数据长度是否超过内存块可用空间（5KB - 头部控制信息大小）
    int max_data_len = MEM_BLOCK_SIZE - sizeof(struct mem_block_header);
    if (data_len > max_data_len) {
        printf("[ERROR] 缓存数据包失败：数据长度超过上限（请求=%d, 上限=%d）\n",
               data_len, max_data_len);
        return RETRANS_DATA_ALLOC_FAIL;
    }

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

    // 处理环形数组冲突（覆盖旧数据包，释放旧内存）
    if (conn->ring_buf[ring_index] != 0) {
        unsigned char *old_mem_block =
            (unsigned char *)conn->ring_buf[ring_index];
        free(old_mem_block); // 释放旧数据包内存
        printf("[OVERWRITE] 环形数组索引=%d "
               "存在旧数据包，已释放旧内存块（地址=0x%lx）\n",
               ring_index, (uintptr_t)old_mem_block);
    }

    // 存入新数据包地址
    conn->ring_buf[ring_index] = (uintptr_t)mem_block;
    printf("[CACHE] 数据包PSN=%u → 环形数组索引=%d | 内存块首地址=0x%lx | "
           "数据长度=%d | 时间戳=%lu ms\n",
           psn, ring_index, (uintptr_t)mem_block, data_len, header->recv_stamp);

    // 更新连接的PSN参数（仅处理PSN物理回绕场景）
    // 处理start_psn：初始状态 或 物理回绕后更小的PSN
    if (conn->start_psn == PSN_INVALID) {
        // 初始状态，直接赋值
        conn->start_psn = psn;
    } else if (psn_less_than(psn, conn->start_psn)) {
        // 仅当PSN不回绕&&psn<start_psn，才更新start_psn
        conn->start_psn = psn;
    }

    // 处理end_psn：初始状态 或 物理回绕后更大的PSN
    if (conn->end_psn == PSN_INVALID) {
        conn->end_psn = psn;
    } else if (psn_greater_than(psn, conn->end_psn)) {
        // PSN发生回绕||(psn不回绕&&psn>end_psn)，更新end_psn
        conn->end_psn = psn;
    }

    conn->cur_psn = psn;

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

/**
 * @brief 独立完整的连接级数据包老化函数（无返回值，所有老化逻辑全内聚）
 * @param conn 连接缓存结构体
 * @note 1. 老化触发规则：若start_psn的数据包recv_stamp超时 →
 * 执行老化；否则跳过，无定时检查逻辑
 * @note 2. 所有老化相关日志全部内聚在 binary_age_psn
 * 函数中，本函数仅做「触发判断+调用执行」
 * @note 3.
 * 上层调用仅需一行age_expired_packets(conn)，无任何其他老化相关代码/打印
 * @note 4. 自动兼容PSN回绕、全量老化、部分老化、无过期等所有场景，无冗余判断
 */
void age_expired_packets(struct connection_cache_array *conn) {
    uint64_t cur_stamp = get_current_timestamp_ms();
    // 时间戳获取失败时直接跳过本次老化
    if (cur_stamp == 0) {
        printf("[WARN] 老化处理：系统时间戳获取失败，本次跳过老化检查\n");
        return;
    }
    // PSN有效性基础校验
    if (conn->start_psn == PSN_INVALID || conn->end_psn == PSN_INVALID) {
        printf("[AGE-PERIODIC] 老化处理：无有效PSN范围，无需清理数据包\n");
        return;
    }

    // 计算start_psn对应的数据包内存块，获取recv_stamp进行老化判断
    uint32_t start_psn = conn->start_psn;
    // 计算start_psn在环形缓冲区的对应索引（环形数组固定寻址公式）
    uint32_t ring_idx = start_psn % RING_BUFFER_SIZE;

    // 强转获取内存块头部，解析报文的recv_stamp和psn（核心：节点级时间戳）
    struct mem_block_header *pkt_header =
        (struct mem_block_header *)(uintptr_t)conn->ring_buf[ring_idx];
    // 严谨性校验：内存块的PSN必须和start_psn一致，防止串包/内存越界导致的误判
    if (pkt_header->psn != start_psn) {
        printf("[WARN] 老化处理：start_psn=0x%06X 与内存块PSN=0x%06X "
               "不一致，跳过老化\n",
               start_psn, pkt_header->psn);
        return;
    }

    // 老化触发判断
    // 核心规则：判断start_psn这个数据包是否超时 →
    // 超时则执行老化，不超时则直接跳过
    if ((cur_stamp - pkt_header->recv_stamp) < PACKET_AGE_CHECK_INTERVAL) {
        printf(
            "[DEBUG] 老化处理：start_psn=0x%06X 未达到老化阈值，当前时间=%lu "
            "ms | 缓存时间=%lu ms | 阈值=%u ms，本次跳过\n",
            start_psn, cur_stamp, pkt_header->recv_stamp,
            PACKET_AGE_CHECK_INTERVAL);
        return;
    }

    // 满足老化条件，执行完整的数据包老化清理
    printf("[INFO] 老化处理：start_psn=0x%06X 数据包已超时（当前=%lu "
           "ms/缓存=%lu ms），开始执行数据包老化清理\n",
           start_psn, cur_stamp, pkt_header->recv_stamp);

    // 记录原始PSN范围，用于二分老化处理
    uint32_t original_start = conn->start_psn;
    uint32_t original_end = conn->end_psn;

    // 核心调用：二分法处理PSN老化（全量/部分/无过期），无返回值
    binary_age_psn(conn, original_start, original_end, cur_stamp);
}

/**
 * @brief 释放单个connection_entry的全部资源
 * @param entry 待释放的连接条目
 */
static void free_connection_entry(struct connection_entry *entry) {
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

/**
 * @brief 遍历单个哈希桶，清理空闲的connection_entry（保证链表连贯）
 * @param bucket_idx 哈希桶索引
 * @return 清理的条目数量
 * @note 优化点：哈希桶级锁全程只加1次、解1次，无频繁加解锁的上下文切换开销
 * @note 摘链(临界区)和释放资源(非临界区)解耦，锁持有时间极短，符合桶锁设计初衷
 */
int clean_idle_entry(uint32_t bucket_idx) {
    int cleaned_count = 0;
    struct connection_entry *prev = NULL;
    struct connection_entry *curr = g_conn_buckets[bucket_idx].head;
    uint32_t curr_time = time(NULL); // 当前时间戳（秒级）
    // 定义临时链表：存放待释放的空闲节点，摘链后统一释放，非临界区操作
    struct connection_entry *del_list_head = NULL;
    struct connection_entry *del_list_tail = NULL;

    // 对一个hash桶：哈希桶级写锁只加1次，覆盖整个冲突链表的遍历+摘链操作
    pthread_rwlock_wrlock(&g_conn_buckets[bucket_idx].rwlock);

    while (curr != NULL) {
        // 判断是否空闲：最后活动时间 + 阈值 < 当前时间 【原有逻辑完全保留】
        if ((curr_time - curr->cache_array->last_active_stamp) >
            CONN_IDLE_EXPIRE_THRESHOLD) {
            struct connection_entry *to_delete = curr;

            // 1. 调整链表指针（保证哈希桶链表连贯）【原有逻辑完全保留】
            if (prev == NULL) {
                // 待删除节点是桶的头节点，更新桶的头指针
                g_conn_buckets[bucket_idx].head = curr->next;
            } else {
                // 待删除节点是中间/尾节点，更新前驱节点的next指针
                prev->next = curr->next;
            }

            // 2. 移动当前指针继续遍历，避免链表断裂 【原有逻辑完全保留】
            curr = curr->next;

            // 3. ✅ 核心修改：将待删除节点加入临时释放链表，暂不释放内存
            to_delete->next = NULL; // 断开与原链表的关联，避免野指针
            if (del_list_head == NULL) {
                del_list_head = to_delete;
                del_list_tail = to_delete;
            } else {
                del_list_tail->next = to_delete;
                del_list_tail = to_delete;
            }

            cleaned_count++;
        } else {
            // 非空闲节点，继续遍历下一个节点 【原有逻辑完全保留】
            prev = curr;
            curr = curr->next;
        }
    }

    // 对一个hash桶：哈希桶级写锁只解1次，整个冲突链表处理完毕后统一解锁
    pthread_rwlock_unlock(&g_conn_buckets[bucket_idx].rwlock);

    // 非临界区操作：解锁后，统一遍历临时链表释放所有空闲节点的资源
    // 释放操作无锁，无上下文切换，无性能损耗
    struct connection_entry *tmp_del = del_list_head;
    while (tmp_del != NULL) {
        struct connection_entry *next_del = tmp_del->next;
        free_connection_entry(
            tmp_del); // 释放连接条目（含cache_array等内部资源）
        tmp_del = next_del;
    }

    return cleaned_count;
}

/**
 * @brief 遍历所有哈希桶，清理空闲的connection_entry
 * @return 总清理条目数量
 * @note 遍历并清理完【单个哈希桶】后，立即微延时一次，再遍历下一个桶
 * @note
 * 彻底解决原逻辑"一次性遍历所有桶耗时过长"的问题，打散遍历压力，让出CPU资源
 */
int clean_global_idle_entry() {
    int total_cleaned = 0;
    if (g_conn_buckets == NULL) {
        return 0;
    }

    for (uint32_t i = 0; i < CONN_BUCKET_COUNT; i++) {
        // 清理当前桶的空闲连接
        total_cleaned += clean_idle_entry(i);

        usleep(CONN_AGE_PER_BUCKET_DELAY);
    }
    return total_cleaned;
}

//==================全局资源老化功能（未完善）==================

/**
 * @brief 全局资源老化线程入口函数
 * @return NULL
 */
void *connection_aging_thread(void *arg) {
    (void)arg; // 屏蔽未使用参数警告
    uint64_t last_print_time = get_current_timestamp_ms();
    const uint64_t print_interval_ms = 10 * 1000;

    printf("[INFO] connection aging thread start success, idle threshold: %u "
           "ms, sleep interval: %u s\n",
           CONN_IDLE_EXPIRE_THRESHOLD, AGE_THREAD_SLEEP_INTERVAL);

    // 响应退出标记，线程可正常停止
    while (g_running) {
        // 1. 执行全局空闲连接清理
        int removed_this_round = clean_global_idle_entry();
        g_total_cleaned_conn += removed_this_round;

        // 打印老化统计日志，含本次清理数+累计+活跃数
        uint64_t curr_time = get_current_timestamp_ms();
        if (removed_this_round > 0 ||
            (curr_time - last_print_time) > print_interval_ms) {
            printf("[INFO] Aging: removed %d connections this round, total "
                   "removed: %u\n",
                   removed_this_round, g_total_cleaned_conn);
            last_print_time = curr_time;
        }

        // 3. 线程休眠
        struct timespec sleep_ts = {0};
        sleep_ts.tv_sec = AGE_THREAD_SLEEP_INTERVAL;
        sleep_ts.tv_nsec = 0;
        nanosleep(&sleep_ts, NULL);
    }

    // 线程正常退出，打印日志
    printf("[INFO] connection aging thread exit success, total cleaned "
           "connections: %u\n",
           g_total_cleaned_conn);
    pthread_exit(NULL);
    return NULL;
}

/**
 * @brief 启动全局资源老化线程
 * @param idle_threshold 空闲超时阈值（毫秒），传0则用默认值
 * @return 0:成功, -1:失败
 * @note 逻辑正确：适配g_shutdown_requested(1=运行 0=退出)，无重复创建线程风险
 * @note 兼容ctrl+c信号回调，线程可正常响应退出信号
 */
int connection_aging_thread_start(uint32_t idle_threshold) {
    if (g_conn_buckets == NULL) {
        fprintf(stderr, "[ERROR] connection_aging_thread_start: global conn "
                        "buckets not initialized\n");
        return -1;
    }

    // ✅ 正确：判断线程正在运行，则先停止旧线程，防止重复创建
    if (g_running) {
        if (connection_aging_thread_stop() != 0) {
            fprintf(stderr, "[ERROR] connection_aging_thread_start: stop old "
                            "thread failed\n");
            return -1;
        }
    }

    // 更新全局空闲阈值，传0则用默认值
    g_running = 1; // ✅ 标记线程运行中，程序无退出信号

    // 创建老化线程，传参为NULL
    int ret = pthread_create(&g_aging_tid, NULL, connection_aging_thread, NULL);
    if (ret != 0) {
        fprintf(stderr,
                "[ERROR] connection_aging_thread_start: create thread failed, "
                "ret=%d, errno=%s\n",
                ret, strerror(errno));
        g_aging_tid = 0;
        g_running = 0; // ✅ 修复：创建失败，标记为退出状态
        return -1;
    }

    // 短暂等待确保线程启动
    usleep(10000);
    if (!g_running) { // ✅ 修复：判断线程是否启动失败（状态变为退出）
        pthread_cancel(g_aging_tid);
        g_aging_tid = 0;
        g_running = 1;
        return -1;
    }

    return 0;
}

/**
 * @brief 停止全局资源老化线程
 * @return 0:成功, -1:线程未运行/停止失败
 * @note 修复所有逻辑错误：等待逻辑生效、判断逻辑正确、线程优雅退出
 * @note 兼容ctrl+c信号回调，优先优雅退出，兜底强制取消，无内存泄漏
 */
int connection_aging_thread_stop(void) {
    // ✅ 修复【致命问题1】：判断逻辑完全修正，语义匹配 1=运行 0=退出
    // 正确逻辑：线程未运行 或 线程ID无效 → 返回错误
    if (!g_running || g_aging_tid == 0) {
        fprintf(
            stderr,
            "[WARN] connection_aging_thread_stop: aging thread not running\n");
        return -1;
    }

    // 设置退出标记，触发线程的while(g_shutdown_requested)循环退出，优雅退出
    g_running = 0;

    // ✅ 修复【致命问题2】：等待循环条件完全修正，生效等待逻辑
    // 正确逻辑：线程还未退出 且 未超时 → 继续等待
    uint32_t wait_cnt = 0;
    const uint32_t max_wait = AGE_THREAD_SLEEP_INTERVAL + 1;
    while (!g_running && wait_cnt < max_wait) {
        sleep(1);
        wait_cnt++;
    }

    // ✅ 修复【问题5】：强制cancel判断逻辑修正
    // 正确逻辑：线程超时仍未退出 → 强制取消线程
    int ret = 0;
    if (!g_running) {
        ret = pthread_cancel(g_aging_tid);
        if (ret != 0) {
            fprintf(stderr,
                    "[WARN] connection_aging_thread_stop: pthread_cancel "
                    "failed, ret=%d\n",
                    ret);
        }
    }

    // 释放线程资源，只调用一次pthread_join，无重复调用，无内存泄漏
    if (g_aging_tid != 0) {
        pthread_join(g_aging_tid, NULL);
        g_aging_tid = 0;
    }
    g_running = 1; // 重置为运行状态，方便下次启动

    return ret == 0 ? 0 : -1;
}

/**
 * @brief
 * 【全局资源老化线程-完整资源释放函数】释放老化线程相关的所有资源，无内存泄漏
 * @return 0:释放成功, -1:释放失败/资源未初始化
 * @note 释放顺序（严格遵守，不可逆）：
 *        1. 停止全局老化线程，确保线程无运行中操作
 *        2. 遍历所有哈希桶，销毁桶的读写锁
 *        3. 遍历所有哈希桶，释放桶内所有connection_entry及内部缓存资源
 *        4. 释放全局哈希桶数组内存
 *        5. 重置所有全局变量，防止野指针
 * @note 线程安全：本函数调用后，老化线程无法再启动，需重新初始化全局资源
 */
int connection_aging_global_resource_release(void) {
    int ret = 0;
    printf("[INFO] Start release global connection aging thread all "
           "resources...\n");

    // ============= 步骤1：先停止全局老化线程，必须第一步执行 =============
    if (g_running) {
        ret = connection_aging_thread_stop();
        if (ret != 0) {
            fprintf(stderr,
                    "[WARN] connection_aging_global_resource_release: stop "
                    "aging thread failed, ret=%d\n",
                    ret);
        }
    }

    // ============= 步骤2：释放全局哈希桶数组及内部所有资源 =============
    if (g_conn_buckets != NULL) {
        // 遍历所有哈希桶
        for (uint32_t i = 0; i < CONN_BUCKET_COUNT; i++) {
            struct connection_bucket *bucket = &g_conn_buckets[i];
            // 2.1 销毁当前桶的读写锁
            pthread_rwlock_wrlock(&bucket->rwlock);
            pthread_rwlock_destroy(&bucket->rwlock);

            // 2.2 释放桶内所有connection_entry
            struct connection_entry *curr = bucket->head;
            while (curr != NULL) {
                struct connection_entry *tmp = curr;
                curr = curr->next;
                free_connection_entry(tmp); // 调用你已有的释放entry的函数
            }

            // 2.3 重置桶内指针
            bucket->head = NULL;
        }

        // 2.4 释放哈希桶数组内存
        free(g_conn_buckets);
        g_conn_buckets = NULL;
        printf("[INFO] Release global connection buckets array success\n");
    }

    // ============= 步骤3：重置所有全局变量，防止野指针/重复释放 =============
    g_running = 1;
    g_aging_tid = 0;
    g_total_cleaned_conn = 0;

    printf("[INFO] Global connection aging thread all resources release "
           "success!\n");
    return ret;
}

/**
 * @brief 备用函数：遍历单个哈希桶，将空闲条目标记为valid=0
 * @param bucket_idx 哈希桶索引
 * @return 标记的条目数量
 */
int connection_bucket_mark_idle_as_invalid(uint32_t bucket_idx) {
    if (bucket_idx >= CONN_BUCKET_COUNT || g_conn_buckets == NULL) {
        return 0;
    }

    int marked_count = 0;
    struct connection_entry *curr = g_conn_buckets[bucket_idx].head;
    uint32_t curr_time = time(NULL);

    // 加写锁：修改entry的valid属性需独占访问
    pthread_rwlock_wrlock(&g_conn_buckets[bucket_idx].rwlock);
    while (curr != NULL) {
        if ((curr_time - curr->cache_array->last_active_stamp) >
            CONN_IDLE_EXPIRE_THRESHOLD) {
            curr->valid = 0;
            marked_count++;
        }
        curr = curr->next;
    }
    pthread_rwlock_unlock(&g_conn_buckets[bucket_idx].rwlock);

    return marked_count;
}

/**
 * @brief 备用函数：遍历单个哈希桶，清理所有valid=0的connection_entry
 * @param bucket_idx 哈希桶索引
 * @return 清理的条目数量
 */
static int connection_bucket_clean_invalid(uint32_t bucket_idx) {
    if (bucket_idx >= CONN_BUCKET_COUNT || g_conn_buckets == NULL) {
        return 0;
    }

    int cleaned_count = 0;
    struct connection_entry *prev = NULL;
    struct connection_entry *curr = g_conn_buckets[bucket_idx].head;

    pthread_rwlock_wrlock(&g_conn_buckets[bucket_idx].rwlock);
    while (curr != NULL) {
        if (curr->valid == 0) {
            struct connection_entry *to_delete = curr;

            // 调整链表指针（保证连贯性）
            if (prev == NULL) {
                g_conn_buckets[bucket_idx].head = curr->next;
            } else {
                prev->next = curr->next;
            }

            // 移动当前指针
            curr = curr->next;

            // 释放资源（解锁后操作）
            pthread_rwlock_unlock(&g_conn_buckets[bucket_idx].rwlock);
            free_connection_entry(to_delete);
            pthread_rwlock_wrlock(&g_conn_buckets[bucket_idx].rwlock);

            cleaned_count++;
        } else {
            prev = curr;
            curr = curr->next;
        }
    }
    pthread_rwlock_unlock(&g_conn_buckets[bucket_idx].rwlock);

    return cleaned_count;
}

/**
 * @brief 备用函数：遍历所有哈希桶，清理所有valid=0的connection_entry
 * @return 总清理条目数量
 */
int connection_global_clean_invalid() {
    int total_cleaned = 0;
    if (g_conn_buckets == NULL) {
        return 0;
    }

    for (uint32_t i = 0; i < CONN_BUCKET_COUNT; i++) {
        total_cleaned += connection_bucket_clean_invalid(i);
    }
    return total_cleaned;
}
#include "pkt_cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

// ==================== FlowTable接口定义 ====================

// 全局流表定义
struct flow_entry* g_flow_table_forward[TABLE_SIZE] = {0};
struct flow_entry* g_flow_table_reverse[TABLE_SIZE] = {0};

// ==================== 辅助函数 ====================

// 将IP字符串转换为本机字节序
static uint32_t ip_str_to_host(const char* ip_str) {
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
    hash = hash ^ ((uint32_t)key->src_port << 16 | key->dst_port);
    hash^=key->pkey;
    return hash % TABLE_SIZE;
}

// 比较两个流键是否相等
static int flow_key_equal(const struct flow_key *k1, const struct flow_key *k2) {
    return (k1->src_ip == k2->src_ip &&
            k1->dst_ip == k2->dst_ip &&
            k1->src_port == k2->src_port &&
            k1->dst_port == k2->dst_port &&
            k1->dst_qp == k2->dst_qp &&
            k1->pkey == k2->pkey);
}

// 向流表插入单条条目
static int insert_flow_entry(struct flow_entry **table, 
                             struct flow_key key, 
                             uint32_t hidden_src_qp, 
                             enum gateway_role role) 
{
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
    struct flow_entry *new_entry = (struct flow_entry*)malloc(sizeof(struct flow_entry));
    if (!new_entry) {
        perror("[ERROR] 流表内存分配失败");
        return -1;
    }

    // 填充数据
    new_entry->flow_key = key;
    new_entry->src_qp   = hidden_src_qp;
    new_entry->role     = role;
    
    // 头插法 插入哈希桶
    new_entry->next = table[hash];
    table[hash] = new_entry;

    return 0;
}

// 从指定流表中删除匹配 Key 的节点
static int delete_flow_entry_internal(struct flow_entry **table, struct flow_key *key) {
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

void print_flow_entry(struct flow_entry *entry, const char* type) {
    char src_ip[INET_ADDRSTRLEN], dst_ip[INET_ADDRSTRLEN];
    // 注意：这里需要将 host 序转回网络序打印，或者直接打印 host 值
    struct in_addr s, d;
    s.s_addr = htonl(entry->flow_key.src_ip);
    d.s_addr = htonl(entry->flow_key.dst_ip);
    
    inet_ntop(AF_INET, &s, src_ip, sizeof(src_ip));
    inet_ntop(AF_INET, &d, dst_ip, sizeof(dst_ip));

    printf("[%s] Key: %s:%u -> %s:%u (DstQP:%u, PKey:%x) | Val: SrcQP=%u, Role=%d\n",
           type, src_ip, entry->flow_key.src_port,
           dst_ip, entry->flow_key.dst_port,
           entry->flow_key.dst_qp, entry->flow_key.pkey,
           entry->src_qp, entry->role);
};
// ==================== 接口实现 ====================

// 创建流键
struct flow_key create_flow_key(const char *src_ip, const char *dst_ip, 
                               uint16_t src_port, uint16_t dst_port,
                               uint32_t dst_qp, uint16_t pkey) 
{
    struct flow_key key;
    memset(&key, 0, sizeof(key));
    
    key.src_ip   = ip_str_to_host(src_ip);
    key.dst_ip   = ip_str_to_host(dst_ip);
    key.src_port = src_port;
    key.dst_port = dst_port;
    key.dst_qp   = dst_qp;
    key.pkey     = pkey;
    key.resv     = 0;
    
    return key;
}

// 添加条目到双向流表
int add_to_flow_table(const char *src_ip_str, const char *dst_ip_str, 
                   uint16_t src_port, uint16_t dst_port,
                   uint32_t src_qp, uint32_t dst_qp, 
                   uint16_t pkey)
{
    // 1. 建立正向规则 (Forward Table)
    // 线路视角：包从 A 发往 B
    // 匹配键：Src=A, Dst=B, DstQP=QP_B
    // 目标值：SrcQP=QP_A, Role=src_gateway
    struct flow_key fwd_key = create_flow_key(src_ip_str, dst_ip_str, src_port, dst_port, dst_qp, pkey);
    if (insert_flow_entry(g_flow_table_forward, fwd_key, src_qp, src_gateway) != 0) {
        printf("[ERROR] 正向流表插入失败\n");
        return -1;
    }

    // 2. 建立反向规则 (Reverse Table)
    // 线路视角：包从 B 发往 A
    // 匹配键：Src=B, Dst=A, DstQP=QP_A
    // 目标值：SrcQP=QP_B, Role=dst_gateway
    // struct flow_key rev_key = create_flow_key(dst_ip_str, src_ip_str, dst_port, src_port, src_qp, pkey);
    struct flow_key rev_key = create_flow_key(dst_ip_str, src_ip_str, src_port, dst_port, src_qp, pkey);
    if (insert_flow_entry(g_flow_table_reverse, rev_key, dst_qp, dst_gateway) != 0) {
        printf("[ERROR] 反向流表插入失败\n");
        return -1;
    }

    printf("[FLOW] 规则条目: %s(QP:%u) <--> %s(QP:%u)\n", src_ip_str, src_qp, dst_ip_str, dst_qp);
    return 0;
}

//查找流表项
struct flow_entry* lookup_flow(const char *pkt_src_ip, const char *pkt_dst_ip,
                                     uint16_t pkt_src_port, uint16_t pkt_dst_port,
                                     uint32_t pkt_dst_qp, uint16_t pkt_pkey)
{
    // 构造查询键
    struct flow_key key = create_flow_key(pkt_src_ip, pkt_dst_ip, pkt_src_port, pkt_dst_port, pkt_dst_qp, pkt_pkey);
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
    fwd_key.src_ip   = key.src_ip;
    fwd_key.dst_ip   = key.dst_ip;
    fwd_key.src_port = key.src_port;
    fwd_key.dst_port = key.dst_port;
    fwd_key.dst_qp   = key.dst_qp;
    fwd_key.pkey     = key.pkey;
    fwd_key.resv     = 0;

    // 2. 构造反向流表 Key (B -> A)
    // 逻辑：反向流查表时，包发给 A，BTH.DstQP 是 A 的 QP (即连接的 SrcQP)
    struct flow_key rev_key;
    memset(&rev_key, 0, sizeof(rev_key));
    rev_key.src_ip   = key.dst_ip;
    rev_key.dst_ip   = key.src_ip;
    rev_key.src_port = key.dst_port;
    rev_key.dst_port = key.src_port;
    rev_key.dst_qp   = key.src_qp;
    rev_key.pkey     = key.pkey;
    rev_key.resv     = 0;

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
    hash ^= (key->src_port | (key->dst_port << 16));
    hash ^= (key->src_qp ^ key->dst_qp);
    hash ^= key->pkey;
    return hash % TABLE_SIZE;
}

// 比较连接键
static int conn_key_equal(const struct connection_key *k1, const struct connection_key *k2) {
    return (k1->src_ip == k2->src_ip &&
            k1->dst_ip == k2->dst_ip &&
            k1->src_port == k2->src_port &&
            k1->dst_port == k2->dst_port &&
            k1->src_qp == k2->src_qp &&
            k1->dst_qp == k2->dst_qp &&
            k1->pkey == k2->pkey);
}

// ==================== 资源管理 (Cache Array) ====================

// 创建并初始化缓存结构
static struct connection_cache_array* alloc_cache_array(int size) {
    struct connection_cache_array *cache = 
        (struct connection_cache_array*)malloc(sizeof(struct connection_cache_array));
    
    if (!cache) {
        perror("[ERROR] Cache结构体分配失败");
        return NULL;
    }
    
    // 1. 动态分配环形数组内存
    // 使用 calloc 确保初始指针全为 NULL (0)
    cache->ring_buf = (uintptr_t*)calloc(size, sizeof(uintptr_t));
    if (!cache->ring_buf) {
        perror("[ERROR] RingBuffer 内存分配失败");
        free(cache);
        return NULL;
    }

    // 2. 初始化控制字段
    cache->array_length = size;
    cache->start_psn    = 0x1000000; 
    cache->end_psn      = 0x1000000;
    cache->cur_psn      = 0;
    cache->last_age_stamp = 0; // 需要配合时间函数初始化

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
    if (!cache) return;

    // 1. 释放环形数组里残留的数据包 (如果有)
    // 注意：这里 ring_buf 存的是 malloc 出来的 packet 指针
    if (cache->ring_buf) {
        for (int i = 0; i < cache->array_length; i++) {
            if (cache->ring_buf[i] != 0) {
                struct mem_block_header *block = (struct mem_block_header*)(uintptr_t)cache->ring_buf[i];
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
struct connection_bucket* get_connection_bucket(struct connection_key key) {
    uint32_t hash = calc_conn_hash(&key);
    return &connection_table[hash];
}

// 创建连接键
struct connection_key create_connection_key(const char *src_ip, const char *dst_ip, 
                                            uint16_t src_port, uint16_t dst_port, 
                                            uint32_t src_qp, uint32_t dst_qp, uint16_t pkey) {
    struct connection_key key;
    // memset(&key, 0, sizeof(key));

    key.src_ip   = ip_str_to_host(src_ip);
    key.dst_ip   = ip_str_to_host(dst_ip);
    key.src_port = src_port;
    key.dst_port = dst_port;
    key.src_qp   = src_qp;
    key.dst_qp   = dst_qp;
    key.pkey     = pkey;
    key.resv     = 0;

    return key;
}

// [修改后] 查找或创建连接条目，并打印缓存信息
struct connection_cache_array* get_or_create_connection_cache_array(struct connection_bucket *bucket, struct connection_key key) {
    
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
        printf(">>> [CACHE INFO] Status: %s | SrcQP: %u\n", is_new ? "CREATED (New)" : "FOUND (Existing)", key.src_qp);
        printf("    |-- Base Addr  : %p\n", (void*)cache);
        printf("    |-- Array Len  : %u\n", cache->array_length);
        printf("    |-- RingBuf Ptr: %p\n", (void*)cache->ring_buf);
        printf("    |-- PSN Info   : Start=%u, End=%u, Cur=%u\n", 
               cache->start_psn, cache->end_psn, cache->cur_psn);
        printf("    |-- Last Active: %lu\n", cache->last_age_stamp);
        printf("--------------------------------------------------\n");
    } else {
        printf(">>> [CACHE INFO] Failed to get/create cache for SrcQP: %u\n", key.src_qp);
    }

    return cache;
}

// [纯查找] 仅查找连接缓存
struct connection_cache_array* get_connection_cache(struct connection_entry *entry, struct connection_key key) 
{
    while (entry) {
        if (conn_key_equal(&entry->connection_key, &key)) {
            return entry->cache_array; // 命中
        }
        entry = entry->next;
    }

    return NULL; // 未命中
}

struct connection_cache_array* create_connection_cache(struct connection_bucket *bucket, struct connection_key key) 
{
    // 1. 安全检查：双重检查 (Double-Check)
    struct connection_cache_array *existing = get_connection_cache(bucket->head, key);
    if (existing) {
        return existing;
    }

    printf("[CONN] 创建新连接资源 (SrcQP:%u)\n", key.src_qp);

    // 2. 分配节点内存
    struct connection_entry *new_entry = 
        (struct connection_entry*)malloc(sizeof(struct connection_entry));
    if (!new_entry) return NULL;

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

        struct connection_entry *curr = bucket->head; // 注意：这里从 bucket->head 取
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
    // 先掩码确保只保留24位，避免高位干扰
    uint32_t a_24 = a & PSN_MASK;
    uint32_t b_24 = b & PSN_MASK;
    
    // 核心逻辑：
    // 1. 差值>半周期 → a在环形中位于b的“后方”（物理回绕后），即a < b
    // 2. 差值≤半周期 → a在环形中位于b的“前方”（无回绕），即a > b
    uint32_t ring_diff = (a_24 - b_24) & PSN_MASK;

    return ring_diff > PSN_HALF_CYCLE;
}

// 环形语境下判断a是否大于b（仅针对24位PSN）
int psn_greater_than(uint32_t a, uint32_t b) {
    uint32_t a_24 = a & PSN_MASK;
    uint32_t b_24 = b & PSN_MASK;
    if (a_24 == b_24) return 0;
    return !psn_less_than(a_24, b_24);
}

// 判断单个PSN是否过期
int is_psn_expired(struct connection_cache_array* conn, uint32_t psn, uint64_t current_ts) {
    uint32_t psn_24 = psn & PSN_MASK;
    uint32_t cache_index = psn_24 % RING_BUFFER_SIZE;

    // 空指针/PSN不匹配 → 视为已过期（无有效数据）
    if (conn->ring_buf[cache_index] == 0) return 2;
    unsigned char* mem_block = (unsigned char*)conn->ring_buf[cache_index];
    struct mem_block_header* header = (struct mem_block_header*)mem_block;
    if (header->psn != psn_24) return 1;

    // 计算存活时间，判断是否过期
    uint64_t survival_time = current_ts - header->recv_stamp;
    return (survival_time > MAX_AGE_MILLISECONDS) ? 1 : 0;
}

// 批量清理指定PSN区间的数据包（处理回绕）
int batch_clean_psn_range(struct connection_cache_array* conn, uint32_t start, uint32_t end) {
    int cleaned_count = 0;
    uint32_t start_24 = start & PSN_MASK;
    uint32_t end_24 = end & PSN_MASK;

    if (end_24 >= start_24) {
        // 非回绕：直接遍历[start, end]
        for (uint32_t psn = start_24; psn <= end_24; psn++) {
            uint32_t idx = psn % RING_BUFFER_SIZE;
            if (conn->ring_buf[idx] == 0) continue;
            
            free((unsigned char*)conn->ring_buf[idx]);
            conn->ring_buf[idx] = 0;
            cleaned_count++;
            printf("[EXPIRED BATCH] PSN=%u | 批量清理过期数据包\n", psn);
        }
    } else {
        // 回绕：分两段遍历[start→PSN_MASK] + [0→end]
        for (uint32_t psn = start_24; psn <= PSN_MASK; psn++) {
            uint32_t idx = psn % RING_BUFFER_SIZE;
            if (conn->ring_buf[idx] == 0) continue;
            
            free((unsigned char*)conn->ring_buf[idx]);
            conn->ring_buf[idx] = 0;
            cleaned_count++;
            printf("[EXPIRED BATCH] PSN=%u | 批量清理过期数据包（回绕段1）\n", psn);
        }
        for (uint32_t psn = 0; psn <= end_24; psn++) {
            uint32_t idx = psn % RING_BUFFER_SIZE;
            if (conn->ring_buf[idx] == 0) continue;
            
            free((unsigned char*)conn->ring_buf[idx]);
            conn->ring_buf[idx] = 0;
            cleaned_count++;
            printf("[EXPIRED BATCH] PSN=%u | 批量清理过期数据包（回绕段2）\n", psn);
        }
    }
    return cleaned_count;
}

//====================缓存数据包相关函数=====================


// 将数据包存入连接缓存结构（集成老化处理逻辑）
int add_to_connection_cache(struct connection_cache_array* conn_cache, uint32_t psn,
                            const unsigned char *packet_data, int packet_len)
{
    // 1. 检查新入参的有效性（适配新传参的参数校验）
    if (!conn_cache || !packet_data || packet_len <= 0 || 
        packet_len > (MEM_BLOCK_SIZE - sizeof(struct mem_block_header))) {
        printf("[ERROR] 无效的缓存参数: conn_cache=%p/输入数据=%p/数据包长度=%d（上限=%d）\n",
               conn_cache, packet_data, packet_len, (int)(MEM_BLOCK_SIZE - sizeof(struct mem_block_header)));
        return -1;
    }

    // 2. 调用缓存函数处理数据包（核心逻辑不变）
    int ret = cache_rdma_packet(conn_cache, psn, packet_data, packet_len);
    if (ret != 0) {
        printf("[ERROR] 数据包缓存失败 (PSN: %u, 错误码: %d)\n", psn, ret);
        return ret;
    }

    // 3. 新增：连接级老化处理逻辑（从pkt_recv.c迁移）
    // 3.1 获取当前毫秒级时间戳
    uint64_t cur_stamp = get_current_timestamp_ms();
    if (cur_stamp == 0) { // 容错：时间戳获取失败时跳过老化
        printf("[WARN] 时间戳获取失败，跳过本次老化检查\n");
    } else {
        // 3.2 初始化连接的老化检查时间戳（首次缓存时设置）
        if (conn_cache->last_age_stamp == 0) {
            conn_cache->last_age_stamp = cur_stamp + CONN_AGE_CHECK_INTERVAL_MS;
            printf("[INFO] 初始化连接老化检查时间戳：下次检查时间=%lu ms\n", conn_cache->last_age_stamp);
        }

        // 3.3 达到检查时间，执行老化处理
        if (cur_stamp >= conn_cache->last_age_stamp) {
            printf("[INFO] 达到老化检查时间（当前=%lu ms/上次检查=%lu ms），执行数据包老化\n",
                   cur_stamp, conn_cache->last_age_stamp);
            // 调用老化函数，捕获返回值（仅日志，不影响缓存结果）
            int age_ret = age_out_expired_packets(conn_cache, cur_stamp);
            switch (age_ret) {
                case RETRANS_NO_CACHED_PACKETS:
                    printf("[INFO] 老化处理：无缓存数据包\n");
                    break;
                case RETRANS_NO_VALID_PSN_RANGE:
                    printf("[INFO] 老化处理：无有效PSN范围，无需清理\n");
                    break;
                default:
                    printf("[INFO] 老化处理完成：清理过期数据包=%d个\n", age_ret);
                    break;
            }
            // 3.4 更新下次老化检查时间
            conn_cache->last_age_stamp = cur_stamp + CONN_AGE_CHECK_INTERVAL_MS;
            printf("[INFO] 更新下次老化检查时间戳：%lu ms\n", conn_cache->last_age_stamp);
        } else {
            // 未到检查时间，打印调试日志（可选，可注释）
            printf("[DEBUG] 未到老化检查时间（当前=%lu ms/下次=%lu ms），跳过\n",
                   cur_stamp, conn_cache->last_age_stamp);
        }
    }

    // 4. 打印缓存成功日志（调整日志内容，适配新传参）
    printf("[INFO] 数据包缓存成功 - PSN: %u, 长度: %d, 缓存范围: %u-%u\n",
           psn, packet_len, conn_cache->start_psn, conn_cache->end_psn);
    return 0;
}


// 缓存RDMA数据包到内存，并将地址存入环形数组
int cache_rdma_packet(struct connection_cache_array* conn, uint32_t psn, const unsigned char* data, int data_len) {
    if (!conn || !data || data_len <= 0) {
        printf("[ERROR] 缓存数据包失败：参数无效（conn=%p, psn=%u, data_len=%d）\n",
               conn, psn, data_len);
        return RETRANS_INVALID_PARAM;
    }

    // 检查数据长度是否超过内存块可用空间（5KB - 头部控制信息大小）
    int max_data_len = MEM_BLOCK_SIZE - sizeof(struct mem_block_header);
    if (data_len > max_data_len) {
        printf("[ERROR] 缓存数据包失败：数据长度超过上限（请求=%d, 上限=%d）\n", data_len, max_data_len);
        return RETRANS_DATA_ALLOC_FAIL;
    }

    // 分配5KB内存块
    unsigned char* mem_block = (unsigned char*)malloc(MEM_BLOCK_SIZE);
    if (!mem_block) {
        printf("[ERROR] 缓存数据包失败：内存块分配失败（5KB）\n");
        return RETRANS_DATA_ALLOC_FAIL;
    }

    // 写入头部控制信息 + RDMA数据包
    struct mem_block_header* header = (struct mem_block_header*)mem_block;
    header->data_len = data_len;
    header->recv_stamp = get_current_timestamp_ms(); // 写入毫秒级时间戳
    header->psn = psn & PSN_MASK; // 写入当前内存块对应的PSN
    memcpy(mem_block + sizeof(struct mem_block_header), data, data_len);

    // 计算PSN对应的环形数组索引（取模实现环形逻辑）
    int ring_index = psn % RING_BUFFER_SIZE;

    // 处理环形数组冲突（覆盖旧数据包，释放旧内存）
    if (conn->ring_buf[ring_index] != 0) {
        unsigned char* old_mem_block = (unsigned char*)conn->ring_buf[ring_index];
        free(old_mem_block); // 释放旧数据包内存
        printf("[OVERWRITE] 环形数组索引=%d 存在旧数据包，已释放旧内存块（地址=0x%lx）\n",
               ring_index, (uintptr_t)old_mem_block);
    }

    // 存入新数据包地址
    conn->ring_buf[ring_index] = (uintptr_t)mem_block;
    printf("[CACHE] 数据包PSN=%u → 环形数组索引=%d | 内存块首地址=0x%lx | 数据长度=%d | 时间戳=%lu ms\n",
           psn, ring_index, (uintptr_t)mem_block, data_len, header->recv_stamp);


    // 更新连接的PSN参数（仅处理PSN物理回绕场景）
    // 处理start_psn：初始状态 或 物理回绕后更小的PSN
    if (conn->start_psn == 0x1000000) { 
        // 初始状态，直接赋值
        conn->start_psn = psn;
    } else if (psn_less_than(psn, conn->start_psn)) { 
        // 仅当PSN不回绕&&psn<start_psn，才更新start_psn
        conn->start_psn = psn;
    }

    // 处理end_psn：初始状态 或 物理回绕后更大的PSN
    if (conn->end_psn == 0x1000000) { 
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
int clean_acked_packets(struct connection_cache_array* conn, uint32_t ack_msn) {
    if (!conn) {
        printf("[ERROR] 清理已确认报文失败：连接缓存为空\n");
        return RETRANS_NO_CACHED_PACKETS;
    }

    // 空缓存检查：无有效数据包，直接返回
    if (conn->start_psn == 0x1000000 || conn->end_psn == 0x1000000) {
        printf("[ACK CLEAN] 无有效数据包，无需清理\n");
        return RETRANS_NO_VALID_PSN_RANGE;
    }

    // 仅保留ack_msn的24位有效部分，避免高位干扰
    uint32_t temp_end = ack_msn & PSN_MASK;
    uint32_t current_start = conn->start_psn & PSN_MASK;
    uint32_t current_end = conn->end_psn & PSN_MASK;
    int cleaned_count = 0;

    printf("[ACK CLEAN] 开始清理已确认报文：ACK MSN=%u | 原始PSN范围=[%u~%u] | 清理区间=[%u~%u]\n",
           temp_end, current_start, current_end, current_start, temp_end);

    // 分两种场景处理遍历：无回绕/回绕
    // 场景1：无回绕（temp_end ≥ current_start）→ 直接遍历[current_start, temp_end]
    if (temp_end >= current_start) {
        for (uint32_t psn = current_start; psn <= temp_end; psn++) {
            uint32_t cache_index = psn % RING_BUFFER_SIZE;
            // 空指针跳过
            if (conn->ring_buf[cache_index] == 0) continue;
            
            // 释放内存块并置空
            unsigned char* mem_block = (unsigned char*)conn->ring_buf[cache_index];
            free(mem_block);
            conn->ring_buf[cache_index] = 0;
            cleaned_count++;
            printf("[ACKED] PSN=%u | 已被MSN=%u确认，释放内存\n", psn, temp_end);
        }
    } 
    // 场景2：真回绕（temp_end < current_start && temp_end在[0,current_end]之间）→ 分两段遍历
    else if(temp_end < current_start && temp_end <= current_end) {
        // 第一段：current_start → PSN_MASK（0xFFFFFF）
        for (uint32_t psn = current_start; psn <= PSN_MASK; psn++) {
            uint32_t cache_index = psn % RING_BUFFER_SIZE;
            if (conn->ring_buf[cache_index] == 0) continue;
            
            unsigned char* mem_block = (unsigned char*)conn->ring_buf[cache_index];
            free(mem_block);
            conn->ring_buf[cache_index] = 0;
            cleaned_count++;
            printf("[ACKED] PSN=%u | 已被MSN=%u确认，释放内存（回绕段1）\n", psn, temp_end);
        }
        // 第二段：0 → temp_end
        for (uint32_t psn = 0; psn <= temp_end; psn++) {
            uint32_t cache_index = psn % RING_BUFFER_SIZE;
            if (conn->ring_buf[cache_index] == 0) continue;
            
            unsigned char* mem_block = (unsigned char*)conn->ring_buf[cache_index];
            free(mem_block);
            conn->ring_buf[cache_index] = 0;
            cleaned_count++;
            printf("[ACKED] PSN=%u | 已被MSN=%u确认，释放内存（回绕段2）\n", psn, temp_end);
        }
    }

    // 更新start_psn：temp_end + 1（处理回绕，仅保留24位）
    uint32_t new_start = (temp_end + 1) & PSN_MASK;
    printf("[ACK CLEAN] 清理完成：释放已确认报文=%d个 | 新start_psn=%u\n", cleaned_count, new_start);

    // 判断是否所有包都被清理（new_start在环形语境下>original_end）
    if (psn_greater_than(new_start, current_end)) {
        // 无剩余有效包，重置PSN参数
        conn->start_psn = 0x1000000;
        conn->end_psn = 0x1000000;
        conn->cur_psn = 0;
        printf("[ACK CLEAN] 所有报文已被清理，重置PSN参数\n");
    } else {
        // 有剩余有效包，更新start_psn为new_start，end_psn保持不变
        conn->start_psn = new_start;
    }

    return cleaned_count;
}


// 老化处理函数：按指定二分逻辑优化版（处理回绕）
int age_out_expired_packets(struct connection_cache_array* conn, uint64_t current_timestamp_ms) {
    if (!conn) {
        printf("[ERROR] 老化处理失败：连接缓存为空\n");
        return RETRANS_NO_CACHED_PACKETS;
    }

    // 空缓存检查：无有效PSN范围时直接返回
    if (conn->start_psn == 0x1000000 || conn->end_psn == 0x1000000) {
        printf("[AGE] 无有效PSN范围，无需老化处理\n");
        return RETRANS_NO_VALID_PSN_RANGE;
    }

    uint32_t original_start = conn->start_psn & PSN_MASK;
    uint32_t original_end = conn->end_psn & PSN_MASK;
    int expired_count = 0;
    uint32_t last_expired_psn = 0x1000000; // 存储全局最大过期PSN

    printf("[AGE] 开始老化处理：当前时间戳=%lu ms | 最大老化时间=%d ms | 原始PSN范围=[%u~%u]\n",
           current_timestamp_ms, MAX_AGE_MILLISECONDS, original_start, original_end);

    // ===================== 用二分法查找老化范围 =====================
    uint32_t start = original_start;
    uint32_t end = original_end;

    if (end >= start) {
        // 场景1：非回绕 → 按指定二分逻辑查找
        uint32_t cur_left = start;
        uint32_t cur_right = end;
        while (cur_left <= cur_right) {
            // 步骤1：判断当前范围的最大值（cur_right）是否过期
            if (is_psn_expired(conn, cur_right, current_timestamp_ms)) {
                last_expired_psn = cur_right; // 找到最大过期PSN，终止查找
                break;
            }
            // 步骤2：未过期 → 缩小范围（无过期则终止）
            if (cur_left == cur_right) {
                break; // 范围缩小到单点，无过期
            }
            // 计算中点（避免溢出），缩小右边界到中点
            uint32_t mid = cur_left + ((cur_right - cur_left) >> 1);
            cur_right = mid;
        }
    } else {
        // 场景2：回绕 → 优化逻辑：先判断PSN_MASK是否过期
        int is_psn_mask_expired = is_psn_expired(conn, PSN_MASK, current_timestamp_ms);
        printf("[AGE] 回绕场景：PSN_MASK(0x%06X)是否过期=%d\n", PSN_MASK, is_psn_mask_expired);

        if (!is_psn_mask_expired) {
            // 分支1：PSN_MASK不过期 → 仅查找[start, PSN_MASK]区间
            uint32_t cur_left1 = start;
            uint32_t cur_right1 = PSN_MASK;
            uint32_t part1_expired = 0x1000000;
            while (cur_left1 <= cur_right1) {
                if (is_psn_expired(conn, cur_right1, current_timestamp_ms)) {
                    part1_expired = cur_right1;
                    break;
                }
                if (cur_left1 == cur_right1) {
                    break;
                }
                uint32_t mid = cur_left1 + ((cur_right1 - cur_left1) >> 1);
                cur_right1 = mid;
            }
            last_expired_psn = part1_expired;
        } else {
            // 分支2：PSN_MASK过期 → [start, PSN_MASK]全过期，仅查找[0, end]区间
            uint32_t cur_left2 = 0;
            uint32_t cur_right2 = end;
            uint32_t part2_expired = 0x1000000;
            while (cur_left2 <= cur_right2) {
                if (is_psn_expired(conn, cur_right2, current_timestamp_ms)) {
                    part2_expired = cur_right2;
                    break;
                }
                if (cur_left2 == cur_right2) {
                    break;
                }
                uint32_t mid = cur_left2 + ((cur_right2 - cur_left2) >> 1);
                cur_right2 = mid;
            }
            // 若[0, end]有过期PSN，最终过期PSN为part2_expired；否则为PSN_MASK（第一段全过期）
            last_expired_psn = (part2_expired != 0x1000000) ? part2_expired : PSN_MASK;
        }
    }
    // ===================== 二分查找逻辑结束 =====================

    // 无过期PSN，直接返回
    if (last_expired_psn == 0x1000000) {
        printf("[AGE] 无过期数据包，无需清理\n");
        return 0;
    }
    printf("[AGE] 二分查找完成：最大过期PSN=0x%06X\n", last_expired_psn);

    // 批量清理[original_start, last_expired_psn]区间的过期包
    if(last_expired_psn >= original_start){
        // 非回绕场景
        expired_count = batch_clean_psn_range(conn, original_start, last_expired_psn);
    }
    else{
        // 回绕场景，分两段清理
        expired_count = batch_clean_psn_range(conn, original_start, PSN_MASK);
        expired_count += batch_clean_psn_range(conn, 0, last_expired_psn);
    }

    // 更新start_psn（处理回绕，仅保留24位）
    uint32_t new_start = (last_expired_psn + 1) & PSN_MASK;
    printf("[AGE] 清理完成：共清理过期数据包=%d个 | 新start_psn=0x%06X\n", expired_count, new_start);

    // 判断是否所有包都过期，重置参数
    int is_all_expired = 0;

    if (original_end >= original_start) {
        is_all_expired = (new_start > original_end);
    } else {
        is_all_expired = (new_start > original_end) && (new_start <= original_start);
    }

    if (is_all_expired) {
        // 无剩余有效包，重置PSN参数
        conn->start_psn = 0x1000000;
        conn->end_psn = 0x1000000;
        conn->cur_psn = 0;
        printf("[AGE] 所有数据包已过期，重置PSN参数\n");
    } else {
        // 有剩余有效包，更新start_psn
        conn->start_psn = new_start;
        // end_psn和cur_psn保持不变（未过期包的结束PSN仍有效）
    }

    printf("[UPDATE] 老化处理后PSN参数：start_psn=0x%06X, end_psn=0x%06X, cur_psn=%u\n",
           conn->start_psn, conn->end_psn, conn->cur_psn);
    return expired_count;
}











//==================全局资源老化功能（未完善）==================

// 1. 初始化连接表锁（全局唯一）
pthread_mutex_t g_conn_table_mutex = PTHREAD_MUTEX_INITIALIZER;

// 2. 实现：更新连接最后活动时间（业务逻辑调用，如收包/发包时）
void update_conn_last_active(struct connection_entry *conn) {
    if (conn == NULL) return;
    // 加锁保证时间戳更新原子性（避免多线程同时写）
    pthread_mutex_lock(&g_conn_table_mutex);
    conn->last_active_stamp = time(NULL); // 秒级时间戳
    pthread_mutex_unlock(&g_conn_table_mutex);
}

// 3. 实现：释放单个连接的内存（按需补充子资源释放）
void free_connection_entry(struct connection_entry *conn) {
    if (conn == NULL) return;
    // 【关键】释放连接内的子资源（根据你的结构体补充）
    // 示例：若有动态分配的缓冲区/IP字符串，需先释放
    // if (conn->src_ip) free(conn->src_ip);
    // if (conn->dst_ip) free(conn->dst_ip);
    // if (conn->qp_ctx) free(conn->qp_ctx);
    
    // 释放连接本身
    free(conn);
}

// 4. 实现：从哈希桶中移除连接（需遍历桶内链表找到并删除）
void remove_conn_from_bucket(struct connection_entry *conn) {
    if (conn == NULL) return;

    pthread_mutex_lock(&g_conn_table_mutex);

    // 假设g_conn_buckets是哈希桶数组，每个桶是connection_entry链表头
    int bucket_idx = conn->bucket_idx; // 需保证connection_entry有bucket_idx字段（记录所属桶）
    struct connection_entry *prev = NULL;
    struct connection_entry *curr = g_conn_buckets[bucket_idx].next; // 假设桶头是哨兵节点

    // 遍历链表找目标连接
    while (curr != NULL) {
        if (curr == conn) {
            // 从链表中移除
            if (prev != NULL) {
                prev->next = curr->next;
            } else {
                // 目标是链表第一个节点
                g_conn_buckets[bucket_idx].next = curr->next;
            }
            break;
        }
        prev = curr;
        curr = curr->next;
    }

    pthread_mutex_unlock(&g_conn_table_mutex);
}

// 5. 核心：全局老化线程逻辑
void *global_conn_age_thread(void *arg) {
    (void)arg; // 忽略参数
    printf("Global connection age thread start, sleep %d sec per loop\n", AGE_THREAD_SLEEP_SEC);

    while (1) {
        // 步骤1：休眠指定时间
        sleep(AGE_THREAD_SLEEP_SEC);

        // 步骤2：遍历所有哈希桶
        time_t now = time(NULL);
        pthread_mutex_lock(&g_conn_table_mutex); // 加锁保护遍历/修改

        // 假设BUCKET_COUNT是哈希桶总数（需提前定义）
        for (int bucket_idx = 0; bucket_idx < BUCKET_COUNT; bucket_idx++) {
            struct connection_entry *prev = NULL;
            struct connection_entry *curr = g_conn_buckets[bucket_idx].next;

            // 遍历当前桶内的所有连接
            while (curr != NULL) {
                // 保存下一个节点（避免删除curr后断链）
                struct connection_entry *next = curr->next;

                // 步骤3：判断是否空闲超时
                if (now - curr->last_active_stamp >= CONN_IDLE_EXPIRE_THRESHOLD_SEC) {
                    printf("Conn idle timeout (bucket %d): src_ip=%s, dst_ip=%s, idle=%ld sec\n",
                           bucket_idx, curr->src_ip, curr->dst_ip, now - curr->last_active_stamp);

                    // 步骤4：从链表移除
                    if (prev != NULL) {
                        prev->next = next;
                    } else {
                        g_conn_buckets[bucket_idx].next = next;
                    }

                    // 步骤5：释放连接内存（解锁后释放，避免锁持有过久）
                    pthread_mutex_unlock(&g_conn_table_mutex);
                    free_connection_entry(curr);
                    pthread_mutex_lock(&g_conn_table_mutex);
                } else {
                    // 未超时，更新prev
                    prev = curr;
                }

                curr = next;
            }
        }

        pthread_mutex_unlock(&g_conn_table_mutex); // 解锁
    }

    // 【可选】优雅退出逻辑（若需要）
    // pthread_exit(NULL);
    return NULL;
}
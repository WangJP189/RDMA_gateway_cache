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
    cache->start_psn    = 0; 
    cache->end_psn      = 0;
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
static void free_cache_array(struct connection_cache_array *cache) {
    if (!cache) return;

    // 1. 释放环形数组里残留的数据包 (如果有)
    // 注意：这里 ring_buf 存的是 malloc 出来的 packet 指针
    if (cache->ring_buf) {
        for (int i = 0; i < cache->array_length; i++) {
            if (cache->ring_buf[i] != 0) {
                // 强转为 void* 释放，具体类型取决于存的数据结构
                free((void*)cache->ring_buf[i]); 
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
    memset(&key, 0, sizeof(key));

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


//====================缓存数据包相关函数=====================

int add_to_connection_cache(struct connection_cache_array* conn_cache, uint32_t psn,
                            const unsigned char *packet_data, int packet_len)
{
    // 检查输入参数有效性
    if (!src_ip || !dst_ip || !packet_data || packet_len <= 0 || packet_len > (MEM_BLOCK_SIZE - sizeof(struct mem_block_header))) {
        printf("无效的缓存参数: 输入为空或数据包过长\n");
        return -1;
    }

    // 通过流表查找获取构造连接键所需的参数（src_qp、端口、pkey等）
    // 假设数据包的源端口和目的端口暂为0，实际使用时需根据实际情况传入
    struct flow_table_entry* flow_entry = lookup_flow(src_ip, dst_ip, 0, 0, dest_qp, 0);
    if (!flow_entry) {
        printf("未找到匹配的流表条目，无法缓存数据包\n");
        return -1;
    }

    // 构造连接键（使用流表中获取的关键参数）
    struct connection_key conn_key = create_connection_key(
        src_ip,
        dst_ip,
        flow_entry->flow_key.src_port,  // 从流表键获取源端口
        flow_entry->flow_key.dst_port,  // 从流表键获取目的端口
        flow_entry->src_qp,             // 从流表值获取源QP
        dest_qp,                        // 目标QP（即目的QP）
        flow_entry->flow_key.pkey,      // 从流表键获取pkey
        0                               // 保留字段
    );

    // 查找或创建连接表条目对应的缓存结构
    struct connection_cache_array* conn_cache = get_or_create_connection_table(conn_key);
    if (!conn_cache) {
        printf("获取或创建连接缓存失败\n");
        return -1;
    }

    // 调用缓存函数处理数据包
    int ret = cache_rdma_packet(conn_cache, psn, packet_data, packet_len);
    if (ret != 0) {
        printf("数据包缓存失败 (PSN: %u)\n", psn);
        return ret;
    }

    // 更新缓存的PSN范围（cache_rdma_packet中已更新，这里做冗余保障）
    if (psn < conn_cache->start_psn) {
        conn_cache->start_psn = psn;
    }
    if (psn > conn_cache->end_psn) {
        conn_cache->end_psn = psn;
    }

    printf("数据包缓存成功 - PSN: %u, 长度: %d, 缓存范围: %u-%u\n",
           psn, packet_len, conn_cache->start_psn, conn_cache->end_psn);
    return 0;
}
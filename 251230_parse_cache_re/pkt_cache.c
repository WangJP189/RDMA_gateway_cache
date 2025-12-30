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
                struct mem_block_header *block = (struct mem_block_header*)(uintptr_t)cache->ring_buf[i];
                free(block); // 释放内存块（header+数据）
                cache->ring_buf[i] = NULL;
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
// 返回值：1（a < b）、0（a >= b）
static int psn_less_than(uint32_t a, uint32_t b) {
    // 24位PSN溢出判断：若b - a 超过2^23（即掩码的一半），说明a是溢出后的大数，b是小数
    const uint32_t HALF_PSN_MASK = PSN_MASK >> 1;
    if ((b - a) & PSN_MASK > HALF_PSN_MASK) {
        return 1; // a > b（溢出场景，比如a=0xFFFFFF，b=0x100）
    } else {
        return (a & PSN_MASK) < (b & PSN_MASK); // 正常场景比较
    }
}


//====================缓存数据包相关函数=====================

// 将数据包存入连接缓存结构
int add_to_connection_cache(struct connection_cache_array* conn_cache, uint32_t psn,
                            const unsigned char *packet_data, int packet_len)
{
    // 1. 检查新入参的有效性（适配新传参的参数校验）
    if (!conn_cache || !packet_data || packet_len <= 0 || 
        packet_len > (MEM_BLOCK_SIZE - sizeof(struct mem_block_header))) {
        printf("无效的缓存参数: conn_cache为空/输入为空/数据包过长\n");
        return -1;
    }

    // 2. 调用缓存函数处理数据包（核心逻辑不变）
    int ret = cache_rdma_packet(conn_cache, psn, packet_data, packet_len);
    if (ret != 0) {
        printf("数据包缓存失败 (PSN: %u)\n", psn);
        return ret;
    }

    // 3. 更新缓存的PSN范围（冗余保障，与原逻辑一致）
    if (psn < conn_cache->start_psn) {
        conn_cache->start_psn = psn;
    }
    if (psn > conn_cache->end_psn) {
        conn_cache->end_psn = psn;
    }

    // 4. 打印缓存成功日志（调整日志内容，适配新传参）
    printf("数据包缓存成功 - PSN: %u, 长度: %d, 缓存范围: %u-%u\n",
           psn, packet_len, conn_cache->start_psn, conn_cache->end_psn);
    return 0;
}


// 缓存RDMA数据包到内存，并将地址存入环形数组
int cache_rdma_packet(struct connection_cache_array* conn, uint32_t psn, const unsigned char* data, int data_len) {
    if (!conn || !data || data_len <= 0) {
        printf("[ERROR] 缓存数据包失败：参数无效（conn=%p, psn=%u, data_len=%d）\n",
               conn, psn, data_len);
        return -1;
    }

    // 检查数据长度是否超过内存块可用空间（5KB - 头部控制信息大小）
    int max_data_len = MEM_BLOCK_SIZE - sizeof(struct mem_block_header);
    if (data_len > max_data_len) {
        printf("[ERROR] 缓存数据包失败：数据长度超过上限（请求=%d, 上限=%d）\n", data_len, max_data_len);
        return -1;
    }

    // 分配5KB内存块
    unsigned char* mem_block = (unsigned char*)malloc(MEM_BLOCK_SIZE);
    if (!mem_block) {
        printf("[ERROR] 缓存数据包失败：内存块分配失败（5KB）\n");
        return -1;
    }

    // 写入头部控制信息 + RDMA数据包
    struct mem_block_header* header = (struct mem_block_header*)mem_block;
    header->data_len = data_len;
    header->timestamp_ms = get_current_timestamp_ms(); // 写入毫秒级时间戳
    header->psn = psn; // 写入当前内存块对应的PSN
    memcpy(mem_block + sizeof(struct mem_block_header), data, data_len);

    // 计算PSN对应的环形数组索引（取模实现环形逻辑）
    int ring_index = psn % RING_BUFFER_SIZE;

    // 处理环形数组冲突（覆盖旧数据包，释放旧内存）
    if (conn->ring_buf[ring_index] != NULL) {
        unsigned char* old_mem_block = (unsigned char*)conn->ring_buf[ring_index];
        free(old_mem_block); // 释放旧数据包内存
        printf("[OVERWRITE] 环形数组索引=%d 存在旧数据包，已释放旧内存块（地址=0x%lx）\n",
               ring_index, (uintptr_t)old_mem_block);
    }

    // 存入新数据包地址
    conn->ring_buf[ring_index] = (uintptr_t)mem_block;
    printf("[CACHE] 数据包PSN=%u → 环形数组索引=%d | 内存块首地址=0x%lx | 数据长度=%d | 时间戳=%lu ms\n",
           psn, ring_index, (uintptr_t)mem_block, data_len, header->timestamp_ms);

    // 更新连接的PSN参数
    if (psn < conn->start_psn || conn->start_psn == 0) { // 兼容初始值0的情况
        conn->start_psn = psn;
    }
    if (psn > conn->end_psn) {
        conn->end_psn = psn;
    }
    conn->cur_psn = psn;

    printf("[UPDATE] 连接PSN参数：start_psn=%u, end_psn=%u, cur_psn=%u\n",
           conn->start_psn, conn->end_psn, conn->cur_psn);

    return 0;
}

// 在连接的有效PSN范围（start_psn~end_psn）内查找丢包的数据包
int find_lost_packets(struct connection_cache_array* conn, uint32_t* lost_psns, int max_lost) {
    if (!conn || !lost_psns || max_lost <= 0) {
        printf("[ERROR] 查找丢包失败：参数无效（conn=%p, max_lost=%d）\n", conn, max_lost);
        return RETRANS_INVALID_PARAM;
    }

    // 空缓存检查
    if (conn->start_psn == 0 || conn->end_psn == 0) {
        return RETRANS_NO_CACHED_PACKETS;       // 未缓存任何数据包
    }

    uint32_t psn_start = conn->start_psn;
    uint32_t psn_end = conn->end_psn;

    // 1. 计算遍历的PSN总数（处理PSN 24位溢出场景）
    uint32_t count;
    if (psn_end >= psn_start) {
        // 非溢出场景：直接相减+1（包含首尾）
        count = psn_end - psn_start + 1;
    } else {
        // 溢出场景：分两段计算（start→PSN最大值 + 0→end）
        count = (PSN_MASK - psn_start + 1) + (psn_end + 1);
    }

    int lost_count = 0;
    printf("\n[FIND] 开始查找连接有效PSN范围[%u~%u]的丢包情况：遍历总数=%u\n", psn_start, psn_end, count);

    // 2. 循环遍历（通用框架，适配24位PSN循环溢出）
    for (uint32_t i = 0; i < count; i++) {
        // 计算当前PSN（核心：24位循环，自动处理溢出）
        uint32_t current_psn = (psn_start + i) & PSN_MASK;
        // 计算缓存索引（BUFFER_MASK适配2的幂次，高效取模）
        uint32_t cache_index = current_psn % RING_BUFFER_SIZE;;

        // 检查是否达到最大丢包存储数
        if (lost_count >= max_lost) {
            printf("[WARN] 已达到最大丢包存储数（max_lost=%d），停止查找\n", max_lost);
            break;
        }

        // 检查环形数组对应位置是否为空
        if (conn->ring_buf[cache_index] == NULL) {
            // 地址为空，说明丢包
            lost_psns[lost_count++] = current_psn;
            printf("[LOST] PSN=%u → 环形数组索引=%u | 地址为空（丢包）\n", 
                   current_psn, cache_index);
        } else {
            // 验证内存块PSN是否匹配
            unsigned char* mem_block = (unsigned char*)conn->ring_buf[cache_index];
            struct mem_block_header* header = (struct mem_block_header*) mem_block;
            if (header->psn != current_psn) {
                printf("[WARN] PSN=%u 数据包被覆盖：环形数组索引=%u | 头部PSN=%u（预期PSN=%u）\n", 
                       current_psn, cache_index, header->psn, current_psn);
                if (lost_count < max_lost) {
                    lost_psns[lost_count++] = current_psn;
                    free(mem_block); // 释放不匹配的数据
                    conn->ring_buf[cache_index] = NULL;// 标记为空，表示丢包
                }
            } else {
                printf("[FOUND] PSN=%u → 环形数组索引=%u | 数据长度=%d（正常）\n",
                       current_psn, cache_index, header->data_len);
            }
        }
    }

    printf("[FIND] 查找完成：有效PSN范围[%u~%u] | 遍历总数=%u | 丢包数=%d\n",
           psn_start, psn_end, count, lost_count);
    return lost_count;
}


// 根据ePSN处理重传：删除psn<ePSN的包，收集psn≥ePSN的包地址用于重传
retransmit_process_result process_retransmit_by_epsn(struct connection_cache_array* conn, uint32_t epsn, uint64_t** retrans_addrs, int* retrans_count) {
    if (!conn || !retrans_addrs || !retrans_count || epsn == 0) {
        printf("[ERROR] 处理重传失败：参数无效（conn=%p, epsn=%u）\n", conn, epsn);
        return RETRANS_INVALID_PARAM;
    }

    if (conn->start_psn == 0 || conn->end_psn == 0) {
        printf("[WARN] 连接未缓存任何数据包，无需处理重传\n");
        return RETRANS_NO_CACHED_PACKETS;
    }

    // 处理PSN循环场景的"start_psn >= epsn"判断
    if (!psn_less_than(conn->start_psn, epsn)) {
        printf("[INFO] 连接start_psn=%u ≥ ePSN=%u，无需处理重传\n", conn->start_psn, epsn);
        *retrans_count = 0;
        *retrans_addrs = NULL;
        return RETRANS_NO_NEED;
    }

    *retrans_count = 0;
    *retrans_addrs = NULL;

    uint32_t psn_start = conn->start_psn;
    uint32_t psn_end = conn->end_psn;

    printf("\n[RETRANS] 开始处理ePSN=%u的重传请求：\n", epsn);
    printf("原有效PSN范围：%u~%u\n", psn_start, psn_end);

    // ========== 步骤1 - 计算遍历的PSN总数（适配溢出） ==========
    uint32_t count;
    if (psn_end >= psn_start) {
        // 非溢出场景：直接相减+1（包含首尾）
        count = psn_end - psn_start + 1;
    } else {
        // 溢出场景：分两段计算（start→PSN最大值 + 0→end）
        count = (PSN_MASK - psn_start + 1) + (psn_end + 1);
    }

    // 先遍历统计删除数和需重传数（避免预分配内存错误）
    int delete_count = 0;
    int need_retrans_count = 0;
    uint64_t* temp_addrs = NULL; // 临时存储重传地址

    // ========== 步骤2 - 循环遍历（适配24位PSN溢出） ==========
    for (uint32_t i = 0; i < count; i++) {
        // 计算当前PSN（核心：24位循环，自动处理溢出）
        uint32_t current_psn = (psn_start + i) & PSN_MASK;
        // 计算缓存索引（适配120000非2的幂的场景）
        uint32_t ring_index = current_psn % RING_BUFFER_SIZE;

        // 跳过空缓存
        if (conn->ring_buf[ring_index] == NULL) {
            continue;
        }

        // 判断当前PSN是否小于ePSN（处理循环溢出）
        if (psn_less_than(current_psn, epsn)) {
            // 步骤1：删除psn < epsn的数据包
            unsigned char* mem_block = (unsigned char*)conn->ring_buf[ring_index];
            free(mem_block);
            conn->ring_buf[ring_index] = NULL;
            delete_count++;
            printf("[DELETE] PSN=%u → 环形数组索引=%u（psn < ePSN）\n", current_psn, ring_index);
        } else {
            // 步骤2：统计需重传的包数量，暂存地址
            need_retrans_count++;
            // 动态扩容存储重传地址（避免预分配错误）
            uint64_t* new_temp = (uint64_t*)realloc(temp_addrs, sizeof(uint64_t) * need_retrans_count);
            if (!new_temp) {
                printf("[ERROR] 分配重传地址数组失败，已分配%d个地址\n", need_retrans_count - 1);
                free(temp_addrs);
                *retrans_count = 0;
                *retrans_addrs = NULL;
                return RETRANS_INVALID_PARAM;
            }
            temp_addrs = new_temp;
            temp_addrs[need_retrans_count - 1] = (uint64_t)(uintptr_t)conn->ring_buf[ring_index];
            printf("[COLLECT] PSN=%u → 环形数组索引=%u（用于重传）\n", current_psn, ring_index);
        }
    }

    // 步骤3：赋值重传地址和数量
    *retrans_addrs = temp_addrs;
    *retrans_count = need_retrans_count;

    // 步骤4：更新连接的start_psn为epsn
    conn->start_psn = epsn;
    // 处理start_psn超过end_psn的场景（无有效缓存）
    if (psn_less_than(conn->end_psn, conn->start_psn)) {
        conn->start_psn = 0;
        conn->end_psn = 0;
        conn->cur_psn = 0;
        printf("[INFO] 更新start_psn=%u后无有效缓存，重置连接PSN\n", epsn);
    }

    printf("[RETRANS] 重传处理完成：删除包数量=%d | 重传包数量=%d\n", delete_count, *retrans_count);
    return RETRANS_SUCCESS;
}

// 收到ACK后清理已被确认的报文（PSN ≤ ack_msn），释放对应内存块
int clean_acked_packets(struct connection_cache_array* conn, uint32_t ack_msn) {
    if (!conn) {
        printf("[ERROR] 清理已确认报文失败：连接缓存为空\n");
        return RETRANS_NO_CACHED_PACKETS;
    }

    // 空缓存检查
    if (conn->start_psn == 0 || conn->end_psn == 0) {
        printf("[ACK CLEAN] 无有效PSN范围，无需清理\n");
        return RETRANS_NO_VALID_PSN_RANGE;
    }

    int cleaned_count = 0;
    uint32_t original_start = conn->start_psn;
    uint32_t original_end = conn->end_psn;
    uint32_t new_start = original_start;
    int has_valid_packet = 0;

    printf("[ACK CLEAN] 开始清理已确认报文：ACK MSN=%u | 原始PSN范围=[%u~%u]\n",
           ack_msn, original_start, original_end);

    // 1. 计算遍历的PSN总数（处理24位PSN溢出场景）
    uint32_t count;
    uint32_t start_psn = original_start;
    uint32_t end_psn = original_end;
    if (end_psn >= start_psn) {
        // 非溢出场景：直接相减+1（包含首尾）
        count = end_psn - start_psn + 1;
    } else {
        // 溢出场景：分两段计算（start→PSN最大值 + 0→end）
        count = (PSN_MASK - start_psn + 1) + (end_psn + 1);
    }

    // 2. 循环遍历所有PSN（通用框架，处理溢出）
    for (uint32_t i = 0; i < count; i++) {
        // 计算当前PSN（核心：24位循环，自动处理溢出）
        uint32_t current_psn = (start_psn + i) & PSN_MASK;
        // 计算缓存索引
        uint32_t cache_index = current_psn % RING_BUFFER_SIZE;

        // ========== 差异化逻辑：清理已确认报文 ==========
        // 如果该位置为空，跳过
        if (conn->ring_buf[cache_index] == NULL) continue;

        unsigned char* mem_block = (unsigned char*)conn->ring_buf[cache_index];
        struct mem_block_header* header = (struct mem_block_header*)mem_block;
        
        // // 如果PSN不匹配，说明该位置数据已被覆盖，跳过
        // if (header->psn != current_psn) continue;

        if (current_psn <= ack_msn) {
            free(mem_block);
            conn->ring_buf[cache_index] = NULL;
            cleaned_count++;
            printf("[ACKED] PSN=%u | 已被MSN=%u确认，释放内存\n", current_psn, ack_msn);
        } else {
            if (!has_valid_packet) {
                new_start = current_psn;
                has_valid_packet = 1;
            }
        }
        // ========== 差异化逻辑结束 ==========
    }

    // 更新PSN参数
    if (has_valid_packet) {
        conn->start_psn = new_start;
    } else {
        conn->start_psn = 0;
        conn->end_psn = 0;
        conn->cur_psn = 0;
    }

    printf("[ACK CLEAN] 清理完成：释放已确认报文=%d个\n", cleaned_count);
    return cleaned_count;
}

// 释放指定PSN对应的内存块，并清空环形数组对应位置
int free_packet_by_psn(struct connection_cache_array* conn, uint32_t psn) {
    if (!conn) {
        printf("[ERROR] 释放数据包失败：连接缓存为空\n");
        return -1;
    }

    int ring_index = psn % RING_BUFFER_SIZE;

    // 释放内存块
    if (conn->ring_buf[ring_index] != NULL) {
        unsigned char* mem_block = (unsigned char*)conn->ring_buf[ring_index];
        free(mem_block);
        conn->ring_buf[ring_index] = NULL;
        printf("[FREE] PSN=%u → 环形数组索引=%d | 内存块已释放\n", psn, ring_index);
    } else {
        printf("[WARN] PSN=%u → 环形数组索引=%d | 地址为空，无需释放\n", psn, ring_index);
    }

    return 0;
}

// 老化处理函数：优化版（一次遍历完成清理和参数更新）
int age_out_expired_packets(struct connection_cache_array* conn, uint64_t current_timestamp_ms) {
    if (!conn) {
        printf("[ERROR] 老化处理失败：连接缓存为空\n");
        return RETRANS_NO_CACHED_PACKETS;
    }

    // 空缓存检查：无有效PSN范围时直接返回
    if (conn->start_psn == 0 && conn->end_psn == 0) {
        printf("[AGE] 无有效PSN范围，无需老化处理\n");
        return RETRANS_NO_VALID_PSN_RANGE;
    }

    int expired_count = 0;
    uint32_t original_start = conn->start_psn;  // 记录原始start，避免遍历中被修改导致范围缩小
    uint32_t original_end = conn->end_psn;      // 记录原始end，确保遍历完整范围
    uint32_t new_start = 0;
    uint32_t new_end = 0;
    uint32_t new_current = 0;
    int has_valid_packet = 0;  // 标记是否存在未过期的有效包

    printf("[AGE] 开始老化处理：当前时间戳=%lu ms | 最大老化时间=%d ms | 原始PSN范围=[%u~%u]\n",
           current_timestamp_ms, MAX_AGE_MILLISECONDS, original_start, original_end);

    // 1. 计算遍历的PSN总数（处理24位PSN溢出场景）
    uint32_t count;
    uint32_t start_psn = original_start;
    uint32_t end_psn = original_end;
    if (end_psn >= start_psn) {
        // 非溢出场景：直接相减+1（包含首尾）
        count = end_psn - start_psn + 1;
    } else {
        // 溢出场景：分两段计算（start→PSN最大值 + 0→end）
        count = (PSN_MASK - start_psn + 1) + (end_psn + 1);
    }

    // 2. 循环遍历所有PSN（通用框架，自动处理溢出）
    for (uint32_t i = 0; i < count; i++) {
        // 计算当前PSN（核心：24位循环，自动处理溢出）
        uint32_t current_psn = (start_psn + i) & PSN_MASK;
        // 计算缓存索引
        uint32_t cache_index = current_psn % RING_BUFFER_SIZE;

        // ========== 差异化逻辑：老化处理核心逻辑 ==========
        // 跳过空位置或已被覆盖的包（PSN不匹配）
        if (conn->ring_buf[cache_index] == NULL) {
            continue;
        }

        unsigned char* mem_block = (unsigned char*)conn->ring_buf[cache_index];
        struct mem_block_header* header = (struct mem_block_header*)mem_block;
        if (header->psn != current_psn) {
            continue;
        }

        // 计算存活时间
        uint64_t survival_time_ms = current_timestamp_ms - header->timestamp_ms;

        // 处理过期包
        if (survival_time_ms > MAX_AGE_MILLISECONDS) {
            free(mem_block);
            conn->ring_buf[cache_index] = NULL;
            expired_count++;
            printf("[EXPIRED] PSN=%u | 环形索引=%d | 存活时间=%lu ms（超过限制%d ms）| 已释放\n",
                   current_psn, cache_index, survival_time_ms, MAX_AGE_MILLISECONDS);
            continue;  // 过期包不参与PSN参数更新
        }

        // 处理未过期包：更新新的PSN参数
        if (!has_valid_packet) {
            new_start = current_psn;  // 第一个未过期的PSN作为新start
            has_valid_packet = 1;
        }
        new_end = current_psn;      // 持续更新最后一个未过期的PSN作为新end
        new_current = current_psn;  // 同步更新current为最后一个有效PSN
        // ========== 差异化逻辑结束 ==========
    }

    // 根据是否有有效包更新连接的PSN参数
    if (has_valid_packet) {
        conn->start_psn = new_start;
        conn->end_psn = new_end;
        conn->cur_psn = new_current;
    } else {
        // 无有效包时重置
        conn->start_psn = 0;
        conn->end_psn = 0;
        conn->cur_psn = 0;
    }

    printf("[UPDATE] 老化处理后PSN参数：start_psn=%u, end_psn=%u, cur_psn=%u\n",
           conn->start_psn, conn->end_psn, conn->cur_psn);
    printf("[AGE] 老化处理完成：共清理过期数据包=%d个\n", expired_count);
    return expired_count;
}







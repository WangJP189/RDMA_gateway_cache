#include "pkt_cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

// ==================== FlowTable接口定义 ====================

// 全局流表定义
struct flow_table_entry* g_flow_table_forward[TABLE_SIZE] = {0};
struct flow_table_entry* g_flow_table_reverse[TABLE_SIZE] = {0};

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
static int insert_flow_entry(struct flow_table_entry **table, 
                             struct flow_key key, 
                             uint32_t hidden_src_qp, 
                             enum gateway_role role) 
{
    uint32_t hash = calc_flow_hash(&key);

    // 查重
    struct flow_table_entry *curr = table[hash];
    while (curr) {
        if (flow_key_equal(&curr->flow_key, &key)) {
            printf("[WARN] 流表规则已存在，跳过插入\n");
            return -1;
        }
        curr = curr->next;
    }

    // 分配内存
    struct flow_table_entry *new_entry = (struct flow_table_entry*)malloc(sizeof(struct flow_table_entry));
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
static int delete_flow_entry_internal(struct flow_table_entry **table, struct flow_key *key) {
    uint32_t hash = calc_flow_hash(key);
    struct flow_table_entry *prev = NULL;
    struct flow_table_entry *curr = table[hash];

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

// 调试打印流表条目
void print_flow_entry(struct flow_table_entry *entry, const char* type) {
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
                               uint32_t dst_qp, uint16_t pkey, uint16_t resv) 
{
    struct flow_key key;
    memset(&key, 0, sizeof(key));
    
    key.src_ip   = ip_str_to_host(src_ip);
    key.dst_ip   = ip_str_to_host(dst_ip);
    key.src_port = src_port;
    key.dst_port = dst_port;
    key.dst_qp   = dst_qp;
    key.pkey     = pkey;
    key.resv     = resv;
    
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
    struct flow_key fwd_key = create_flow_key(src_ip_str, dst_ip_str, src_port, dst_port, dst_qp, pkey, 0);
    if (insert_flow_entry(g_flow_table_forward, fwd_key, src_qp, src_gateway) != 0) {
        printf("[ERROR] 正向流表插入失败\n");
        return -1;
    }

    // 2. 建立反向规则 (Reverse Table)
    // 线路视角：包从 B 发往 A
    // 匹配键：Src=B, Dst=A, DstQP=QP_A
    // 目标值：SrcQP=QP_B, Role=dst_gateway
    struct flow_key rev_key = create_flow_key(dst_ip_str, src_ip_str, dst_port, src_port, src_qp, pkey, 0);
    if (insert_flow_entry(g_flow_table_reverse, rev_key, dst_qp, dst_gateway) != 0) {
        printf("[ERROR] 反向流表插入失败\n");
        return -1;
    }

    printf("[FLOW] 规则条目: %s(QP:%u) <--> %s(QP:%u)\n", src_ip_str, src_qp, dst_ip_str, dst_qp);
    return 0;
}

//查找流表项
struct flow_table_entry* lookup_flow(const char *pkt_src_ip, const char *pkt_dst_ip,
                                     uint16_t pkt_src_port, uint16_t pkt_dst_port,
                                     uint32_t pkt_dst_qp, uint16_t pkt_pkey)
{
    // 构造查询键
    struct flow_key key = create_flow_key(pkt_src_ip, pkt_dst_ip, pkt_src_port, pkt_dst_port, pkt_dst_qp, pkt_pkey, 0);
    uint32_t hash = calc_flow_hash(&key);

    // 1. 先查 Forward 表
    struct flow_table_entry *curr = g_flow_table_forward[hash];
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
    struct flow_table_entry *curr, *tmp;
    
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
struct connection_table_entry* connection_table[TABLE_SIZE] = {0};

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

    // 3. 初始化读写锁
    if (pthread_rwlock_init(&cache->rwlock, NULL) != 0) {
        perror("[ERROR] 读写锁初始化失败");
        free(cache->ring_buf);
        free(cache);
        return NULL;
    }

    return cache;
}

// 释放缓存结构
static void free_cache_array(struct connection_cache_array *cache) {
    if (!cache) return;

    // 1. 销毁锁
    pthread_rwlock_destroy(&cache->rwlock);

    // 2. 释放环形数组里残留的数据包 (如果有)
    // 注意：这里假设 ring_buf 存的是 malloc 出来的 packet 指针
    if (cache->ring_buf) {
        for (int i = 0; i < cache->array_length; i++) {
            if (cache->ring_buf[i] != 0) {
                // 强转为 void* 释放，具体类型取决于你存的数据结构
                free((void*)cache->ring_buf[i]); 
            }
        }
        // 释放数组本身
        free(cache->ring_buf);
    }

    // 3. 释放结构体
    free(cache);
}

// ==================== 接口实现 ====================

// 创建连接键
struct connection_key create_connection_key(const char *src_ip, const char *dst_ip, 
                                            uint16_t src_port, uint16_t dst_port, 
                                            uint32_t src_qp, uint32_t dst_qp,
                                            uint16_t pkey, uint16_t resv) 
{
    struct connection_key key;
    memset(&key, 0, sizeof(key));

    key.src_ip   = ip_str_to_host(src_ip);
    key.dst_ip   = ip_str_to_host(dst_ip);
    key.src_port = src_port;
    key.dst_port = dst_port;
    key.src_qp   = src_qp;
    key.dst_qp   = dst_qp;
    key.pkey     = pkey;
    key.resv     = resv;

    return key;
}

// 查找或创建连接条目
struct connection_cache_array* get_or_create_connection_table(struct connection_key key) 
{
    uint32_t hash = calc_conn_hash(&key);
    struct connection_table_entry *entry = connection_table[hash];

    // 1. 查找现有条目
    while (entry) {
        if (conn_key_equal(&entry->connection_key, &key)) {
            return entry->cache_array; // 找到了，直接返回缓存指针
        }
        entry = entry->next;
    }

    // 2. 未找到，创建新条目 (Lazy Creation)
    printf("[CONN] 新连接建立 (SrcQP:%u -> DstQP:%u)，分配缓存...\n", key.src_qp, key.dst_qp);

    struct connection_table_entry *new_entry = 
        (struct connection_table_entry*)malloc(sizeof(struct connection_table_entry));
    if (!new_entry) return NULL;

    // 填充键
    new_entry->connection_key = key;

    // 分配重型资源 (RingBuffer)
    new_entry->cache_array = alloc_cache_array(RING_BUFFER_SIZE);
    if (!new_entry->cache_array) {
        free(new_entry);
        return NULL;
    }

    // 插入哈希表 (头插法)
    new_entry->next = connection_table[hash];
    connection_table[hash] = new_entry;

    return new_entry->cache_array;
}

// 销毁特定连接条目
void remove_connection_entry(struct connection_key key) {
    uint32_t hash = calc_conn_hash(&key);
    struct connection_table_entry *curr = connection_table[hash];
    struct connection_table_entry *prev = NULL;

    while (curr) {
        if (conn_key_equal(&curr->connection_key, &key)) {
            // 摘除节点
            if (prev) {
                prev->next = curr->next;
            } else {
                connection_table[hash] = curr->next;
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
        struct connection_table_entry *curr = connection_table[i];
        while (curr) {
            struct connection_table_entry *tmp = curr;
            curr = curr->next;
            
            if (tmp->cache_array) {
                free_cache_array(tmp->cache_array);
            }
            free(tmp);
        }
        connection_table[i] = NULL;
    }
    printf("[CONN] 连接表已完全销毁\n");
}


//============================连接缓存操作====================================

int add_to_connection_cache(const char *src_ip, const char *dst_ip,
                           uint32_t dest_qp, uint32_t psn,
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


// 辅助函数：获取当前系统的毫秒级时间戳
uint64_t get_current_timestamp_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts); // 使用单调时钟，避免系统时间修改影响
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
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
        return -1;
    }

    // 空缓存检查
    if (conn->start_psn == 0 || conn->end_psn == 0) {
        return 0;  // 无缓存数据，返回0个丢失包
    }

    uint32_t psn_start = conn->start_psn;
    uint32_t psn_end = conn->end_psn;

    int lost_count = 0;
    printf("\n[FIND] 开始查找连接有效PSN范围[%u~%u]的丢包情况：\n", psn_start, psn_end);

    // 遍历有效PSN范围，检查环形数组对应位置是否为空
    for (uint32_t psn = psn_start; psn <= psn_end; psn++) {
        if (lost_count >= max_lost) {
            printf("[WARN] 已达到最大丢包存储数（max_lost=%d），停止查找\n", max_lost);
            break;
        }

        int ring_index = psn % RING_BUFFER_SIZE;

        if (conn->ring_buf[ring_index] == NULL) {
            // 地址为空，说明丢包
            lost_psns[lost_count++] = psn;
            printf("[LOST] PSN=%u → 环形数组索引=%d | 地址为空（丢包）\n", psn, ring_index);
        } else {
            // 验证内存块PSN是否匹配
            unsigned char* mem_block = (unsigned char*)conn->ring_buf[ring_index];
            struct mem_block_header* header = (struct mem_block_header*) mem_block;
            if (header->psn != psn) {
                printf("[WARN] PSN=%u 数据包被覆盖：环形数组索引=%d | 头部PSN=%u（预期PSN=%u）\n", 
                       psn, ring_index, header->psn, psn);
                if (lost_count < max_lost) {
                    lost_psns[lost_count++] = psn;
                    free(mem_block); // 释放不匹配的旧数据
                    conn->ring_buf[ring_index] = NULL;// 标记为空，表示丢包
                }
            } else {
                printf("[FOUND] PSN=%u → 环形数组索引=%d | 数据长度=%d（正常）\n",
                       psn, ring_index, header->data_len);
            }
        }
    }

    printf("[FIND] 查找完成：有效PSN范围[%u~%u] | 丢包数=%d\n",
           psn_start, psn_end, lost_count);
    return lost_count;
}


// 根据ePSN处理重传：删除psn<ePSN的包，收集psn≥ePSN的包地址用于重传
retransmit_process_result process_retransmit_by_epsn(struct connection_cache_array* conn, uint32_t epsn, 
                                                  uint64_t** retrans_addrs, int* retrans_count) {
    if (!conn || !retrans_addrs || !retrans_count || epsn == 0) {
        printf("[ERROR] 处理重传失败：参数无效（conn=%p, epsn=%u）\n", conn, epsn);
        return RETRANS_INVALID_PARAM;
    }

    if (conn->start_psn == 0 || conn->end_psn == 0) {
        printf("[WARN] 连接未缓存任何数据包，无需处理重传\n");
        return RETRANS_NO_CACHED_PACKETS;
    }

    if (conn->start_psn >= epsn) {
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

    // 步骤1：删除psn < epsn的数据包
    int delete_count = 0;
    for (uint32_t psn = psn_start; psn < epsn && psn <= psn_end; psn++) {
        int ring_index = psn % RING_BUFFER_SIZE;
        if (conn->ring_buf[ring_index] != NULL) {
            unsigned char* mem_block = (unsigned char*)conn->ring_buf[ring_index];
            free(mem_block);
            conn->ring_buf[ring_index] = NULL;
            delete_count++;
            printf("[DELETE] PSN=%u → 环形数组索引=%d（psn < ePSN）\n", psn, ring_index);
        }
    }

    // 步骤2：合并统计数量和收集地址为一次循环，减少遍历开销
    int max_retrans = psn_end >= epsn ? (psn_end - epsn + 1) : 0; // 最大可能的重传包数（用于预分配）
    if (max_retrans > 0) {
        *retrans_addrs = (uint64_t*)malloc(sizeof(uint64_t) * max_retrans);
        if (!*retrans_addrs) {
            printf("[ERROR] 分配重传地址数组失败\n");
            return RETRANS_INVALID_PARAM;
        }

        *retrans_count = 0;
        for (uint32_t psn = epsn; psn <= psn_end; psn++) {
            int ring_index = psn % RING_BUFFER_SIZE;
            if (conn->ring_buf[ring_index] != 0) { // 只收集有效数据包
                (*retrans_addrs)[*retrans_count] = (uint64_t)(uintptr_t)conn->ring_buf[ring_index];
                printf("[COLLECT] PSN=%u → 环形数组索引=%d（用于重传）\n", psn, ring_index);
                (*retrans_count)++;
            }
        }

        // 若实际重传数小于最大可能值，可收缩数组（可选优化）
        if (*retrans_count < max_retrans) {
            uint64_t* temp = (uint64_t*)realloc(*retrans_addrs, sizeof(uint64_t) * (*retrans_count));
            if (temp) {
                *retrans_addrs = temp;
            }
        }
    }

    // 步骤3：更新连接的start_psn为epsn
    conn->start_psn = epsn;
    if (conn->start_psn > conn->end_psn) {
        conn->start_psn = 0;
        conn->end_psn = 0;
        conn->cur_psn = 0;
    }

    printf("[RETRANS] 重传处理完成：删除包数量=%d | 重传包数量=%d\n", delete_count, *retrans_count);
    return RETRANS_SUCCESS;
}

// 收到ACK后清理已被确认的报文（PSN ≤ ack_msn），释放对应内存块
int clean_acked_packets(struct connection_cache_array* conn, uint32_t ack_msn) {
    if (!conn) {
        printf("[ERROR] 清理已确认报文失败：连接缓存为空\n");
        return -1;
    }

    // 空缓存检查
    if (conn->start_psn == 0 || conn->end_psn == 0) {
        printf("[ACK CLEAN] 无有效PSN范围，无需清理\n");
        return 0;
    }

    int cleaned_count = 0;
    uint32_t original_start = conn->start_psn;
    uint32_t original_end = conn->end_psn;
    uint32_t new_start = original_start;
    int has_valid_packet = 0;

    printf("[ACK CLEAN] 开始清理已确认报文：ACK MSN=%u | 原始PSN范围=[%u~%u]\n",
           ack_msn, original_start, original_end);

    // 遍历缓存中所有有效PSN，清理≤ack_msn的已确认报文
    for (uint32_t psn = original_start; psn <= original_end; psn++) {
        int ring_index = psn % RING_BUFFER_SIZE;
        //如果该位置为空，跳过
        if (conn->ring_buf[ring_index] == NULL) continue;

        unsigned char* mem_block = (unsigned char*)conn->ring_buf[ring_index];
        struct mem_block_header* header = (struct mem_block_header*)mem_block;
        
        // 如果PSN不匹配，说明该位置数据已被覆盖，跳过
        if (header->psn != psn) continue;

        if (psn <= ack_msn) {
            free(mem_block);
            conn->ring_buf[ring_index] = NULL;
            cleaned_count++;
            printf("[ACKED] PSN=%u | 已被MSN=%u确认，释放内存\n", psn, ack_msn);
        } else {
            if (!has_valid_packet) {
                new_start = psn;
                has_valid_packet = 1;
            }
        }
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
        return -1;
    }

    // 空缓存检查：无有效PSN范围时直接返回
    if (conn->start_psn == 0 && conn->end_psn == 0) {
        printf("[AGE] 无有效PSN范围，无需老化处理\n");
        return 0;
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

    // 一次遍历完成：从原始start到原始end，按顺序处理
    for (uint32_t psn = original_start; psn <= original_end; psn++) {
        int ring_index = psn % RING_BUFFER_SIZE;
        // 跳过空位置或已被覆盖的包（PSN不匹配）
        if (conn->ring_buf[ring_index] == NULL) {
            continue;
        }
        
        unsigned char* mem_block = (unsigned char*)conn->ring_buf[ring_index];
        struct mem_block_header* header = (struct mem_block_header*)mem_block;
        if (header->psn != psn) {
            continue;
        }

        // 计算存活时间
        uint64_t survival_time_ms = current_timestamp_ms - header->timestamp_ms;

        // 处理过期包
        if (survival_time_ms > MAX_AGE_MILLISECONDS) {
            free(mem_block);
            conn->ring_buf[ring_index] = NULL;
            expired_count++;
            printf("[EXPIRED] PSN=%u | 环形索引=%d | 存活时间=%lu ms（超过限制%d ms）| 已释放\n",
                   psn, ring_index, survival_time_ms, MAX_AGE_MILLISECONDS);
            continue;  // 过期包不参与PSN参数更新
        }

        // 处理未过期包：更新新的PSN参数
        if (!has_valid_packet) {
            new_start = psn;  // 第一个未过期的PSN作为新start
            has_valid_packet = 1;
        }
        new_end = psn;      // 持续更新最后一个未过期的PSN作为新end
        new_current = psn;  // 同步更新current为最后一个有效PSN
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

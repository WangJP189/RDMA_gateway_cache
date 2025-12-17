#include "pkt_cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <time.h>

// ==================== 连接表相关定义 ====================

// 全局哈希表定义
struct connection_table_entry* g_connection_table_forward[CONNECTION_TABLE_SIZE] = {0};
struct connection_table_entry* g_connection_table_reverse[CONNECTION_TABLE_SIZE] = {0};

unsigned int calculate_connection_table_hash(const struct connection_table_key *key) {
    unsigned int hash = 0;
    hash = (key->Src_IP ^ key->Dst_IP) + key->Dst_QP;
    return hash % CONNECTION_TABLE_SIZE;
}

bool connection_table_key_equal(const struct connection_table_key *key1, 
                         const struct connection_table_key *key2) {
    return (key1->Src_IP == key2->Src_IP) &&
           (key1->Dst_IP == key2->Dst_IP) &&
           (key1->Dst_QP == key2->Dst_QP);
}

struct connection_table_key create_connection_table_key(
                        const char *ip1, const char *ip2, uint32_t qp) {
    struct connection_table_key key;
    memset(&key, 0, sizeof(key));
    
    inet_pton(AF_INET, ip1, &key.Src_IP);
    inet_pton(AF_INET, ip2, &key.Dst_IP);
    key.Dst_QP = qp;
    
    return key;
}

void print_connection_table_key(const struct connection_table_key *key) {
    char ip1_str[INET_ADDRSTRLEN];
    char ip2_str[INET_ADDRSTRLEN];
    
    inet_ntop(AF_INET, &key->Src_IP, ip1_str, INET_ADDRSTRLEN);
    inet_ntop(AF_INET, &key->Dst_IP, ip2_str, INET_ADDRSTRLEN);
    
    printf("SrcIP: %s, DstIP: %s, DstQP: %u", ip1_str, ip2_str, key->Dst_QP);
}

int add_to_connection_table(struct connection_table_entry** table, 
                       const struct connection_table_key *key, uint32_t value, struct ConnectionCache *shared_meta) {
    unsigned int index = calculate_connection_table_hash(key);
    
    // 检查是否已存在
    struct connection_table_entry *current = table[index];
    while (current != NULL) {
        if (connection_table_key_equal(&current->connection_table_key, key)) {
            printf("连接表条目已存在: ");
            print_connection_table_key(key);
            printf(" <- SrcQP:: %u\n", value);
            return -1;
        }
        current = current->next;
    }
    
    // 创建新节点
    struct connection_table_entry *new_entry = 
        (struct connection_table_entry*)malloc(sizeof(struct connection_table_entry));
    if (new_entry == NULL) {
        perror("内存分配失败");
        return -1;
    }
    
    new_entry->connection_table_key = *key;
    new_entry->Src_QP = value;

    new_entry->cache_array = shared_meta;
    // 原子增加引用计数
    if (shared_meta) {
        __sync_fetch_and_add(&shared_meta->ref_count, 1);
    }

    new_entry->next = table[index];
    table[index] = new_entry;
    
    return 0;
}

int add_connection_table_entry(const char *src_ip, const char *dst_ip, 
                        uint32_t src_qp, uint32_t dst_qp) {
    struct ConnectionCache *shared_meta = create_meta_array(120);
    if (!shared_meta) return -1;
    
    // 创建正向键值对: (src_ip, dst_ip, dst_qp) -> src_qp
    struct connection_table_key key_forward = create_connection_table_key(src_ip, dst_ip, dst_qp);
    
    // 创建反向键值对: (dst_ip, src_ip, src_qp) -> dst_qp  
    struct connection_table_key key_reverse = create_connection_table_key(dst_ip, src_ip, src_qp);
    
    // 添加到两个哈希表
    if (add_to_connection_table(g_connection_table_forward, &key_forward, src_qp, shared_meta) != 0) {
        return -1;
    }
    
    if (add_to_connection_table(g_connection_table_reverse, &key_reverse, dst_qp, shared_meta) != 0) {
        // 反向添加失败时的清理逻辑
        return -1;
    }
    
    printf("成功添加连接表条目:\n");
    printf("  正向: ");
    print_connection_table_key(&key_forward);
    printf(" <- SrcQP: %u\n", src_qp);
    printf("  反向: ");
    print_connection_table_key(&key_reverse);
    printf(" <- SrcQP: %u\n", dst_qp);
    
    return 0;
}

void print_connection_table(struct connection_table_entry** table, const char *table_name) {
    printf("%s 哈希表内容:\n", table_name);
    printf("==========================================\n");
    
    int total_entries = 0;
    for (int i = 0; i < CONNECTION_TABLE_SIZE; i++) {
        struct connection_table_entry *current = table[i];
        while (current != NULL) {
            printf("哈希桶[%d]: ", i);
            print_connection_table_key(&current->connection_table_key);
            printf(" <- SrcQP: %u\n", current->Src_QP);

            struct ConnectionCache *meta = current->cache_array;
            if (meta) {
                printf("    └── [Meta] RefCount: %d, ArrayPtr: %p, Len: %d, Range: %u-%u", 
                       meta->ref_count,           
                       (void*)meta->MemArray, 
                       meta->arraylength,         
                       meta->start_psn,           
                       meta->end_psn);
            } else {
                printf("    └── [Meta] NULL");
            }
            
            printf("\n"); 

            total_entries++;
            current = current->next;
        }
    }
    
    if (total_entries == 0) {
        printf("  空表\n");
    } else {
        printf("总计: %d 个条目\n", total_entries);
    }
    printf("==========================================\n");
}

void print_all_connection_table(void) {
    print_connection_table(g_connection_table_forward, "正向连接表");
    print_connection_table(g_connection_table_reverse, "反向连接表");
}

void free_connection_table(struct connection_table_entry** table) {
    for (int i = 0; i < CONNECTION_TABLE_SIZE; i++) {
        struct connection_table_entry *current = table[i];
        while (current != NULL) {
            struct connection_table_entry *temp = current;
            current = current->next;

            if (temp->cache_array) {
                release_meta_array(temp->cache_array);
            }

            free(temp);
        }
        table[i] = NULL;
    }
}

void free_all_connection_tables() {
    free_connection_table(g_connection_table_forward);
    free_connection_table(g_connection_table_reverse);
    printf("连接表内存已释放\n");
}

uint32_t lookup_qp_mapping(const char *src_ip, const char *dst_ip, uint32_t dest_qp){

    if (!src_ip || !dst_ip) {
        return -1;
    }
    
    // 创建查找的key
    struct connection_table_key tmp_key = create_connection_table_key(src_ip, dst_ip, dest_qp);
    unsigned int tmp_hash = calculate_connection_table_hash(&tmp_key);
    
    // 在正向表中查找
    struct connection_table_entry *entry = g_connection_table_forward[tmp_hash];
    while (entry) {
        if (connection_table_key_equal(&entry->connection_table_key, &tmp_key)) {
            printf("正向表找到映射: SrcQP=%u\n", entry->Src_QP);
            return entry->Src_QP;
        }
        entry = entry->next;
    }
    
    // 在反向表中查找
    entry = g_connection_table_reverse[tmp_hash];
    while (entry) {
        if (connection_table_key_equal(&entry->connection_table_key, &tmp_key)) {
            printf("反向表找到映射: 源QP %u\n", entry->Src_QP);
            return entry->Src_QP;
        }
        entry = entry->next;
    }
    
    // 两个表中都没找到
    printf("未找到QP映射: SrcIP=%s, DstIP=%s, DstQP=%u\n", 
           src_ip, dst_ip, dest_qp);
    return -1;

}

struct ConnectionCache* create_meta_array(int length) {
    // 分配 ConnectionCache 结构体本身的内存
    struct ConnectionCache *meta = (struct ConnectionCache*)malloc(sizeof(struct ConnectionCache));
    if (!meta) {
        perror("malloc ConnectionCache failed");
        return NULL;
    }
    
    // 初始化结构体基础字段
    memset(meta, 0, sizeof(struct ConnectionCache));
    
    // 初始化环形数组（所有元素设为NULL）
    memset(meta->MemArray, 0, sizeof(meta->MemArray));
    
    // 初始化PSN参数
    meta->start_psn = UINT32_MAX;  // 初始化为最大值，方便第一次比较
    meta->end_psn   = 0;

    meta->arraylength = length;
    meta->ref_count = 0;  // 初始引用计数为0
    
    return meta;
}

void release_meta_array(struct ConnectionCache *meta) {
    if (!meta) return;

    // 原子递减引用计数
    if (__sync_sub_and_fetch(&meta->ref_count, 1) == 0) {
        // 引用计数归零，执行真正的物理释放
        free(meta);
        printf("ConnectionCache已彻底释放\n");
    }
}

// 根据三元组查找连接表条目，用于关联缓存
struct connection_table_entry* find_connection_entry(const char* src_ip, const char* dst_ip, uint32_t dst_qp) {
    struct connection_table_key key = create_connection_table_key(src_ip, dst_ip, dst_qp);
    unsigned int hash = calculate_connection_table_hash(&key);

    // 先查正向表
    struct connection_table_entry* entry = g_connection_table_forward[hash];
    while (entry) {
        if (connection_table_key_equal(&entry->connection_table_key, &key)) {
            return entry;
        }
        entry = entry->next;
    }

    // 再查反向表
    entry = g_connection_table_reverse[hash];
    while (entry) {
        if (connection_table_key_equal(&entry->connection_table_key, &key)) {
            return entry;
        }
        entry = entry->next;
    }

    return NULL; // 未找到
}

// ==================== 连接缓存相关定义 ====================

int add_to_connection_cache(const char *src_ip, const char *dst_ip,
                           uint32_t dest_qp, uint32_t psn,
                           const unsigned char *packet_data, int packet_len)
{
    // 暂未实现具体逻辑
    return 0;
}

struct connection_cache* create_connection_cache()
{
    // 暂未实现具体逻辑
    return NULL;
}

void destroy_connection_cache(struct connection_cache *cache)
{
    // 暂未实现具体逻辑
}

// 辅助函数：获取当前系统的毫秒级时间戳
uint64_t get_current_timestamp_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts); // 使用单调时钟，避免系统时间修改影响
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

// 缓存RDMA数据包到内存，并将地址存入环形数组
int cache_rdma_packet(struct ConnectionCache* conn, uint32_t psn, const unsigned char* data, int data_len) {
    if (!conn || !data || data_len <= 0) {
        printf("[ERROR] 缓存数据包失败：参数无效（conn=%p, psn=%u, data_len=%d）\n",
               conn, psn, data_len);
        return -1;
    }

    // 检查数据长度是否超过内存块可用空间（5KB - 头部控制信息大小）
    int max_data_len = MEM_BLOCK_SIZE - sizeof(MemBlockHeader);
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
    MemBlockHeader* header = (MemBlockHeader*)mem_block;
    header->data_len = data_len;
    header->timestamp_ms = get_current_timestamp_ms(); // 写入毫秒级时间戳
    memcpy(mem_block + sizeof(MemBlockHeader), data, data_len);

    // 计算PSN对应的环形数组索引（取模实现环形逻辑）
    int ring_index = psn % RING_BUFFER_SIZE;
    if (ring_index < 0 || ring_index >= RING_BUFFER_SIZE) {
        printf("[ERROR] 缓存数据包失败：PSN对应的索引超出范围（psn=%u, 索引=%d, 数组大小=%d）\n",
               psn, ring_index, RING_BUFFER_SIZE);
        free(mem_block);
        return -1;
    }

    // 处理环形数组冲突（覆盖旧数据包，释放旧内存）
    if (conn->MemArray[ring_index] != NULL) {
        unsigned char* old_mem_block = (unsigned char*)conn->MemArray[ring_index];
        free(old_mem_block); // 释放旧数据包内存
        printf("[OVERWRITE] 环形数组索引=%d 存在旧数据包，已释放旧内存块（地址=0x%lx）\n",
               ring_index, (uintptr_t)old_mem_block);
    }

    // 存入新数据包地址
    conn->MemArray[ring_index] = (uintptr_t*)mem_block;
    printf("[CACHE] 数据包PSN=%u → 环形数组索引=%d | 内存块首地址=0x%lx | 数据长度=%d | 时间戳=%lu ms\n",
           psn, ring_index, (uintptr_t)mem_block, data_len, header->timestamp_ms);

    // 更新连接的PSN参数
    if (psn < conn->start_psn) {
        conn->start_psn = psn;
    }
    if (psn > conn->end_psn) {
        conn->end_psn = psn;
    }
    conn->current_psn = psn;

    printf("[UPDATE] 连接PSN参数：start_psn=%u, end_psn=%u, current_psn=%u\n",
           conn->start_psn, conn->end_psn, conn->current_psn);

    return 0;
}

// 在连接的有效PSN范围（start_psn~end_psn）内查找丢包的数据包
int find_lost_packets(struct ConnectionCache* conn, uint32_t* lost_psns, int max_lost) {
    if (!conn || !lost_psns || max_lost <= 0) {
        printf("[ERROR] 查找丢包失败：参数无效（conn=%p, max_lost=%d）\n", conn, max_lost);
        return -1;
    }

    uint32_t psn_start = conn->start_psn;
    uint32_t psn_end = conn->end_psn;

    if (psn_start == 0 || psn_start > psn_end) {
        printf("[WARN] 连接无有效PSN范围（start_psn=%u, end_psn=%u），无丢包可查\n",
               psn_start, psn_end);
        return 0;
    }

    int lost_count = 0;
    printf("\n[FIND] 开始查找连接有效PSN范围[%u~%u]的丢包情况：\n", psn_start, psn_end);

    // 遍历有效PSN范围，检查环形数组对应位置是否为空
    for (uint32_t psn = psn_start; psn <= psn_end && lost_count < max_lost; psn++) {
        int ring_index = psn % RING_BUFFER_SIZE;
        if (ring_index < 0 || ring_index >= RING_BUFFER_SIZE) {
            printf("[WARN] PSN=%u 对应的索引超出范围（索引=%d），跳过\n", psn, ring_index);
            continue;
        }

        if (conn->MemArray[ring_index] == NULL) {
            // 地址为空，说明丢包
            lost_psns[lost_count++] = psn;
            printf("[LOST] PSN=%u → 环形数组索引=%d | 地址为空（丢包）\n", psn, ring_index);
        } else {
            // 验证内存块数据
            unsigned char* mem_block = (unsigned char*)conn->MemArray[ring_index];
            MemBlockHeader* header = (MemBlockHeader*)mem_block;
            printf("[FOUND] PSN=%u → 环形数组索引=%d | 地址=0x%lx | 数据长度=%d | 时间戳=%lu ms（正常）\n",
                   psn, ring_index, (uintptr_t)mem_block, header->data_len, header->timestamp_ms);
        }
    }

    printf("[FIND] 查找完成：有效PSN范围[%u~%u] | 总检查PSN数=%u | 丢包数=%d\n",
           psn_start, psn_end, psn_end - psn_start + 1, lost_count);
    return lost_count;
}

// 根据ePSN处理重传：删除psn<ePSN的包，收集psn≥ePSN的包地址用于重传
int process_retransmit_by_epsn(struct ConnectionCache* conn, uint32_t epsn, uint64_t** retrans_addrs, int* retrans_count) {
    if (!conn || !retrans_addrs || !retrans_count || epsn == 0) {
        printf("[ERROR] 处理重传失败：参数无效（conn=%p, epsn=%u）\n", conn, epsn);
        return -1;
    }

    *retrans_count = 0;
    *retrans_addrs = NULL;

    uint32_t psn_start = conn->start_psn;
    uint32_t psn_end = conn->end_psn;

    if (psn_start == 0 || psn_start > psn_end) {
        printf("[WARN] 连接无有效PSN范围，无需处理重传\n");
        return 0;
    }

    printf("\n[RETRANS] 开始处理ePSN=%u的重传请求：\n", epsn);
    printf("原有效PSN范围：%u~%u\n", psn_start, psn_end);

    // 步骤1：删除psn < epsn的数据包
    int delete_count = 0;
    for (uint32_t psn = psn_start; psn < epsn && psn <= psn_end; psn++) {
        int ring_index = psn % RING_BUFFER_SIZE;
        if (ring_index < 0 || ring_index >= RING_BUFFER_SIZE) {
            continue;
        }

        if (conn->MemArray[ring_index] != NULL) {
            unsigned char* mem_block = (unsigned char*)conn->MemArray[ring_index];
            free(mem_block);
            conn->MemArray[ring_index] = NULL;
            delete_count++;
            printf("[DELETE] PSN=%u → 环形数组索引=%d | 内存块已释放（psn < ePSN）\n",
                   psn, ring_index);
        }
    }

    // 步骤2：收集psn ≥ epsn的数据包地址
    int retrans_temp_count = 0;
    for (uint32_t psn = epsn; psn <= psn_end; psn++) {
        int ring_index = psn % RING_BUFFER_SIZE;
        if (ring_index < 0 || ring_index >= RING_BUFFER_SIZE) {
            continue;
        }

        if (conn->MemArray[ring_index] != NULL) {
            retrans_temp_count++;
        }
    }

    if (retrans_temp_count > 0) {
        *retrans_addrs = (uint64_t*)malloc(sizeof(uint64_t) * retrans_temp_count);
        if (!*retrans_addrs) {
            printf("[ERROR] 分配重传地址数组失败\n");
            return -1;
        }

        int idx = 0;
        for (uint32_t psn = epsn; psn <= psn_end; psn++) {
            int ring_index = psn % RING_BUFFER_SIZE;
            if (ring_index < 0 || ring_index >= RING_BUFFER_SIZE) {
                continue;
            }

            if (conn->MemArray[ring_index] != NULL) {
                (*retrans_addrs)[idx++] = (uint64_t)(uintptr_t)conn->MemArray[ring_index];
                printf("[COLLECT] PSN=%u → 环形数组索引=%d | 地址=0x%lx（用于重传）\n",
                       psn, ring_index, (uintptr_t)conn->MemArray[ring_index]);
            }
        }
        *retrans_count = idx;
    }

    // 步骤3：更新连接的start_psn为epsn
    conn->start_psn = epsn;
    if (conn->start_psn > conn->end_psn) {
        conn->start_psn = 0;
        conn->end_psn = 0;
        conn->current_psn = 0;
    }

    printf("[RETRANS] 重传处理完成：删除psn<ePSN的包数量=%d | 收集重传包数量=%d\n",
           delete_count, *retrans_count);
    printf("[UPDATE] 连接新的有效PSN范围：%u~%u\n", conn->start_psn, conn->end_psn);

    return 0;
}

// 释放指定PSN对应的内存块，并清空环形数组对应位置
int free_packet_by_psn(struct ConnectionCache* conn, uint32_t psn) {
    if (!conn) {
        printf("[ERROR] 释放数据包失败：连接缓存为空\n");
        return -1;
    }

    int ring_index = psn % RING_BUFFER_SIZE;
    if (ring_index < 0 || ring_index >= RING_BUFFER_SIZE) {
        printf("[ERROR] 释放数据包失败：PSN=%u 对应的索引超出范围（索引=%d）\n", psn, ring_index);
        return -1;
    }

    // 释放内存块
    if (conn->MemArray[ring_index] != NULL) {
        unsigned char* mem_block = (unsigned char*)conn->MemArray[ring_index];
        MemBlockHeader* header = (MemBlockHeader*)mem_block;
        uint64_t survival_time = get_current_timestamp_ms() - header->timestamp_ms;
        free(mem_block);
        conn->MemArray[ring_index] = NULL;
        printf("[FREE] PSN=%u → 环形数组索引=%d | 内存块已释放（存活时间=%lu ms）\n",
               psn, ring_index, survival_time);
    } else {
        printf("[WARN] PSN=%u → 环形数组索引=%d | 地址为空，无需释放\n", psn, ring_index);
    }

    return 0;
}

// IP字符串转网络字节序的uint32_t
uint32_t ip_str_to_uint(const char* ip) {
    struct in_addr addr;
    if (inet_pton(AF_INET, ip, &addr) != 1) {
        printf("[ERROR] 无效的IP地址：%s\n", ip);
        return 0;
    }
    return addr.s_addr; // 网络字节序
}

// 老化处理函数：根据毫秒级时间戳清理过期的数据包
int age_out_expired_packets(struct ConnectionCache* conn, uint64_t current_timestamp_ms) {
    if (!conn) {
        printf("[ERROR] 老化处理失败：连接缓存为空\n");
        return -1;
    }

    int expired_count = 0;
    printf("[AGE] 开始老化处理：当前时间戳=%lu ms | 最大老化时间=%d ms\n",
           current_timestamp_ms, MAX_AGE_MILLISECONDS);

    // 遍历环形数组，检查并清理过期数据包
    for (int ring_index = 0; ring_index < RING_BUFFER_SIZE; ring_index++) {
        if (conn->MemArray[ring_index] != NULL) {
            // 提取内存块和头部信息
            unsigned char* mem_block = (unsigned char*)conn->MemArray[ring_index];
            MemBlockHeader* header = (MemBlockHeader*)mem_block;
            // 计算数据包存活时间（毫秒）
            uint64_t survival_time_ms = current_timestamp_ms - header->timestamp_ms;

            // 判断是否过期
            if (survival_time_ms > MAX_AGE_MILLISECONDS) {
                // 释放内存并清空环形数组位置
                free(mem_block);
                conn->MemArray[ring_index] = NULL;
                expired_count++;
                printf("[EXPIRED] 环形数组索引=%d | 内存块已释放（存活时间=%lu ms，超过最大限制%d ms）\n",
                       ring_index, survival_time_ms, MAX_AGE_MILLISECONDS);
            }
        }
    }

    // 更新连接的PSN参数（如果有必要）
    if (expired_count > 0) {
        // 重新计算start_psn和end_psn
        uint32_t new_start = 0;
        uint32_t new_end = 0;
        uint32_t new_current = 0;
        int has_valid_packet = 0;

        for (uint32_t psn = conn->start_psn; psn <= conn->end_psn; psn++) {
            int ring_index = psn % RING_BUFFER_SIZE;
            if (conn->MemArray[ring_index] != NULL) {
                if (!has_valid_packet) {
                    new_start = psn;
                    has_valid_packet = 1;
                }
                new_end = psn;
                new_current = psn;
            }
        }

        if (!has_valid_packet) {
            new_start = 0;
            new_end = 0;
            new_current = 0;
        }

        conn->start_psn = new_start;
        conn->end_psn = new_end;
        conn->current_psn = new_current;

        printf("[UPDATE] 老化处理后连接PSN参数：start_psn=%u, end_psn=%u, current_psn=%u\n",
               conn->start_psn, conn->end_psn, conn->current_psn);
    }

    printf("[AGE] 老化处理完成：共清理过期数据包=%d个\n", expired_count);
    return expired_count;
}
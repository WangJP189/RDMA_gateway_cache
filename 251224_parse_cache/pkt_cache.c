#define _POSIX_C_SOURCE 199309L

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
    struct ConnectionCache *shared_meta = create_cache_array(120);
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
                release_cache_array(temp->cache_array);
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

// uint32_t lookup_qp_mapping(const char *src_ip, const char *dst_ip, uint32_t dest_qp){

//     if (!src_ip || !dst_ip) {
//         return -1;
//     }
    
//     // 创建查找的key
//     struct connection_table_key tmp_key = create_connection_table_key(src_ip, dst_ip, dest_qp);
//     unsigned int tmp_hash = calculate_connection_table_hash(&tmp_key);
    
//     // 在正向表中查找
//     struct connection_table_entry *entry = g_connection_table_forward[tmp_hash];
//     while (entry) {
//         if (connection_table_key_equal(&entry->connection_table_key, &tmp_key)) {
//             printf("正向表找到映射: SrcQP=%u\n", entry->Src_QP);
//             return entry->Src_QP;
//         }
//         entry = entry->next;
//     }
    
//     // 在反向表中查找
//     entry = g_connection_table_reverse[tmp_hash];
//     while (entry) {
//         if (connection_table_key_equal(&entry->connection_table_key, &tmp_key)) {
//             printf("反向表找到映射: 源QP %u\n", entry->Src_QP);
//             return entry->Src_QP;
//         }
//         entry = entry->next;
//     }
    
//     // 两个表中都没找到
//     printf("未找到QP映射: SrcIP=%s, DstIP=%s, DstQP=%u\n", 
//            src_ip, dst_ip, dest_qp);
//     return -1;

// }

struct ConnectionCache* create_cache_array(int length) {
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

void release_cache_array(struct ConnectionCache *meta) {
    if (!meta) return;

    // 原子递减引用计数
    if (__sync_sub_and_fetch(&meta->ref_count, 1) == 0) {
        // 1. 释放环形数组中的所有内存块
        for (int i = 0; i < meta->arraylength; i++) {
            if (meta->MemArray[i] != 0) {
                free((void*)meta->MemArray[i]);
                meta->MemArray[i] = 0;
            }
        }
        // 2. 释放结构体本身
        free(meta);
        printf("ConnectionCache已彻底释放\n");
    }
}

// 根据三元组查找连接表条目，用于关联缓存
struct connection_table_entry* find_connection_entry(const char* src_ip, const char* dst_ip, uint32_t dst_qp) {
    if (!src_ip || !dst_ip) return NULL;

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

uint64_t get_current_time_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

void update_entry_timestamp(struct connection_table_entry *entry) {
    if (!entry) return;

    // 1. 获取当前时间 (只获取一次，保证双向同步)
    uint64_t now = get_current_time_ns();

    // 2. 刷新当前条目
    entry->last_active_ns = now;

    // 3. 寻找并刷新“对端”条目
    // 利用当前条目的信息，推导对端的 Key
    // 原理：我的 Src 是你的 Dst，我的 Dst 是你的 Src，我的 Value(SrcQP) 是你的 Key(DstQP)
    struct connection_table_key peer_key;
    peer_key.Src_IP = entry->connection_table_key.Dst_IP;
    peer_key.Dst_IP = entry->connection_table_key.Src_IP;
    peer_key.Dst_QP = entry->Src_QP; // 关键：利用 Value 推导 Key

    // 计算对端的哈希值
    unsigned int hash = calculate_connection_table_hash(&peer_key);

    // 4. 在正向表和反向表中查找对端
    // 因为不知道 entry 是来自正向表还是反向表，对端可能在任意一个表中
    // 我们两个都查一下（通常只会在其中一个找到）

    // 尝试在正向表找 peer
    struct connection_table_entry *peer = g_connection_table_forward[hash];
    while (peer) {
        if (connection_table_key_equal(&peer->connection_table_key, &peer_key)) {
            peer->last_active_ns = now;
            // printf("联动刷新(Fwd): QP=%u\n", peer->connection_table_key.Dst_QP);
            return; // 找到了就返回
        }
        peer = peer->next;
    }

    // 尝试在反向表找 peer
    peer = g_connection_table_reverse[hash];
    while (peer) {
        if (connection_table_key_equal(&peer->connection_table_key, &peer_key)) {
            peer->last_active_ns = now;
            // printf("联动刷新(Rev): QP=%u\n", peer->connection_table_key.Dst_QP);
            return; // 找到了就返回
        }
        peer = peer->next;
    }
}

static void remove_entry_from_table(struct connection_table_entry **table, struct connection_table_key *key) {
    unsigned int hash = calculate_connection_table_hash(key);
    struct connection_table_entry *prev = NULL;
    struct connection_table_entry *curr = table[hash];

    while (curr) {
        if (connection_table_key_equal(&curr->connection_table_key, key)) {
            // 摘除节点
            if (prev) {
                prev->next = curr->next;
            } else {
                table[hash] = curr->next;
            }
            
            // 释放资源
            if (curr->cache_array) {
                release_cache_array(curr->cache_array);
            }
            free(curr);
            return; // 找到并删除后返回
        }
        prev = curr;
        curr = curr->next;
    }
}

void cleanup_expired_connections(void) {
    uint64_t now = get_current_time_ns();
    int cleaned_count = 0;

    for (int i = 0; i < CONNECTION_TABLE_SIZE; i++) {
        struct connection_table_entry *prev = NULL;
        struct connection_table_entry *curr = g_connection_table_forward[i];

        while (curr) {

            // =============== [新增打印] ===============
            // printf("[DEBUG] 触发 TimeOutCheck | 当前时间: %lu ns (约 %.2f 秒)\n", 
            //        now, (double)now / 1000000000.0);
            // =========================================

            // 检查是否超时
            if ((now - curr->last_active_ns) > CONN_TIMEOUT_NS) {
                struct connection_table_entry *to_free = curr;
                
                // =============== [打印时间戳] ===============
                printf("[DEBUG] 判定超时: QP=%u | 当前时间: %.2f s | 最后活跃: %.2f s | 差值: %.2f s > 阈值\n",
                       to_free->Src_QP,
                       (double)now / 1e9,
                       (double)to_free->last_active_ns / 1e9,
                       (double)(now - to_free->last_active_ns) / 1e9);
                // ==========================================

                // 1. 准备反向删除的数据 (SrcIP <-> DstIP, Value是SrcQP)
                // 注意：在正向表中，key是(SrcA, DstB, QP_B), Value是QP_A
                // 反向表的Key应该是 (DstB, SrcA, QP_A)
                
                // 我们需要手动构建反向Key来清理反向表
                struct connection_table_key rev_key;
                rev_key.Src_IP = to_free->connection_table_key.Dst_IP; // 反向Src = 正向Dst
                rev_key.Dst_IP = to_free->connection_table_key.Src_IP; // 反向Dst = 正向Src
                rev_key.Dst_QP = to_free->Src_QP;                      // 反向DstQP = 正向的Value(SrcQP)

                // 2. 从正向链表中摘除
                if (prev) {
                    prev->next = curr->next;
                } else {
                    g_connection_table_forward[i] = curr->next;
                }
                curr = curr->next; // 移动遍历指针

                // 3. 去反向表中删除对应项
                remove_entry_from_table(g_connection_table_reverse, &rev_key);

                // 4. 释放正向节点资源
                if (to_free->cache_array) {
                    release_cache_array(to_free->cache_array);
                }
                
                // 打印日志方便调试
                char src_ip[INET_ADDRSTRLEN], dst_ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &to_free->connection_table_key.Src_IP, src_ip, INET_ADDRSTRLEN);
                inet_ntop(AF_INET, &to_free->connection_table_key.Dst_IP, dst_ip, INET_ADDRSTRLEN);
                printf("[Timeout] 清理连接: %s <-> %s\n", src_ip, dst_ip);

                free(to_free);
                cleaned_count++;
            } else {
                // 未超时，继续
                prev = curr;
                curr = curr->next;
            }
        }
    }
}

// ==================== 连接缓存相关定义 ====================

int add_to_connection_cache(const char *src_ip, const char *dst_ip,
                           uint32_t dest_qp, uint32_t psn,
                           const unsigned char *packet_data, int packet_len)
{
    // 检查输入参数有效性
    if (!src_ip || !dst_ip || !packet_data || packet_len <= 0 || packet_len > (MEM_BLOCK_SIZE - sizeof(MemBlockHeader))) {
        printf("无效的缓存参数: 输入为空或数据包过长\n");
        return -1;
    }

    // 查找对应的连接表条目
    struct connection_table_entry* entry = find_connection_entry(src_ip, dst_ip, dest_qp);
    if (!entry) {
        printf("未找到匹配的连接条目，无法缓存数据包\n");
        return -1;
    }

    // 获取连接关联的缓存结构
    struct ConnectionCache* conn_cache = entry->cache_array;
    if (!conn_cache) {
        printf("连接缓存未初始化，创建新缓存\n");
        conn_cache = create_cache_array(RING_BUFFER_SIZE);
        if (!conn_cache) {
            return -1;
        }
        entry->cache_array = conn_cache;
        __sync_fetch_and_add(&conn_cache->ref_count, 1);
    }

    // 调用缓存函数处理数据包
    int ret = cache_rdma_packet(conn_cache, psn, packet_data, packet_len);
    if (ret != 0) {
        printf("数据包缓存失败 (PSN: %u)\n", psn);
        return ret;
    }

    // 更新缓存的PSN范围
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

struct connection_cache* create_connection_cache()
{
    // 分配缓存结构体内存
    struct ConnectionCache* cache = (struct ConnectionCache*)malloc(sizeof(struct ConnectionCache));
    if (!cache) {
        perror("创建连接缓存失败");
        return NULL;
    }

    // 初始化基础字段
    memset(cache, 0, sizeof(struct ConnectionCache));
    cache->arraylength = RING_BUFFER_SIZE;
    cache->start_psn = UINT32_MAX;  // 初始化为最大PSN，便于首次更新
    cache->end_psn = 0;
    cache->current_psn = 0;
    cache->ref_count = 1;  // 初始引用计数为1

    // 初始化环形数组（所有元素设为NULL）
    memset(cache->MemArray, 0, sizeof(cache->MemArray));

    printf("创建新连接缓存: 容量=%d, 地址=%p\n", RING_BUFFER_SIZE, (void*)cache);
    return (struct connection_cache*)cache;
}

void destroy_connection_cache(struct connection_cache *cache)
{
    if (!cache) return;

    struct ConnectionCache* conn_cache = (struct ConnectionCache*)cache;

    // 释放环形数组中所有内存块
    for (int i = 0; i < conn_cache->arraylength; i++) {
        if (conn_cache->MemArray[i] != 0) {
            // 释放完整的内存块（包含头部和数据）
            free((void*)conn_cache->MemArray[i]);
            conn_cache->MemArray[i] = 0;
        }
    }

    // 释放缓存结构体本身
    free(conn_cache);
    printf("连接缓存已销毁: 地址=%p\n", (void*)cache);
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
    header->psn = psn; // 写入当前内存块对应的PSN
    memcpy(mem_block + sizeof(MemBlockHeader), data, data_len);

    // 计算PSN对应的环形数组索引（取模实现环形逻辑）
    int ring_index = psn % RING_BUFFER_SIZE;

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

    // 空缓存检查
    if (conn->start_psn == UINT32_MAX || conn->end_psn == 0) {
        return 0;  // 无缓存数据，返回0个丢失包
    }

    uint32_t psn_start = conn->start_psn;
    uint32_t psn_end = conn->end_psn;

    // 处理未初始化的情况（首次缓存前）
    if (psn_start == UINT32_MAX) {
        printf("[WARN] 连接未缓存任何数据包，无丢包可查\n");
        return 0;
    }

    int lost_count = 0;
    printf("\n[FIND] 开始查找连接有效PSN范围[%u~%u]的丢包情况：\n", psn_start, psn_end);

    // 遍历有效PSN范围，检查环形数组对应位置是否为空
    for (uint32_t psn = psn_start; psn <= psn_end; psn++) {
        // 新增：如果已找到的丢包数达到max_lost，停止查找（避免越界）
        if (lost_count >= max_lost) {
            printf("[WARN] 已达到最大丢包存储数（max_lost=%d），停止查找\n", max_lost);
            break;
        }

        int ring_index = psn % RING_BUFFER_SIZE;
        if (ring_index < 0 || ring_index >= RING_BUFFER_SIZE) {
            printf("[WARN] PSN=%u 对应的索引超出范围（索引=%d），跳过\n", psn, ring_index);
            continue;
        }

        if (conn->MemArray[ring_index] == NULL) {
            // 地址为空，说明丢包：仅在未超max_lost时存储
            lost_psns[lost_count++] = psn;
            printf("[LOST] PSN=%u → 环形数组索引=%d | 地址为空（丢包）\n", psn, ring_index);
        } else {
            // 验证内存块数据
            unsigned char* mem_block = (unsigned char*)conn->MemArray[ring_index];
            MemBlockHeader* header = (MemBlockHeader*)mem_block;

            // 检查头部PSN是否匹配
            if(header->psn != psn) {
                printf("[WARN] PSN=%u 数据包被覆盖：环形数组索引=%d | 地址=0x%lx | 头部PSN=%u（预期PSN=%u）\n", psn, ring_index, (uintptr_t)mem_block, header->psn, psn);
                // 新增：仅在未超max_lost时存储
                if (lost_count < max_lost) {
                    lost_psns[lost_count++] = psn;
                    
                    //不匹配的话，释放内存块，防止误用
                    free(mem_block);
                    conn->MemArray[ring_index] = NULL;
                } else {
                    printf("[WARN] 已达到最大丢包存储数，不再记录\n");
                }
            } else {
                printf("[FOUND] PSN=%u → 环形数组索引=%d | 地址=0x%lx | 数据长度=%d | 时间戳=%lu ms（正常）\n",
                    psn, ring_index, (uintptr_t)mem_block, header->data_len, header->timestamp_ms);
            }
        }
    }

    printf("[FIND] 查找完成：有效PSN范围[%u~%u] | 总检查PSN数=%u | 丢包数=%d（最大可存储=%d）\n",
           psn_start, psn_end, psn_end - psn_start + 1, lost_count, max_lost);
    return lost_count;  // 返回实际找到的丢包数（不超过max_lost）
}


// 根据ePSN处理重传：删除psn<ePSN的包，收集psn≥ePSN的包地址用于重传
RetransmitProcessResult process_retransmit_by_epsn(struct ConnectionCache* conn, uint32_t epsn, 
                                                  uint64_t** retrans_addrs, int* retrans_count) {
    if (!conn || !retrans_addrs || !retrans_count || epsn == 0) {
        printf("[ERROR] 处理重传失败：参数无效（conn=%p, epsn=%u）\n", conn, epsn);
        return RETRANS_INVALID_PARAM;
    }

    if (conn->start_psn > conn->end_psn) {
        printf("[WARN] 连接无有效PSN范围，无需处理重传\n");
        return RETRANS_NO_VALID_PSN_RANGE;
    }

    if (conn->start_psn == UINT32_MAX || conn->end_psn == 0) {
        printf("[WARN] 连接未缓存任何数据包，无需处理重传\n");
        return RETRANS_NO_CACHED_PACKETS;
    }

    if (conn->start_psn >= epsn) {
        printf("[INFO] 连接start_psn=%u 已大于等于ePSN=%u，无需处理重传\n", conn->start_psn, epsn);
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

        if (conn->MemArray[ring_index] != NULL) {
            // 验证内存块数据
            unsigned char* mem_block = (unsigned char*)conn->MemArray[ring_index];
            MemBlockHeader* header = (MemBlockHeader*)mem_block;
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

        if (conn->MemArray[ring_index] != NULL) {
            retrans_temp_count++;
        }
    }

    if (retrans_temp_count > 0) {
        *retrans_addrs = (uint64_t*)malloc(sizeof(uint64_t) * retrans_temp_count);
        if (!*retrans_addrs) {
            printf("[ERROR] 分配重传地址数组失败\n");
            return RETRANS_INVALID_PARAM;
        }

        int idx = 0;
        for (uint32_t psn = epsn; psn <= psn_end; psn++) {
            int ring_index = psn % RING_BUFFER_SIZE;

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

    return RETRANS_SUCCESS;
}

// 释放指定PSN对应的内存块，并清空环形数组对应位置
int free_packet_by_psn(struct ConnectionCache* conn, uint32_t psn) {
    if (!conn) {
        printf("[ERROR] 释放数据包失败：连接缓存为空\n");
        return -1;
    }

    int ring_index = psn % RING_BUFFER_SIZE;

    // 释放内存块
    if (conn->MemArray[ring_index] != NULL) {
        unsigned char* mem_block = (unsigned char*)conn->MemArray[ring_index];
        free(mem_block);
        conn->MemArray[ring_index] = NULL;
        printf("[FREE] PSN=%u → 环形数组索引=%d | 内存块已释放\n", psn, ring_index);
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

// 老化处理函数：优化版（一次遍历完成清理和参数更新）
int age_out_expired_packets(struct ConnectionCache* conn, uint64_t current_timestamp_ms) {
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
        unsigned char* mem_block = (unsigned char*)conn->MemArray[ring_index];

        // 跳过空位置或已被覆盖的包（PSN不匹配）
        if (!mem_block) {
            continue;
        }
        MemBlockHeader* header = (MemBlockHeader*)mem_block;
        if (header->psn != psn) {
            continue;
        }

        // 计算存活时间
        uint64_t survival_time_ms = current_timestamp_ms - header->timestamp_ms;

        // 处理过期包
        if (survival_time_ms > MAX_AGE_MILLISECONDS) {
            free(mem_block);
            conn->MemArray[ring_index] = NULL;
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
        conn->current_psn = new_current;
    } else {
        // 无有效包时重置
        conn->start_psn = 0;
        conn->end_psn = 0;
        conn->current_psn = 0;
    }

    printf("[UPDATE] 老化处理后PSN参数：start_psn=%u, end_psn=%u, current_psn=%u\n",
           conn->start_psn, conn->end_psn, conn->current_psn);
    printf("[AGE] 老化处理完成：共清理过期数据包=%d个\n", expired_count);
    return expired_count;
}
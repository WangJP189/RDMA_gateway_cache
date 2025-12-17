#include "pkt_cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

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
    // 原子增加引用计数 (无锁)
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
        // 如果反向添加失败，需要删除正向条目
        // 这里简化处理，实际应该实现删除函数
        // ToDo
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
                       meta->ref_count,           // 引用计数
                       (void*)meta->MemArray, // 内部数组在堆上的首地址
                       meta->arraylength,         // 数组长度
                       meta->start_psn,           // PSN 范围
                       meta->end_psn);
            } else {
                printf("    └── [Meta] NULL");
            }
            
            printf("\n"); // 换行

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
    
    // 1. 首先创建查找的key
    struct connection_table_key tmp_key = create_connection_table_key(src_ip, dst_ip, dest_qp);
    unsigned int tmp_hash = calculate_connection_table_hash(&tmp_key);
    
    // 2. 在正向表中查找
    struct connection_table_entry *entry = g_connection_table_forward[tmp_hash];
    while (entry) {
        if (connection_table_key_equal(&entry->connection_table_key, &tmp_key)) {
            // 找到正向映射：返回value
            printf("正向表找到映射: ");
            // print_connection_table_key(&tmp_key);
            // printf(" -> 源QP: %u\n", entry->value);
            printf("SrcQP=%u\n", entry->Src_QP);
            return entry->Src_QP;
        }
        entry = entry->next;
    }
    
    // 3. 在反向表中查找
    entry = g_connection_table_reverse[tmp_hash];
    while (entry) {
        if (connection_table_key_equal(&entry->connection_table_key, &tmp_key)) {
            // 找到反向映射：返回value
            printf("反向表找到映射: ");
            // print_connection_table_key(&tmp_key);
            // printf(" -> 目的QP: %u\n", entry->value);
            printf(" 源QP %u\n", entry->Src_QP);
            return entry->Src_QP;
        }
        entry = entry->next;
    }
    
    // 4. 两个表中都没找到
    printf("未找到QP映射: SrcIP=%s, DstIP=%s, DstQP=%u\n", 
           src_ip, dst_ip, dest_qp);
    return -1;

}

struct ConnectionCache* create_meta_array(int length) {
    // 1. 分配 ConnectionCache 结构体本身的内存
    struct ConnectionCache *meta = (struct ConnectionCache*)malloc(sizeof(struct ConnectionCache));
    if (!meta) {
        perror("malloc ConnectionCache failed");
        return NULL;
    }
    
    // 初始化结构体基础字段
    memset(meta, 0, sizeof(struct ConnectionCache));
    
    // 将 start_psn 设为最大值，这样 min(start, new_psn) 第一次一定会更新为 new_psn
    meta->start_psn = UINT32_MAX; 
    meta->end_psn   = 0;

    meta->arraylength = length;
    meta->ref_count = 0; // 初始引用计数为0
    
    // 2. 分配内部数组 MemArray
    if (length > 0) {
        meta->MemArray = (uintptr_t*)calloc(length, sizeof(uintptr_t));
        
        // 检查分配是否成功
        if (!meta->MemArray) {
            perror("calloc MemArray failed");
            free(meta); // 既然内部数组失败，外壳也要释放
            return NULL;
        }
    } else {
        meta->MemArray = NULL;
    }
    
    return meta;
}

void release_meta_array(struct ConnectionCache *meta) {
    if (!meta) return;

    // 原子递减引用计数
    if (__sync_sub_and_fetch(&meta->ref_count, 1) == 0) {
        // 引用计数归零，执行真正的物理释放
        
        // 1. 先释放内部的数组 (如果存在)
        if (meta->MemArray) {
            // meta->MemArray = NULL;
            free(meta->MemArray);

        }
        
        // 2. 再释放结构体本身
        free(meta);
        printf("ConnectionCache及其内部数组已彻底释放\n");
    }
}

// 根据三元组查找连接表条目，用于关联缓存。返回指针或NULL
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

// struct cache_manager* init_cache_manager(size_t hash_size, 
//                                         size_t max_conns,
//                                         size_t max_packets_per_conn,
//                                         size_t max_bytes_per_conn_mb,
//                                         int conn_timeout_seconds)
// {
//     struct cache_manager *mgr = malloc(sizeof(struct cache_manager));
//     if (!mgr) {
//         perror("malloc cache_manager");
//         return NULL;
//     }
    
//     // 分配哈希表
//     mgr->hash_table_size = hash_size;
//     mgr->hash_table = calloc(hash_size, sizeof(struct hash_table_entry*));
//     if (!mgr->hash_table) {
//         perror("calloc hash_table");
//         free(mgr);
//         return NULL;
//     }
    
//     mgr->max_connections = max_conns;
//     mgr->max_packets_per_conn = max_packets_per_conn;
//     // TODO: 数字修改成宏定义, 方便全局统一修改缓存最大容量
//     mgr->max_bytes_per_conn = max_bytes_per_conn_mb * 1024 * 1024;
//     mgr->connection_timeout = conn_timeout_seconds;
//     mgr->total_connections = 0;
    
//     if (pthread_mutex_init(&mgr->global_lock, NULL) != 0) {
//         perror("pthread_mutex_init global_lock");
//         free(mgr->hash_table);
//         free(mgr);
//         return NULL;
//     }
    
//     printf("初始化缓存管理器: 哈希表大小=%zu, 最大连接数=%zu, 每连接最大报文数=%zu\n",
//            hash_size, max_conns, max_packets_per_conn);
    
//     return mgr;
// }

int add_to_connection_cache(const char *src_ip, const char *dst_ip,
                           uint32_t dest_qp, uint32_t psn,
                           const unsigned char *packet_data, int packet_len)
{
    // if (!g_cache_mgr) {
    //     // WTC ToDo
    //     // fprintf(stderr, "缓存管理器未初始化\n");
    //     // return -1;
    //     g_cache_mgr = init_cache_manager(1024, 1000, 1000, 100, 300);
    // }
    
    // // 创建连接键
    // struct connection_key key = create_connection_key(src_ip, dst_ip, src_port, 
    //                                                  dst_port, service_type, pkey, dest_qp);
    
    // // 获取或创建连接缓存
    // struct connection_cache *conn_cache = get_or_create_connection_cache(g_cache_mgr, &key);
    // if (!conn_cache) {
    //     return -1;
    // }
    
    // // 创建缓存报文
    // struct cached_packet *packet = malloc(sizeof(struct cached_packet));
    // if (!packet) {
    //     perror("malloc cached_packet");
    //     return -1;
    // }
    
    // packet->app_data = malloc(data_len);
    // if (!packet->app_data) {
    //     perror("malloc app_data");
    //     free(packet);
    //     return -1;
    // }
    
    // memcpy(packet->app_data, app_data, data_len);
    // packet->data_len = data_len;
    // packet->dest_qp = dest_qp;
    // packet->psn = psn;
    // gettimeofday(&packet->timestamp, NULL);
    // packet->next = NULL;
    // packet->prev = NULL;
    
    // // 按PSN顺序插入
    // if (insert_packet_sorted(conn_cache, packet) != 0) {
    //     free(packet->app_data);
    //     free(packet);
    //     return -1;
    // }
    
    // printf("成功缓存报文: ");
    // print_connection_key(&key);
    // printf(", QP=%u, PSN=%u, 大小=%d bytes\n", dest_qp, psn, data_len);
    
    // return 0;
}

// struct connection_key create_connection_key(const char *src_ip, const char *dst_ip,
//                                             uint16_t src_port, uint16_t dst_port,
//                                             uint8_t service_type, uint16_t pkey, uint32_t dest_qp)
// {
//     struct connection_key key;
    
//     inet_pton(AF_INET, src_ip, &key.src_ip);
//     inet_pton(AF_INET, dst_ip, &key.dst_ip);
//     key.src_port = src_port;
//     key.dst_port = dst_port;
//     key.service_type = service_type;
//     key.pkey = pkey;
//     // WTC
//     key.dest_qp = dest_qp;
//     // 查找对应的源QP
//     key.src_qp = lookup_qp_mapping(src_ip, dst_ip, dest_qp);
//     if (key.src_qp == (uint32_t)-1) {
//         key.src_qp = 0;
//         printf("警告: 未找到QP映射，连接键中的src_qp设置为0\n");
//     }
//     printf("connection_key: %s:%d:%u,%s:%d:%u\n",src_ip, key.src_port, key.src_qp, dst_ip, key.dst_port, key.dest_qp);
//     return key;
// }

// struct connection_cache* get_or_create_connection_cache(
//         struct cache_manager *mgr, const struct connection_key *key)
// {
    // uint32_t hash_index = calculate_hash(key, mgr->hash_table_size);
    
    // pthread_mutex_lock(&mgr->global_lock);
    
    // // 查找现有连接
    // struct hash_table_entry *entry = mgr->hash_table[hash_index];
    // while (entry) {
    //     if (connection_keys_equal(&entry->key, key)) {
    //         // 更新最后活动时间
    //         gettimeofday(&entry->cache->last_activity, NULL);
    //         pthread_mutex_unlock(&mgr->global_lock);
    //         return entry->cache;
    //     }
    //     entry = entry->next;
    // }
    
    // // 检查连接数限制
    // if (mgr->total_connections >= mgr->max_connections) {
    //     printf("达到最大连接数限制 (%zu/%zu), 无法创建新连接缓存\n",
    //            mgr->total_connections, mgr->max_connections);
    //     pthread_mutex_unlock(&mgr->global_lock);
    //     return NULL;
    // }
    
    // // 创建新连接缓存
    // struct connection_cache *new_cache = create_connection_cache();
    // if (!new_cache) {
    //     pthread_mutex_unlock(&mgr->global_lock);
    //     return NULL;
    // }
    
    // // 创建哈希表条目
    // struct hash_table_entry *new_entry = malloc(sizeof(struct hash_table_entry));
    // if (!new_entry) {
    //     perror("malloc hash_table_entry");
    //     destroy_connection_cache(new_cache);
    //     pthread_mutex_unlock(&mgr->global_lock);
    //     return NULL;
    // }
    
    // new_entry->key = *key;
    // new_entry->cache = new_cache;
    // new_entry->next = mgr->hash_table[hash_index];
    // mgr->hash_table[hash_index] = new_entry;
    
    // mgr->total_connections++;
    
    // pthread_mutex_unlock(&mgr->global_lock);
    
    // printf("创建新连接缓存: ");
    // print_connection_key(key);
    // printf("\n");
    
    // return new_cache;
//}

// uint32_t calculate_hash(const struct connection_key *key, size_t table_size)
// {
//     uint32_t hash = 5381;
    
//     hash = ((hash << 5) + hash) + key->src_ip;
//     hash = ((hash << 5) + hash) + key->dst_ip;
//     hash = ((hash << 5) + hash) + key->src_port;
//     hash = ((hash << 5) + hash) + key->dst_port;
//     hash = ((hash << 5) + hash) + key->service_type;
//     hash = ((hash << 5) + hash) + key->pkey;
//     // WTC ToDo
//     return hash % table_size;
// }

// int connection_keys_equal(const struct connection_key *a,
//                           const struct connection_key *b)
// {
//     // WTC ToDo
//     return (a->src_ip == b->src_ip &&
//             a->dst_ip == b->dst_ip &&
//             a->src_port == b->src_port &&
//             a->dst_port == b->dst_port &&
//             a->service_type == b->service_type &&
//             a->pkey == b->pkey);
// }

struct connection_cache* create_connection_cache()
{
    // struct connection_cache *cache = malloc(sizeof(struct connection_cache));
    // if (!cache) {
    //     perror("malloc connection_cache");
    //     return NULL;
    // }
    
    // cache->head = NULL;
    // cache->tail = NULL;
    // cache->count = 0;
    // cache->total_bytes = 0;
    // cache->min_psn = 0;
    // cache->max_psn = 0;
    // gettimeofday(&cache->last_activity, NULL);
    
    // if (pthread_mutex_init(&cache->lock, NULL) != 0) {
    //     perror("pthread_mutex_init connection_cache lock");
    //     free(cache);
    //     return NULL;
    // }
    
    // return cache;
}


void destroy_connection_cache(struct connection_cache *cache)
{
    // if (!cache) return;
    
    // pthread_mutex_lock(&cache->lock);
    
    // // 清空所有缓存报文
    // struct cached_packet *current = cache->head;
    // while (current) {
    //     struct cached_packet *next = current->next;
    //     free(current->app_data);
    //     free(current);
    //     current = next;
    // }
    
    // pthread_mutex_unlock(&cache->lock);
    // pthread_mutex_destroy(&cache->lock);
    // free(cache);
}

// void print_connection_key(const struct connection_key *key)
// {
//     char src_ip[INET_ADDRSTRLEN];
//     char dst_ip[INET_ADDRSTRLEN];
    
//     inet_ntop(AF_INET, &key->src_ip, src_ip, INET_ADDRSTRLEN);
//     inet_ntop(AF_INET, &key->dst_ip, dst_ip, INET_ADDRSTRLEN);
    
//     // WTC ToDo

//     printf("连接: %s:%d -> %s:%d, 服务类型=%d, pkey=0x%04x",
//            src_ip, key->src_port, dst_ip, key->dst_port, 
//            key->service_type, key->pkey);
// }

int insert_packet_sorted(struct connection_cache *cache, 
                         struct cached_packet *new_packet) 
{

}




// 辅助函数：获取当前系统的毫秒级时间戳（自解释，支持高精度时间计算）
uint64_t get_current_timestamp_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts); // 使用单调时钟，避免系统时间修改影响
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

// 3. 缓存RDMA数据包到内存，并将地址存入环形数组（直接传入连接缓存）
int cache_rdma_packet(ConnectionCache* conn, uint32_t psn, const unsigned char* data, int data_len) {
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

    // 步骤1：分配5KB内存块
    unsigned char* mem_block = (unsigned char*)malloc(MEM_BLOCK_SIZE);
    if (!mem_block) {
        printf("[ERROR] 缓存数据包失败：内存块分配失败（5KB）\n");
        return -1;
    }

    // 步骤2：写入头部控制信息 + RDMA数据包
    MemBlockHeader* header = (MemBlockHeader*)mem_block;
    header->data_len = data_len;
    header->timestamp_ms = get_current_timestamp_ms(); // 写入毫秒级时间戳
    memcpy(mem_block + sizeof(MemBlockHeader), data, data_len);

    // 步骤3：计算PSN对应的环形数组索引（取模实现环形逻辑）
    int ring_index = psn % RING_BUFFER_SIZE;
    if (ring_index < 0 || ring_index >= RING_BUFFER_SIZE) {
        printf("[ERROR] 缓存数据包失败：PSN对应的索引超出范围（psn=%u, 索引=%d, 数组大小=%d）\n",
               psn, ring_index, RING_BUFFER_SIZE);
        free(mem_block);
        return -1;
    }

    // 步骤4：处理环形数组冲突（覆盖旧数据包，释放旧内存）
    if (conn->ring_buffer[ring_index] != 0) {
        unsigned char* old_mem_block = (unsigned char*)(uintptr_t)conn->ring_buffer[ring_index];
        free(old_mem_block); // 释放旧数据包内存
        printf("[OVERWRITE] 环形数组索引=%d 存在旧数据包，已释放旧内存块（地址=0x%lx）\n",
               ring_index, (uintptr_t)old_mem_block);
    }

    // 步骤5：存入新数据包地址
    conn->ring_buffer[ring_index] = (uint64_t)(uintptr_t)mem_block;
    printf("[CACHE] 数据包PSN=%u → 环形数组索引=%d | 内存块首地址=0x%lx | 数据长度=%d | 时间戳=%lu ms\n",
           psn, ring_index, (uintptr_t)mem_block, data_len, header->timestamp_ms);

    // 步骤6：更新连接的PSN参数
    if (conn->start_psn == 0 || psn < conn->start_psn) {
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

// 4. 在连接的有效PSN范围（start_psn~end_psn）内查找丢包的数据包
int find_lost_packets(ConnectionCache* conn, uint32_t* lost_psns, int max_lost) {
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

        if (conn->ring_buffer[ring_index] == 0) {
            // 地址为空，说明丢包
            lost_psns[lost_count++] = psn;
            printf("[LOST] PSN=%u → 环形数组索引=%d | 地址为空（丢包）\n", psn, ring_index);
        } else {
            // 验证内存块数据
            unsigned char* mem_block = (unsigned char*)(uintptr_t)conn->ring_buffer[ring_index];
            MemBlockHeader* header = (MemBlockHeader*)mem_block;
            printf("[FOUND] PSN=%u → 环形数组索引=%d | 地址=0x%lx | 数据长度=%d | 时间戳=%lu ms（正常）\n",
                   psn, ring_index, (uintptr_t)mem_block, header->data_len, header->timestamp_ms);
        }
    }

    printf("[FIND] 查找完成：有效PSN范围[%u~%u] | 总检查PSN数=%u | 丢包数=%d\n",
           psn_start, psn_end, psn_end - psn_start + 1, lost_count);
    return lost_count;
}

// 5. 根据ePSN处理重传：删除psn<ePSN的包，收集psn≥ePSN的包地址用于重传
int process_retransmit_by_epsn(ConnectionCache* conn, uint32_t epsn, uint64_t** retrans_addrs, int* retrans_count) {
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

        if (conn->ring_buffer[ring_index] != 0) {
            unsigned char* mem_block = (unsigned char*)(uintptr_t)conn->ring_buffer[ring_index];
            free(mem_block);
            conn->ring_buffer[ring_index] = 0;
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

        if (conn->ring_buffer[ring_index] != 0) {
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

            if (conn->ring_buffer[ring_index] != 0) {
                (*retrans_addrs)[idx++] = conn->ring_buffer[ring_index];
                printf("[COLLECT] PSN=%u → 环形数组索引=%d | 地址=0x%lx（用于重传）\n",
                       psn, ring_index, (uintptr_t)conn->ring_buffer[ring_index]);
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

// 6. 释放指定PSN对应的内存块，并清空环形数组对应位置
int free_packet_by_psn(ConnectionCache* conn, uint32_t psn) {
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
    if (conn->ring_buffer[ring_index] != 0) {
        unsigned char* mem_block = (unsigned char*)(uintptr_t)conn->ring_buffer[ring_index];
        MemBlockHeader* header = (MemBlockHeader*)mem_block;
        uint64_t survival_time = get_current_timestamp_ms() - header->timestamp_ms;
        free(mem_block);
        conn->ring_buffer[ring_index] = 0;
        printf("[FREE] PSN=%u → 环形数组索引=%d | 内存块已释放（存活时间=%lu ms）\n",
               psn, ring_index, survival_time);
    } else {
        printf("[WARN] PSN=%u → 环形数组索引=%d | 地址为空，无需释放\n", psn, ring_index);
    }

    return 0;
}

// 7. IP字符串转网络字节序的uint32_t
uint32_t ip_str_to_uint(const char* ip) {
    struct in_addr addr;
    if (inet_pton(AF_INET, ip, &addr) != 1) {
        printf("[ERROR] 无效的IP地址：%s\n", ip);
        return 0;
    }
    return addr.s_addr; // 网络字节序
}

// 8. 老化处理函数：根据毫秒级时间戳清理过期的数据包（核心修改）
int age_out_expired_packets(ConnectionCache* conn, uint64_t current_timestamp_ms) {
    if (!conn) {
        printf("[ERROR] 老化处理失败：连接缓存为空\n");
        return -1;
    }

    int expired_count = 0;
    printf("[AGE] 开始老化处理：当前时间戳=%lu ms | 最大老化时间=%d ms\n",
           current_timestamp_ms, MAX_AGE_MILLISECONDS);

    // 遍历环形数组，仅检查并清理过期数据包（无额外PSN参数更新，降低时间复杂度）
    for (int ring_index = 0; ring_index < RING_BUFFER_SIZE; ring_index++) {
        if (conn->ring_buffer[ring_index] != 0) {
            // 提取内存块和头部信息
            unsigned char* mem_block = (unsigned char*)(uintptr_t)conn->ring_buffer[ring_index];
            MemBlockHeader* header = (MemBlockHeader*)mem_block;
            // 计算数据包存活时间（毫秒）
            uint64_t survival_time_ms = current_timestamp_ms - header->timestamp_ms;

            // 判断是否过期
            if (survival_time_ms > MAX_AGE_MILLISECONDS) {
                // 释放内存并清空环形数组位置
                free(mem_block);
                conn->ring_buffer[ring_index] = 0;
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
            if (conn->ring_buffer[ring_index] != 0) {
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
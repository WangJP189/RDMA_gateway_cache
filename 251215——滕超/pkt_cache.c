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
                       const struct connection_table_key *key, uint32_t value, struct MetaArray *shared_meta) {
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
    struct MetaArray *shared_meta = create_meta_array(120);
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

            struct MetaArray *meta = current->cache_array;
            if (meta) {
                printf("    └── [Meta] RefCount: %d, ArrayPtr: %p, Len: %d, Range: %u-%u", 
                       meta->ref_count,           // 引用计数
                       (void*)meta->MetaDataArry, // 内部数组在堆上的首地址
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

struct MetaArray* create_meta_array(int length) {
    // 1. 分配 MetaArray 结构体本身的内存
    struct MetaArray *meta = (struct MetaArray*)malloc(sizeof(struct MetaArray));
    if (!meta) {
        perror("malloc MetaArray failed");
        return NULL;
    }
    
    // 初始化结构体基础字段
    memset(meta, 0, sizeof(struct MetaArray));
    
    // 将 start_psn 设为最大值，这样 min(start, new_psn) 第一次一定会更新为 new_psn
    meta->start_psn = UINT32_MAX; 
    meta->end_psn   = 0;

    meta->arraylength = length;
    meta->ref_count = 0; // 初始引用计数为0
    
    // 2. 分配内部数组 MetaDataArry
    if (length > 0) {
        meta->MetaDataArry = (uintptr_t*)calloc(length, sizeof(uintptr_t));
        
        // 检查分配是否成功
        if (!meta->MetaDataArry) {
            perror("calloc MetaDataArry failed");
            free(meta); // 既然内部数组失败，外壳也要释放
            return NULL;
        }
    } else {
        meta->MetaDataArry = NULL;
    }
    
    return meta;
}

void release_meta_array(struct MetaArray *meta) {
    if (!meta) return;

    // 原子递减引用计数
    if (__sync_sub_and_fetch(&meta->ref_count, 1) == 0) {
        // 引用计数归零，执行真正的物理释放
        
        // 1. 先释放内部的数组 (如果存在)
        if (meta->MetaDataArry) {
            free(meta->MetaDataArry);
            meta->MetaDataArry = NULL;
        }
        
        // 2. 再释放结构体本身
        free(meta);
        printf("MetaArray及其内部数组已彻底释放\n");
    }
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
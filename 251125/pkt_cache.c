// RDMA报文缓存管理系统（环形缓存+多连接批量处理+每个连接）的的对应1个进程
/*
整体逻辑：
1. 单连接管理：每个连接使用独立环形内存区，通过PSN直接计算存储偏移
2. 多连接管理：每个连接对应1个进程，通过哈希表快速定位连接缓存
3. 批量缓存机制：定时或达到阈值时批量处理缓存请求，防止某些连接过慢让其他连接等待
4. 重传优化：通过PSN直接定位数据包，实现快速查找

编译命令：
gcc pkt_cache.c -o pkt_cache -lpthread -lrdmacm -libverbs

运行命令：
sudo ./pkt_cache
*/

// RDMA报文缓存管理系统 - 简化稳定版本
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/time.h>
#include <arpa/inet.h>

// 配置参数
#define MAX_CONNECTIONS 10           // 最大连接数
#define RING_BUFFER_SIZE 1000        // 每个连接的环形缓冲区大小（包数）
#define MAX_PACKET_SIZE 4096         // 最大包大小

// 连接标识键
struct connection_key {
    uint32_t src_ip;
    uint32_t dst_ip;
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t src_qp;
    uint32_t dest_qp;
};

// 缓存的数据包
struct cached_packet {
    unsigned char data[MAX_PACKET_SIZE];
    int data_len;
    uint32_t psn;
    int valid;
    struct timeval timestamp;
};

// 单个连接的环形缓存
struct connection_cache {
    struct cached_packet ring[RING_BUFFER_SIZE];
    uint32_t base_psn;
    uint32_t min_psn;
    uint32_t max_psn;
    uint32_t window_start;
    uint32_t window_size;
    size_t total_bytes;
    struct timeval last_activity;
    pthread_mutex_t lock;
};

// 哈希表节点
struct hash_entry {
    struct connection_key key;
    struct connection_cache *cache;
    struct hash_entry *next;
};

// 缓存管理器
struct cache_manager {
    struct hash_entry **hash_table;
    size_t hash_table_size;
    size_t total_connections;
    pthread_mutex_t global_lock;
};

// 全局缓存管理器
struct cache_manager *g_cache_mgr = NULL;

// 简化哈希函数
uint32_t calculate_hash(const struct connection_key *key, size_t table_size) {
    return (key->src_ip + key->dst_ip + key->src_port + key->dst_port) % table_size;
}

// 连接键比较
int connection_keys_equal(const struct connection_key *a, const struct connection_key *b) {
    return (a->src_ip == b->src_ip &&
            a->dst_ip == b->dst_ip &&
            a->src_port == b->src_port &&
            a->dst_port == b->dst_port &&
            a->src_qp == b->src_qp &&
            a->dest_qp == b->dest_qp);
}

// 创建连接键
struct connection_key create_connection_key(const char *src_ip, const char *dst_ip,
                                          uint16_t src_port, uint16_t dst_port,
                                          uint32_t src_qp, uint32_t dest_qp) {
    struct connection_key key;
    memset(&key, 0, sizeof(key));
    inet_pton(AF_INET, src_ip, &key.src_ip);
    inet_pton(AF_INET, dst_ip, &key.dst_ip);
    key.src_port = src_port;
    key.dst_port = dst_port;
    key.src_qp = src_qp;
    key.dest_qp = dest_qp;
    return key;
}

// 创建连接缓存
struct connection_cache* create_connection_cache(uint32_t first_psn) {
    struct connection_cache *cache = malloc(sizeof(struct connection_cache));
    if (!cache) return NULL;
    
    memset(cache, 0, sizeof(struct connection_cache));
    cache->base_psn = first_psn;
    cache->min_psn = first_psn;
    cache->max_psn = first_psn;
    cache->window_start = first_psn;
    cache->window_size = 32; // 默认窗口大小
    gettimeofday(&cache->last_activity, NULL);
    
    if (pthread_mutex_init(&cache->lock, NULL) != 0) {
        free(cache);
        return NULL;
    }
    
    return cache;
}

// 计算PSN在环形缓冲区中的位置
static inline size_t psn_to_index(struct connection_cache *cache, uint32_t psn) {
    return (psn - cache->base_psn) % RING_BUFFER_SIZE;
}

// 插入数据包到缓存
int insert_packet(struct connection_cache *cache, uint32_t psn, 
                 const unsigned char *data, int data_len) {
    if (!cache || !data || data_len <= 0 || data_len > MAX_PACKET_SIZE) {
        return -1;
    }
    
    pthread_mutex_lock(&cache->lock);
    
    // 计算位置
    size_t index = psn_to_index(cache, psn);
    
    // 检查是否在窗口内
    if (psn < cache->window_start || psn >= cache->window_start + cache->window_size) {
        printf("PSN %u 超出窗口范围 [%u, %u]\n", 
               psn, cache->window_start, cache->window_start + cache->window_size - 1);
        pthread_mutex_unlock(&cache->lock);
        return -1;
    }
    
    // 更新数据包
    struct cached_packet *packet = &cache->ring[index];
    
    if (packet->valid) {
        // 释放旧数据占用的字节数
        cache->total_bytes -= packet->data_len;
    }
    
    memcpy(packet->data, data, data_len);
    packet->data_len = data_len;
    packet->psn = psn;
    packet->valid = 1;
    gettimeofday(&packet->timestamp, NULL);
    
    // 更新统计信息
    cache->total_bytes += data_len;
    if (psn < cache->min_psn) cache->min_psn = psn;
    if (psn > cache->max_psn) cache->max_psn = psn;
    gettimeofday(&cache->last_activity, NULL);
    
    pthread_mutex_unlock(&cache->lock);
    return 0;
}

// 查找数据包
struct cached_packet* find_packet(struct connection_cache *cache, uint32_t psn) {
    if (!cache) return NULL;
    
    pthread_mutex_lock(&cache->lock);
    
    size_t index = psn_to_index(cache, psn);
    struct cached_packet *packet = &cache->ring[index];
    
    if (!packet->valid || packet->psn != psn) {
        pthread_mutex_unlock(&cache->lock);
        return NULL;
    }
    
    // 返回数据副本
    struct cached_packet *result = malloc(sizeof(struct cached_packet));
    if (result) {
        memcpy(result, packet, sizeof(struct cached_packet));
        // 复制数据内容
        result->data_len = packet->data_len;
        memcpy(result->data, packet->data, packet->data_len);
    }
    
    pthread_mutex_unlock(&cache->lock);
    return result;
}

// 获取或创建连接缓存
struct connection_cache* get_or_create_cache(struct cache_manager *mgr, 
                                           const struct connection_key *key,
                                           uint32_t first_psn) {
    uint32_t hash_index = calculate_hash(key, mgr->hash_table_size);
    
    pthread_mutex_lock(&mgr->global_lock);
    
    // 查找现有连接
    struct hash_entry *entry = mgr->hash_table[hash_index];
    while (entry) {
        if (connection_keys_equal(&entry->key, key)) {
            gettimeofday(&entry->cache->last_activity, NULL);
            pthread_mutex_unlock(&mgr->global_lock);
            return entry->cache;
        }
        entry = entry->next;
    }
    
    // 检查连接数限制
    if (mgr->total_connections >= MAX_CONNECTIONS) {
        printf("达到最大连接数限制 (%zu/%d)\n", mgr->total_connections, MAX_CONNECTIONS);
        pthread_mutex_unlock(&mgr->global_lock);
        return NULL;
    }
    
    // 创建新连接缓存
    struct connection_cache *new_cache = create_connection_cache(first_psn);
    if (!new_cache) {
        pthread_mutex_unlock(&mgr->global_lock);
        return NULL;
    }
    
    struct hash_entry *new_entry = malloc(sizeof(struct hash_entry));
    if (!new_entry) {
        free(new_cache);
        pthread_mutex_unlock(&mgr->global_lock);
        return NULL;
    }
    
    new_entry->key = *key;
    new_entry->cache = new_cache;
    new_entry->next = mgr->hash_table[hash_index];
    mgr->hash_table[hash_index] = new_entry;
    mgr->total_connections++;
    
    printf("创建新连接缓存: %u.%u.%u.%u:%u -> %u.%u.%u.%u:%u (QP%u->QP%u)\n",
           (key->src_ip >> 24) & 0xFF, (key->src_ip >> 16) & 0xFF, 
           (key->src_ip >> 8) & 0xFF, key->src_ip & 0xFF, key->src_port,
           (key->dst_ip >> 24) & 0xFF, (key->dst_ip >> 16) & 0xFF,
           (key->dst_ip >> 8) & 0xFF, key->dst_ip & 0xFF, key->dst_port,
           key->src_qp, key->dest_qp);
    
    pthread_mutex_unlock(&mgr->global_lock);
    return new_cache;
}

// 批量插入数据包（简化版本）
int add_to_batch_queue(const char *src_ip, const char *dst_ip,
                      uint16_t src_port, uint16_t dst_port,
                      uint32_t src_qp, uint32_t dest_qp,
                      uint32_t psn, const unsigned char *app_data, int data_len) {
    if (!g_cache_mgr || !app_data || data_len <= 0) {
        return -1;
    }
    
    // 创建连接键
    struct connection_key key = create_connection_key(src_ip, dst_ip, src_port, dst_port, src_qp, dest_qp);
    
    // 获取或创建缓存
    struct connection_cache *cache = get_or_create_cache(g_cache_mgr, &key, psn);
    if (!cache) {
        return -1;
    }
    
    // 直接插入数据包
    return insert_packet(cache, psn, app_data, data_len);
}

// 初始化缓存管理器
struct cache_manager* init_cache_manager(size_t hash_size) {
    struct cache_manager *mgr = malloc(sizeof(struct cache_manager));
    if (!mgr) return NULL;
    
    mgr->hash_table_size = hash_size;
    mgr->hash_table = calloc(hash_size, sizeof(struct hash_entry*));
    if (!mgr->hash_table) {
        free(mgr);
        return NULL;
    }
    
    mgr->total_connections = 0;
    
    if (pthread_mutex_init(&mgr->global_lock, NULL) != 0) {
        free(mgr->hash_table);
        free(mgr);
        return NULL;
    }
    
    printf("初始化缓存管理器: 哈希表大小=%zu, 最大连接数=%d\n", hash_size, MAX_CONNECTIONS);
    return mgr;
}

// 销毁缓存管理器
void destroy_cache_manager(struct cache_manager *mgr) {
    if (!mgr) return;
    
    for (size_t i = 0; i < mgr->hash_table_size; i++) {
        struct hash_entry *entry = mgr->hash_table[i];
        while (entry) {
            struct hash_entry *next = entry->next;
            if (entry->cache) {
                pthread_mutex_destroy(&entry->cache->lock);
                free(entry->cache);
            }
            free(entry);
            entry = next;
        }
    }
    
    free(mgr->hash_table);
    pthread_mutex_destroy(&mgr->global_lock);
    free(mgr);
}

// 打印连接状态
void print_all_connections_status() {
    if (!g_cache_mgr) {
        printf("缓存管理器未初始化\n");
        return;
    }
    
    printf("\n===== 连接状态 =====\n");
    printf("总连接数: %zu/%d\n", g_cache_mgr->total_connections, MAX_CONNECTIONS);
    
    pthread_mutex_lock(&g_cache_mgr->global_lock);
    
    for (size_t i = 0; i < g_cache_mgr->hash_table_size; i++) {
        struct hash_entry *entry = g_cache_mgr->hash_table[i];
        while (entry) {
            struct connection_key *key = &entry->key;
            struct connection_cache *cache = entry->cache;
            
            printf("连接: %u.%u.%u.%u:%u -> %u.%u.%u.%u:%u (QP%u->QP%u)\n",
                   (key->src_ip >> 24) & 0xFF, (key->src_ip >> 16) & 0xFF,
                   (key->src_ip >> 8) & 0xFF, key->src_ip & 0xFF, key->src_port,
                   (key->dst_ip >> 24) & 0xFF, (key->dst_ip >> 16) & 0xFF,
                   (key->dst_ip >> 8) & 0xFF, key->dst_ip & 0xFF, key->dst_port,
                   key->src_qp, key->dest_qp);
            
            pthread_mutex_lock(&cache->lock);
            printf("  PSN范围: %u-%u, 窗口: [%u, %u], 缓存数据: %zu bytes\n",
                   cache->min_psn, cache->max_psn,
                   cache->window_start, cache->window_start + cache->window_size - 1,
                   cache->total_bytes);
            pthread_mutex_unlock(&cache->lock);
            
            entry = entry->next;
        }
    }
    
    pthread_mutex_unlock(&g_cache_mgr->global_lock);
    printf("====================\n");
}

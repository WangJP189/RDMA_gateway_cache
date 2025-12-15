#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "pkt_cache.h"

// 全局缓存管理器
struct cache_manager *g_cache_mgr = NULL;

// 计算五元组哈希值
static uint32_t connection_hash(const struct connection_key *key, size_t table_size) {
    if (!key || table_size == 0) return 0;
    uint64_t hash = key->src_ip;
    hash ^= key->dst_ip + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    hash ^= key->src_port + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    hash ^= key->dst_port + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    hash ^= key->qp + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    return (uint32_t)(hash % table_size);
}

// 比较两个连接键是否相等
static int connection_key_equal(const struct connection_key *a, const struct connection_key *b) {
    if (!a || !b) return 0;
    return (a->src_ip == b->src_ip &&
            a->dst_ip == b->dst_ip &&
            a->src_port == b->src_port &&
            a->dst_port == b->dst_port &&
            a->qp == b->qp);
}

// 创建连接缓存
static struct connection_cache* create_connection_cache(const struct connection_key *key) {
    if (!key) return NULL;

    struct connection_cache *cache = malloc(sizeof(struct connection_cache));
    if (!cache) {
        printf("[ERROR] 创建连接缓存失败：内存分配失败\n");
        return NULL;
    }

    // 初始化成员变量
    memset(cache, 0, sizeof(struct connection_cache));
    cache->key = *key;
    cache->start_psn = 0;
    cache->end_psn = 0;
    cache->ring_used_count = 0;
    
    // 初始化环形数组所有指针为NULL
    for (int i = 0; i < RING_BUFFER_SIZE; i++) {
        cache->packet_ptr_ring[i] = NULL;
    }

    // 初始化互斥锁
    if (pthread_mutex_init(&cache->ring_lock, NULL) != 0 ||
        pthread_mutex_init(&cache->mem_lock, NULL) != 0) {
        printf("[ERROR] 创建连接缓存失败：锁初始化失败\n");
        free(cache);
        return NULL;
    }

    printf("[INFO] 成功创建连接缓存\n");
    return cache;
}

// 销毁连接缓存
static void destroy_connection_cache(struct connection_cache *cache) {
    if (!cache) return;

    // 释放所有缓存的数据包内存
    pthread_mutex_lock(&cache->ring_lock);
    for (int i = 0; i < RING_BUFFER_SIZE; i++) {
        if (cache->packet_ptr_ring[i]) {
            free(cache->packet_ptr_ring[i]);
            cache->packet_ptr_ring[i] = NULL;
        }
    }
    pthread_mutex_unlock(&cache->ring_lock);

    // 销毁互斥锁
    pthread_mutex_destroy(&cache->ring_lock);
    pthread_mutex_destroy(&cache->mem_lock);
    free(cache);
    
    printf("[INFO] 成功销毁连接缓存\n");
}

// 计算PSN在环形数组中的索引
static int calculate_ring_index(uint32_t psn) {
    // 环形数组索引 = PSN % 环形数组大小（确保循环利用）
    return psn % RING_BUFFER_SIZE;
}

// 初始化缓存管理器
struct cache_manager* init_cache_manager(size_t max_conns) {
    if (max_conns == 0) {
        printf("[ERROR] 最大连接数不能为0\n");
        return NULL;
    }

    struct cache_manager *mgr = malloc(sizeof(struct cache_manager));
    if (!mgr) {
        printf("[ERROR] 缓存管理器初始化失败：内存分配失败\n");
        return NULL;
    }

    memset(mgr, 0, sizeof(struct cache_manager));
    mgr->max_connections = max_conns;
    mgr->hash_table_size = max_conns * 2;  // 哈希表大小为最大连接数的2倍
    mgr->hash_table = calloc(mgr->hash_table_size, sizeof(struct hash_table_entry*));
    
    // 分配连接缓存数组
    mgr->conn_caches = calloc(mgr->max_connections, sizeof(struct connection_cache *));
    if (!mgr->conn_caches) {
        printf("[ERROR] 缓存管理器初始化失败：连接数组分配失败\n");
        free(mgr->hash_table);
        free(mgr);
        return NULL;
    }

    if (!mgr->hash_table) {
        printf("[ERROR] 缓存管理器初始化失败：哈希表分配失败\n");
        free(mgr->conn_caches);
        free(mgr);
        return NULL;
    }

    pthread_mutex_init(&mgr->global_lock, NULL);
    mgr->total_connections = 0;

    printf("[INFO] 缓存管理器初始化成功（最大连接数: %zu）\n", mgr->max_connections);
    return mgr;
}

// 销毁缓存管理器
void destroy_cache_manager(struct cache_manager *mgr) {
    if (!mgr) return;

    pthread_mutex_lock(&mgr->global_lock);

    // 销毁所有连接缓存
    for (size_t i = 0; i < mgr->hash_table_size; i++) {
        struct hash_table_entry *entry = mgr->hash_table[i];
        while (entry) {
            struct hash_table_entry *next = entry->next;
            destroy_connection_cache(entry->cache);
            free(entry);
            entry = next;
        }
    }

    free(mgr->hash_table);
    free(mgr->conn_caches);
    pthread_mutex_destroy(&mgr->global_lock);
    free(mgr);
    
    printf("[INFO] 缓存管理器销毁完成\n");
}

// 添加数据包到缓存
int add_packet_to_cache(const char *src_ip, const char *dst_ip,
                       uint16_t src_port, uint16_t dst_port,
                       uint32_t src_qp, uint32_t dest_qp,
                       uint32_t psn, const unsigned char *data, int data_len) {
    if (!g_cache_mgr || !src_ip || !dst_ip || !data || data_len <= 0 || data_len > MEM_BLOCK_SIZE) {
        printf("[ERROR] 无效的数据包参数\n");
        return -1;
    }

    // 构建连接键
    struct connection_key key;
    key.src_ip = ip_str_to_uint(src_ip);
    key.dst_ip = ip_str_to_uint(dst_ip);
    key.src_port = htons(src_port);  // 转换为网络字节序
    key.dst_port = htons(dst_port);
    key.qp = src_qp;  // 使用源QP作为连接标识

    pthread_mutex_lock(&g_cache_mgr->global_lock);

    // 查找现有连接
    uint32_t hash = connection_hash(&key, g_cache_mgr->hash_table_size);
    struct hash_table_entry *entry = g_cache_mgr->hash_table[hash];
    struct connection_cache *cache = NULL;

    while (entry) {
        if (connection_key_equal(&entry->cache->key, &key)) {
            cache = entry->cache;
            break;
        }
        entry = entry->next;
    }

    // 未找到则创建新连接
    if (!cache) {
        if (g_cache_mgr->total_connections >= g_cache_mgr->max_connections) {
            pthread_mutex_unlock(&g_cache_mgr->global_lock);
            printf("[ERROR] 达到最大连接数限制（%zu）\n", g_cache_mgr->max_connections);
            return -1;
        }

        cache = create_connection_cache(&key);
        if (!cache) {
            pthread_mutex_unlock(&g_cache_mgr->global_lock);
            return -1;
        }

        // 添加到哈希表
        struct hash_table_entry *new_entry = malloc(sizeof(struct hash_table_entry));
        if (!new_entry) {
            destroy_connection_cache(cache);
            pthread_mutex_unlock(&g_cache_mgr->global_lock);
            printf("[ERROR] 哈希表条目分配失败\n");
            return -1;
        }
        new_entry->cache = cache;
        new_entry->next = g_cache_mgr->hash_table[hash];
        g_cache_mgr->hash_table[hash] = new_entry;
        g_cache_mgr->total_connections++;

        // 存入连接数组
        for (size_t i = 0; i < g_cache_mgr->max_connections; i++) {
            if (!g_cache_mgr->conn_caches[i]) {
                g_cache_mgr->conn_caches[i] = cache;
                break;
            }
        }
        printf("[INFO] 新增连接缓存，当前连接数: %zu\n", g_cache_mgr->total_connections);
    }

    pthread_mutex_unlock(&g_cache_mgr->global_lock);

    // 分配内存块存储数据包
    struct cached_packet *pkt = malloc(sizeof(struct cached_packet));
    if (!pkt) {
        printf("[ERROR] 数据包内存分配失败\n");
        return -1;
    }
    memcpy(pkt->data, data, data_len);
    pkt->data_len = data_len;
    pkt->psn = psn;

    // 计算环形数组索引
    int ring_idx = calculate_ring_index(psn);
    if (ring_idx < 0 || ring_idx >= RING_BUFFER_SIZE) {
        free(pkt);
        printf("[ERROR] 无效的环形数组索引: %d\n", ring_idx);
        return -1;
    }

    // 存入环形数组
    pthread_mutex_lock(&cache->ring_lock);
    
    // 如果该位置已有数据，先释放旧数据
    if (cache->packet_ptr_ring[ring_idx]) {
        free(cache->packet_ptr_ring[ring_idx]);
        cache->ring_used_count--;
        printf("[WARN] 环形数组位置%d已有数据，已释放旧数据\n", ring_idx);
    }

    // 存入新数据指针
    cache->packet_ptr_ring[ring_idx] = pkt;
    cache->ring_used_count++;

    // 更新PSN范围
    if (cache->start_psn == 0 || psn < cache->start_psn) {
        cache->start_psn = psn;
    }
    if (psn > cache->end_psn) {
        cache->end_psn = psn;
    }

    pthread_mutex_unlock(&cache->ring_lock);

    printf("[CACHE] PSN=%u 存入环形数组位置%d | 当前使用数: %zu | PSN范围: %u-%u\n",
           psn, ring_idx, cache->ring_used_count, cache->start_psn, cache->end_psn);
    return 0;
}

// 处理重传请求
int process_retransmit_request(struct connection_cache *cache, uint32_t ePSN,
                              struct cached_packet **retrans_pkts, size_t *count) {
    if (!cache || !retrans_pkts || !count) {
        printf("[ERROR] 重传请求参数无效\n");
        return -1;
    }

    *count = 0;
    *retrans_pkts = NULL;

    // 收集需要重传的数据包
    pthread_mutex_lock(&cache->ring_lock);
    
    // 先统计数量
    for (int i = 0; i < RING_BUFFER_SIZE; i++) {
        if (cache->packet_ptr_ring[i] && cache->packet_ptr_ring[i]->psn >= ePSN) {
            (*count)++;
        }
    }

    if (*count == 0) {
        pthread_mutex_unlock(&cache->ring_lock);
        printf("[RETRANS] 没有需要重传的数据包（ePSN=%u）\n", ePSN);
        return 0;
    }

    // 分配内存存储重传包
    *retrans_pkts = malloc(sizeof(struct cached_packet) * (*count));
    if (!*retrans_pkts) {
        pthread_mutex_unlock(&cache->ring_lock);
        *count = 0;
        printf("[ERROR] 重传包数组分配失败\n");
        return -1;
    }

    // 复制需要重传的数据包
    size_t idx = 0;
    for (int i = 0; i < RING_BUFFER_SIZE; i++) {
        struct cached_packet *pkt = cache->packet_ptr_ring[i];
        if (pkt && pkt->psn >= ePSN) {
            (*retrans_pkts)[idx] = *pkt;  // 复制数据包内容
            idx++;
            
            // 释放原内存并置空指针
            free(pkt);
            cache->packet_ptr_ring[i] = NULL;
            cache->ring_used_count--;
        }
    }

    pthread_mutex_unlock(&cache->ring_lock);

    printf("[RETRANS] 处理重传请求完成（ePSN=%u）| 重传包数量: %zu | 剩余缓存数: %zu\n",
           ePSN, *count, cache->ring_used_count);
    return 0;
}

// 打印单个连接状态
void print_connection_status(struct connection_cache *cache) {
    if (!cache) return;

    char src_ip_str[INET_ADDRSTRLEN];
    char dst_ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &cache->key.src_ip, src_ip_str, INET_ADDRSTRLEN);
    inet_ntop(AF_INET, &cache->key.dst_ip, dst_ip_str, INET_ADDRSTRLEN);

    printf("\n===== 连接状态 =====");
    printf("\n源地址: %s:%u (QP%u)",
           src_ip_str, ntohs(cache->key.src_port), cache->key.qp);
    printf("\n目的地址: %s:%u",
           dst_ip_str, ntohs(cache->key.dst_port));
    printf("\n环形数组大小: %d", RING_BUFFER_SIZE);
    printf("\n已使用数量: %zu", cache->ring_used_count);
    printf("\nPSN有效范围: %u - %u", cache->start_psn, cache->end_psn);
    
    // 打印前5个有效数据位置
    printf("\n前5个有效数据位置: ");
    pthread_mutex_lock(&cache->ring_lock);
    int shown = 0;
    for (int i = 0; i < RING_BUFFER_SIZE && shown < 5; i++) {
        if (cache->packet_ptr_ring[i]) {
            printf("位置%d(PSN=%u) ", i, cache->packet_ptr_ring[i]->psn);
            shown++;
        }
    }
    if (shown == 0) printf("无");
    pthread_mutex_unlock(&cache->ring_lock);
    printf("\n====================\n");
}

// 打印所有连接状态
void print_all_connections_status() {
    if (!g_cache_mgr) return;

    printf("\n===== 所有连接缓存状态 =====");
    printf("\n总连接数: %zu (最大: %zu)\n",
           g_cache_mgr->total_connections, g_cache_mgr->max_connections);

    pthread_mutex_lock(&g_cache_mgr->global_lock);
    for (size_t i = 0; i < g_cache_mgr->hash_table_size; i++) {
        struct hash_table_entry *entry = g_cache_mgr->hash_table[i];
        while (entry) {
            print_connection_status(entry->cache);
            entry = entry->next;
        }
    }
    pthread_mutex_unlock(&g_cache_mgr->global_lock);
}

// IP字符串转网络字节序
uint32_t ip_str_to_uint(const char *ip) {
    struct in_addr addr;
    if (inet_pton(AF_INET, ip, &addr) != 1) {
        printf("[ERROR] 无效的IP地址: %s\n", ip);
        return 0;
    }
    return addr.s_addr;
}
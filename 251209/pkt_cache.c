#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/time.h>
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

// 计算初始内存地址（基于五元组哈希）
uint32_t calculate_initial_addr(const struct connection_key *key) {
    if (!key) return 0;
    return (key->src_ip ^ key->dst_ip ^ key->src_port ^ key->dst_port ^ key->qp) % INIT_MEM_SIZE;
}

// 初始化批量缓冲区
int init_batch_buffer(struct batch_buffer *buffer) {
    if (!buffer) return -1;
    
    memset(buffer, 0, sizeof(struct batch_buffer));
    buffer->head = 0;
    buffer->tail = 0;
    buffer->count = 0;
    buffer->base_psn = 0;
    gettimeofday(&buffer->last_flush, NULL);
    return pthread_mutex_init(&buffer->lock, NULL);
}

// 初始化线性内存区
struct linear_memory* init_linear_memory(size_t capacity) {
    struct linear_memory *mem = malloc(sizeof(struct linear_memory));
    if (!mem) return NULL;

    mem->blocks = malloc(sizeof(struct cached_packet) * capacity);
    if (!mem->blocks) {
        free(mem);
        return NULL;
    }

    memset(mem->blocks, 0, sizeof(struct cached_packet) * capacity);
    mem->capacity = capacity;
    mem->used = 0;
    mem->next = NULL;
    pthread_mutex_init(&mem->lock, NULL);

    print("初始化线性内存区，容量 %zu 个块\n", mem->capacity);

    return mem;
}


// 扩展线性内存区
int extend_linear_memory(struct linear_memory *mem) {
    if (!mem) return -1;

    while (mem->next) {
        mem = mem->next;
    }

    struct linear_memory *new_mem = init_linear_memory(EXTEND_MEM_SIZE);
    if (!new_mem) return -1;

    mem->next = new_mem;
    printf("扩展内存成功，新增 %d 个块\n", EXTEND_MEM_SIZE);
    return 0;
}

// 插入数据包到缓冲区
int insert_to_buffer(struct connection_cache *cache, uint32_t psn, 
                    const unsigned char *data, int data_len) {
    if (!cache || !data || data_len <= 0 || data_len > MEM_BLOCK_SIZE) {
        return -1;
    }

    pthread_mutex_lock(&cache->buffer.lock);

    if (cache->buffer.count >= BUFFER_CAPACITY) {
        pthread_mutex_unlock(&cache->buffer.lock);
        return -1;
    }

    int insert_pos = (cache->buffer.tail) % BUFFER_CAPACITY;
    struct cached_packet *pkt = &cache->buffer.packets[insert_pos];

    memcpy(pkt->data, data, data_len);
    pkt->data_len = data_len;
    pkt->psn = psn;
    pkt->valid = 1;

    cache->buffer.tail = (cache->buffer.tail + 1) % BUFFER_CAPACITY;
    cache->buffer.count++;

    if (psn < cache->min_psn || cache->min_psn == 0) {
        cache->min_psn = psn;
    }
    if (psn > cache->max_psn) {
        cache->max_psn = psn;
    }

    pthread_mutex_unlock(&cache->buffer.lock);
    return 0;
}

// 刷新缓冲区到内存
int flush_buffer_to_memory(struct connection_cache *cache) {
    if (!cache || cache->buffer.count == 0) {
        return 0;
    }

    pthread_mutex_lock(&cache->buffer.lock);
    if (cache->buffer.count == 0) {
        pthread_mutex_unlock(&cache->buffer.lock);
        return 0;
    }

    struct cached_packet *packets = malloc(sizeof(struct cached_packet) * cache->buffer.count);
    if (!packets) {
        pthread_mutex_unlock(&cache->buffer.lock);
        return -1;
    }

    int count = 0;
    int pos = cache->buffer.head;
    while (count < cache->buffer.count) {
        if (cache->buffer.packets[pos].valid) {
            packets[count] = cache->buffer.packets[pos];
            count++;
        }
        pos = (pos + 1) % BUFFER_CAPACITY;
    }

    // 按PSN排序
    for (int i = 0; i < count - 1; i++) {
        for (int j = 0; j < count - i - 1; j++) {
            if (packets[j].psn > packets[j + 1].psn) {
                struct cached_packet temp = packets[j];
                packets[j] = packets[j + 1];
                packets[j + 1] = temp;
            }
        }
    }

    cache->buffer.base_psn = packets[count - 1].psn;

    struct linear_memory *current_mem = cache->memory;
    int written = 0;

    while (written < count) {
        pthread_mutex_lock(&current_mem->lock);
        
        size_t available = current_mem->capacity - current_mem->used;
        size_t to_write = (available < count - written) ? available : count - written;

        if (to_write > 0) {
            memcpy(&current_mem->blocks[current_mem->used], 
                   &packets[written], 
                   to_write * sizeof(struct cached_packet));
            current_mem->used += to_write;
            written += to_write;
        }

        pthread_mutex_unlock(&current_mem->lock);

        if (written < count && !current_mem->next) {
            if (extend_linear_memory(current_mem) != 0) {
                free(packets);
                pthread_mutex_unlock(&cache->buffer.lock);
                return -1;
            }
        }

        if (current_mem->next) {
            current_mem = current_mem->next;
        }
    }

    free(packets);

    cache->buffer.head = cache->buffer.tail;
    cache->buffer.count = 0;
    gettimeofday(&cache->buffer.last_flush, NULL);

    pthread_mutex_unlock(&cache->buffer.lock);
    return 0;
}

// 定时刷新线程函数
void* batch_flush_thread(void *arg) {
    struct connection_cache *cache = (struct connection_cache *)arg;
    struct timeval now;
    struct timeval diff;

    while (cache->running) {
        usleep(1000);

        pthread_mutex_lock(&cache->buffer.lock);
        gettimeofday(&now, NULL);
        
        diff.tv_sec = now.tv_sec - cache->buffer.last_flush.tv_sec;
        diff.tv_usec = now.tv_usec - cache->buffer.last_flush.tv_usec;
        long ms_diff = diff.tv_sec * 1000 + diff.tv_usec / 1000;

        if (cache->buffer.count >= BATCH_THRESHOLD || ms_diff >= BATCH_TIMEOUT_MS) {
            if (cache->buffer.count > 0) {
                pthread_mutex_unlock(&cache->buffer.lock);
                flush_buffer_to_memory(cache);
            } else {
                pthread_mutex_unlock(&cache->buffer.lock);
            }
        } else {
            pthread_mutex_unlock(&cache->buffer.lock);
        }
    }

    return NULL;
}

// 创建连接缓存
struct connection_cache* create_connection_cache(const struct connection_key *key) {
    if (!key) return NULL;

    struct connection_cache *cache = malloc(sizeof(struct connection_cache));
    if (!cache) return NULL;

    memset(cache, 0, sizeof(struct connection_cache));
    cache->key = *key;
    cache->running = 1;
    cache->min_psn = 0;
    cache->max_psn = 0;

    if (init_batch_buffer(&cache->buffer) != 0) {
        free(cache);
        return NULL;
    }

    cache->memory = init_linear_memory(INIT_MEM_SIZE);
    if (!cache->memory) {
        free(cache);
        return NULL;
    }

    if (pthread_create(&cache->flush_thread, NULL, batch_flush_thread, cache) != 0) {
        destroy_connection_cache(cache);
        return NULL;
    }

    return cache;
}

// 销毁连接缓存
void destroy_connection_cache(struct connection_cache *cache) {
    if (!cache) return;

    cache->running = 0;

    if (pthread_self() != cache->flush_thread && cache->flush_thread != 0) {
        pthread_cancel(cache->flush_thread);
        pthread_join(cache->flush_thread, NULL);
    }

    struct linear_memory *current = cache->memory;
    while (current) {
        struct linear_memory *next = current->next;
        if (current->blocks) free(current->blocks);
        pthread_mutex_destroy(&current->lock);
        free(current);
        current = next;
    }

    pthread_mutex_destroy(&cache->buffer.lock);
    free(cache);
}

// 根据ePSN处理重传请求
int process_retransmit_request(struct connection_cache *cache, uint32_t ePSN,
                              struct cached_packet **retrans_pkts, size_t *count) {
    if (!cache || !retrans_pkts || !count) return -1;

    *count = 0;
    *retrans_pkts = NULL;

    struct linear_memory *current = cache->memory;
    while (current) {
        pthread_mutex_lock(&current->lock);
        
        for (size_t i = 0; i < current->used; i++) {
            if (current->blocks[i].valid && current->blocks[i].psn >= ePSN) {
                (*count)++;
            }
        }
        
        pthread_mutex_unlock(&current->lock);
        current = current->next;
    }

    if (*count == 0) return 0;

    *retrans_pkts = malloc(sizeof(struct cached_packet) * (*count));
    if (!*retrans_pkts) return -1;

    size_t idx = 0;
    current = cache->memory;
    while (current) {
        pthread_mutex_lock(&current->lock);
        
        for (size_t i = 0; i < current->used; i++) {
            if (current->blocks[i].valid) {
                if (current->blocks[i].psn >= ePSN) {
                    (*retrans_pkts)[idx++] = current->blocks[i];
                } else {
                    // memset(&current->blocks[i], 0, sizeof(struct cached_packet));

                    //仅把psn<ePSN的包标记为无效,而不区清除
                    current->blocks[i].valid = 0;
                }
            }
        }
        
        pthread_mutex_unlock(&current->lock);
        current = current->next;
    }

    return 0;
}

// 初始化缓存管理器
struct cache_manager* init_cache_manager(size_t max_conns) {
    if (max_conns == 0) return NULL;

    struct cache_manager *mgr = malloc(sizeof(struct cache_manager));
    if (!mgr) return NULL;

    memset(mgr, 0, sizeof(struct cache_manager));
    mgr->max_connections = max_conns;
    mgr->hash_table_size = max_conns * 2;  // 哈希表大小为最大连接数的2倍
    mgr->hash_table = calloc(mgr->hash_table_size, sizeof(struct hash_table_entry*));
    
    //为mgr分配连接缓存数组
    mgr->conn_caches = calloc(mgr->max_connections, sizeof(struct connection_cache *));
    if (!mgr->conn_caches) {
        free(mgr->hash_table);
        free(mgr);
        return NULL;
    }

    if (!mgr->hash_table) {
        free(mgr);
        return NULL;
    }

    pthread_mutex_init(&mgr->global_lock, NULL);
    mgr->total_connections = 0;

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
    pthread_mutex_destroy(&mgr->global_lock);
    free(mgr);
}

// 添加数据包到缓存
int add_packet_to_cache(const char *src_ip, const char *dst_ip,
                       uint16_t src_port, uint16_t dst_port,
                       uint32_t src_qp, uint32_t dest_qp,
                       uint32_t psn, const unsigned char *data, int data_len) {
    if (!g_cache_mgr || !src_ip || !dst_ip || !data || data_len <= 0) {
        return -1;
    }

    struct connection_key key;
    key.src_ip = ip_str_to_uint(src_ip);
    key.dst_ip = ip_str_to_uint(dst_ip);
    key.src_port = htons(src_port);  // 转换为网络字节序
    key.dst_port = htons(dst_port);
    key.qp = src_qp;  // 使用源QP作为连接标识的QP

    pthread_mutex_lock(&g_cache_mgr->global_lock);

    // 检查是否已存在该连接
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

    // 不存在则创建新连接
    if (!cache) {
        if (g_cache_mgr->total_connections >= g_cache_mgr->max_connections) {
            pthread_mutex_unlock(&g_cache_mgr->global_lock);
            return -1;  // 达到最大连接数
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
            return -1;
        }
        new_entry->cache = cache;
        new_entry->next = g_cache_mgr->hash_table[hash];
        g_cache_mgr->hash_table[hash] = new_entry;
        g_cache_mgr->total_connections++;

        // 找到第一个空位置存入连接
        for (size_t i = 0; i < g_cache_mgr->max_connections; i++) {
            if (!g_cache_mgr->conn_caches[i]) {
                g_cache_mgr->conn_caches[i] = cache;
                break;
            }
        }
    }

    pthread_mutex_unlock(&g_cache_mgr->global_lock);

    // 插入数据包到缓冲区
    return insert_to_buffer(cache, psn, data, data_len);
}

// 打印单个连接状态
void print_connection_status(struct connection_cache *cache) {
    if (!cache) return;

    char src_ip_str[INET_ADDRSTRLEN];
    char dst_ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &cache->key.src_ip, src_ip_str, INET_ADDRSTRLEN);
    inet_ntop(AF_INET, &cache->key.dst_ip, dst_ip_str, INET_ADDRSTRLEN);

    printf("连接: %s:%u (QP%u) -> %s:%u\n",
           src_ip_str, ntohs(cache->key.src_port), cache->key.qp,
           dst_ip_str, ntohs(cache->key.dst_port));
    printf("  PSN范围: %u - %u\n", cache->min_psn, cache->max_psn);
    
    // 计算总缓存包数
    size_t total_pkts = 0;
    struct linear_memory *current = cache->memory;
    while (current) {
        pthread_mutex_lock(&current->lock);
        total_pkts += current->used;
        pthread_mutex_unlock(&current->lock);
        current = current->next;
    }
    printf("  缓存包数: %zu\n", total_pkts);
    printf("  缓冲区当前包数: %d\n", cache->buffer.count);
}

// 打印所有连接状态
void print_all_connections_status() {
    if (!g_cache_mgr) return;

    printf("当前缓存连接数: %zu (最大: %zu)\n",
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

// 转换IP字符串到网络字节序（从测试文件移过来的共享函数）
uint32_t ip_str_to_uint(const char *ip) {
    struct in_addr addr;
    inet_pton(AF_INET, ip, &addr);
    return addr.s_addr;
}
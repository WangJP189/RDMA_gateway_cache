#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <unistd.h>

// #include "pkt_cache.h"

// 配置参数
#define MTU_SIZE 1500              // MTU大小
#define MEM_BLOCK_SIZE (MTU_SIZE)  // 内存块大小
#define BUFFER_CAPACITY 100        // buffer容量(阈值的2倍)
#define BATCH_THRESHOLD 50         // 批量处理阈值
#define BATCH_TIMEOUT_MS 10        // 批量处理超时(ms)
#define INIT_MEM_SIZE 1024         // 初始内存块数量
#define EXTEND_MEM_SIZE 512        // 内存扩展块数量


// 缓存报文结构
struct cached_packet {
    unsigned char data[MEM_BLOCK_SIZE];  // 数据块
    int data_len;                        // 实际数据长度
    uint32_t psn;                        // 包序列号
    int valid;                           // 有效性标记
};

// 连接标识键（五元组）
struct connection_key {
    uint32_t src_ip;      // 源IP
    uint32_t dst_ip;      // 目的IP
    uint16_t src_port;    // 源端口
    uint16_t dst_port;    // 目的端口
    uint32_t qp;          // QP信息
};

// 批量处理缓冲区
struct batch_buffer {
    struct cached_packet packets[BUFFER_CAPACITY];  // 数据包缓冲区
    int head;                     // 头指针
    int tail;                     // 尾指针
    int count;                    // 当前数量
    struct timeval last_flush;    // 上次刷新时间
    uint32_t base_psn;            // 基准PSN
    pthread_mutex_t lock;         // 缓冲区锁
};

// 线性内存区
struct linear_memory {
    struct cached_packet *blocks;  // 内存块数组
    size_t capacity;               // 总容量
    size_t used;                   // 已使用数量
    struct linear_memory *next;    // 下一段内存(用于扩展)
    pthread_mutex_t lock;          // 内存锁
};

// 单个连接的缓存管理结构
struct connection_cache {
    struct connection_key key;     // 连接键
    struct batch_buffer buffer;    // 批量缓冲区
    struct linear_memory *memory;  // 线性内存区
    uint32_t min_psn;              // 最小PSN
    uint32_t max_psn;              // 最大PSN
    pthread_t flush_thread;        // 定时刷新线程
    int running;                   // 运行标志
};

// 全局缓存管理器
struct cache_manager {
    struct hash_table_entry **hash_table;   // 哈希表
    size_t hash_table_size;                 // 哈希表大小
    size_t max_connections;           // 最大连接数
    size_t max_packets_per_conn;      // 每连接最大报文数
    size_t max_bytes_per_conn;        // 每连接最大字节数
    int connection_timeout;           // 连接超时时间（秒）
    pthread_mutex_t global_lock;      // 全局锁
    size_t total_connections;         // 总连接数
};

// 前置声明（解决隐式声明警告）
void destroy_connection_cache(struct connection_cache *cache);

// 全局缓存管理器
struct cache_manager *g_cache_mgr = NULL;


// 计算初始内存地址（基于五元组哈希）
uint32_t calculate_initial_addr(const struct connection_key *key) {
    if (!key) return 0;
    // 基于五元组计算哈希作为初始地址偏移
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
    return mem;
}

// 扩展线性内存区
int extend_linear_memory(struct linear_memory *mem) {
    if (!mem) return -1;

    // 找到最后一段内存
    while (mem->next) {
        mem = mem->next;
    }

    // 创建新的扩展内存
    struct linear_memory *new_mem = init_linear_memory(EXTEND_MEM_SIZE);
    if (!new_mem) return -1;

    mem->next = new_mem;
    printf("扩展内存成功，新增 %d 个块\n", EXTEND_MEM_SIZE);
    return 0;
}

// 插入数据包到缓冲区（O(1)时间复杂度）
int insert_to_buffer(struct connection_cache *cache, uint32_t psn, 
                    const unsigned char *data, int data_len) {
    if (!cache || !data || data_len <= 0 || data_len > MEM_BLOCK_SIZE) {
        return -1;
    }

    pthread_mutex_lock(&cache->buffer.lock);

    // 检查缓冲区是否已满
    if (cache->buffer.count >= BUFFER_CAPACITY) {
        pthread_mutex_unlock(&cache->buffer.lock);
        return -1;
    }

    // 计算插入位置（循环队列）
    int insert_pos = (cache->buffer.tail) % BUFFER_CAPACITY;
    struct cached_packet *pkt = &cache->buffer.packets[insert_pos];

    // 复制数据
    memcpy(pkt->data, data, data_len);
    pkt->data_len = data_len;
    pkt->psn = psn;
    pkt->valid = 1;

    // 更新缓冲区状态
    cache->buffer.tail = (cache->buffer.tail + 1) % BUFFER_CAPACITY;
    cache->buffer.count++;

    // 更新连接的PSN范围
    if (psn < cache->min_psn || cache->min_psn == 0) {
        cache->min_psn = psn;
    }
    if (psn > cache->max_psn) {
        cache->max_psn = psn;
    }

    pthread_mutex_unlock(&cache->buffer.lock);
    return 0;
}

// 刷新缓冲区到内存（地址映射机制）
int flush_buffer_to_memory(struct connection_cache *cache) {
    if (!cache || cache->buffer.count == 0) {
        return 0;
    }

    pthread_mutex_lock(&cache->buffer.lock);
    if (cache->buffer.count == 0) {
        pthread_mutex_unlock(&cache->buffer.lock);
        return 0;
    }

    // 1. 收集所有数据包并按PSN排序（保证内存中有序）
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

    // 2. 更新base_psn为当前最大PSN
    cache->buffer.base_psn = packets[count - 1].psn;

    // 3. 将排序后的数据包写入线性内存
    struct linear_memory *current_mem = cache->memory;
    int written = 0;

    while (written < count) {
        pthread_mutex_lock(&current_mem->lock);
        
        // 检查当前内存是否有足够空间
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

        // 如果当前内存已满且还有数据要写，扩展内存
        if (written < count && !current_mem->next) {
            if (extend_linear_memory(current_mem) != 0) {
                free(packets);
                pthread_mutex_unlock(&cache->buffer.lock);
                return -1;
            }
        }

        // 移动到下一段内存
        if (current_mem->next) {
            current_mem = current_mem->next;
        }
    }

    free(packets);

    // 4. 清空缓冲区
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
        usleep(1000);  // 1ms检查一次

        pthread_mutex_lock(&cache->buffer.lock);
        gettimeofday(&now, NULL);
        
        // 计算时间差(ms)
        diff.tv_sec = now.tv_sec - cache->buffer.last_flush.tv_sec;
        diff.tv_usec = now.tv_usec - cache->buffer.last_flush.tv_usec;
        long ms_diff = diff.tv_sec * 1000 + diff.tv_usec / 1000;

        // 检查是否需要刷新
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

    // 初始化缓冲区
    if (init_batch_buffer(&cache->buffer) != 0) {
        free(cache);
        return NULL;
    }

    // 初始化线性内存（基于五元组计算初始地址）
    cache->memory = init_linear_memory(INIT_MEM_SIZE);
    if (!cache->memory) {
        free(cache);
        return NULL;
    }

    // 创建定时刷新线程
    if (pthread_create(&cache->flush_thread, NULL, batch_flush_thread, cache) != 0) {
        destroy_connection_cache(cache);
        return NULL;
    }

    return cache;
}

// 销毁连接缓存
void destroy_connection_cache(struct connection_cache *cache) {
    if (!cache) return;

    // 停止运行标志
    cache->running = 0;

    // 销毁定时线程
    if (pthread_self() != cache->flush_thread && cache->flush_thread != 0) {
        pthread_cancel(cache->flush_thread);
        pthread_join(cache->flush_thread, NULL);
    }

    // 销毁线性内存
    struct linear_memory *current = cache->memory;
    while (current) {
        struct linear_memory *next = current->next;
        if (current->blocks) free(current->blocks);
        pthread_mutex_destroy(&current->lock);
        free(current);
        current = next;
    }

    // 销毁缓冲区锁
    pthread_mutex_destroy(&cache->buffer.lock);

    free(cache);
}

// 根据ePSN处理重传请求
int process_retransmit_request(struct connection_cache *cache, uint32_t ePSN,
                              struct cached_packet **retrans_pkts, size_t *count) {
    if (!cache || !retrans_pkts || !count) return -1;

    *count = 0;
    *retrans_pkts = NULL;

    // 收集所有需要重传的包（psn >= ePSN）
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

    // 分配重传包数组
    *retrans_pkts = malloc(sizeof(struct cached_packet) * (*count));
    if (!*retrans_pkts) return -1;

    // 填充重传包并清空旧包
    size_t idx = 0;
    current = cache->memory;
    while (current) {
        pthread_mutex_lock(&current->lock);
        
        for (size_t i = 0; i < current->used; i++) {
            if (current->blocks[i].valid) {
                if (current->blocks[i].psn >= ePSN) {
                    (*retrans_pkts)[idx++] = current->blocks[i];
                } else {
                    // 清空psn < ePSN的包
                    memset(&current->blocks[i], 0, sizeof(struct cached_packet));
                }
            }
        }
        
        pthread_mutex_unlock(&current->lock);
        current = current->next;
    }

    return 0;
}
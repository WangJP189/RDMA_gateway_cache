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

// RDMA报文缓存管理系统（线形缓存+多连接批量处理）
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <signal.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <unistd.h>

// #include "pkt_cache.h"

// 配置参数
#define MAX_CONNECTIONS 10        // 支持上百个连接
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
    struct timeval timestamp;            // 缓存时间戳
    int valid;                           // 有效性标记
};

// 连接标识键（五元组+QP信息）
struct connection_key {
    uint32_t src_ip;      // 源IP
    uint32_t dst_ip;      // 目的IP
    uint16_t src_port;    // 源端口
    uint16_t dst_port;    // 目的端口
    uint32_t src_qp;      // 源QP
    uint32_t dest_qp;     // 目的QP
};

// 批量处理缓冲区
struct batch_buffer {
    struct cached_packet packets[BUFFER_CAPACITY];  // 数据包缓冲区
    int head;                     // 头指针
    int tail;                     // 尾指针
    int count;                    // 当前数量
    struct timeval last_flush;    // 上次刷新时间
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
    struct timeval last_activity;  // 最后活动时间
    pthread_t flush_thread;        // 定时刷新线程
    int running;                   // 运行标志
    pid_t process_id;              // 进程ID
    int shm_id;                    // 共享内存ID
};

// 哈希表节点
struct hash_entry {
    struct connection_key key;     // 连接标识
    struct connection_cache *cache;// 缓存结构
    pid_t process_id;              // 进程ID
    int shm_id;                    // 共享内存ID
    struct hash_entry *next;       // 哈希冲突链表
};

// 缓存管理器
struct cache_manager {
    struct hash_entry **hash_table;// 哈希表
    size_t hash_table_size;        // 哈希表大小
    size_t total_connections;      // 总连接数
    pthread_mutex_t global_lock;   // 全局锁
};

// 全局缓存管理器
struct cache_manager *g_cache_mgr = NULL;

// 哈希计算函数
uint32_t calculate_hash(const struct connection_key *key, size_t table_size) {
    return (key->src_ip + key->dst_ip + key->src_port + key->dst_port + 
            key->src_qp + key->dest_qp) % table_size;
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

// 初始化批量缓冲区
int init_batch_buffer(struct batch_buffer *buffer) {
    memset(buffer, 0, sizeof(struct batch_buffer));
    buffer->head = 0;
    buffer->tail = 0;
    buffer->count = 0;
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

// 插入数据包到缓冲区
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
    gettimeofday(&pkt->timestamp, NULL);

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

    gettimeofday(&cache->last_activity, NULL);

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

    printf("刷新缓冲区: 共 %d 个数据包\n", cache->buffer.count);

    // 1. 收集所有数据包并按PSN排序
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

    // 简单冒泡排序（按PSN从小到大）
    for (int i = 0; i < count - 1; i++) {
        for (int j = 0; j < count - i - 1; j++) {
            if (packets[j].psn > packets[j + 1].psn) {
                struct cached_packet temp = packets[j];
                packets[j] = packets[j + 1];
                packets[j + 1] = temp;
            }
        }
    }

    // 2. 将排序后的数据包写入线性内存
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
            printf("写入 %zu 个数据包到内存，当前段已使用: %zu/%zu\n",
                   to_write, current_mem->used, current_mem->capacity);
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

    // 3. 清空缓冲区
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

    // 如果是父进程，清理共享内存
    if (getpid() != cache->process_id) {
        shmdt(cache);
        shmctl(cache->shm_id, IPC_RMID, NULL);
    }
}

// 创建连接缓存
struct connection_cache* create_connection_cache(const struct connection_key *key) {
    // 创建共享内存
    int shm_id = shmget(IPC_PRIVATE, sizeof(struct connection_cache), 0666 | IPC_CREAT);
    if (shm_id == -1) {
        perror("shmget failed");
        return NULL;
    }

    // 映射共享内存
    struct connection_cache *cache = (struct connection_cache*)shmat(shm_id, NULL, 0);
    if (cache == (void*)-1) {
        perror("shmat failed");
        shmctl(shm_id, IPC_RMID, NULL);
        return NULL;
    }

    // 初始化缓存结构
    memset(cache, 0, sizeof(struct connection_cache));
    cache->key = *key;
    cache->shm_id = shm_id;
    cache->running = 1;
    cache->min_psn = 0;
    cache->max_psn = 0;
    gettimeofday(&cache->last_activity, NULL);

    // 初始化缓冲区
    if (init_batch_buffer(&cache->buffer) != 0) {
        shmdt(cache);
        shmctl(shm_id, IPC_RMID, NULL);
        return NULL;
    }

    // 初始化线性内存
    cache->memory = init_linear_memory(INIT_MEM_SIZE);
    if (!cache->memory) {
        shmdt(cache);
        shmctl(shm_id, IPC_RMID, NULL);
        return NULL;
    }

    // 创建子进程
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork failed");
        destroy_connection_cache(cache);
        return NULL;
    } else if (pid == 0) {
        // 子进程：启动定时刷新线程
        if (pthread_create(&cache->flush_thread, NULL, batch_flush_thread, cache) != 0) {
            perror("pthread_create failed");
            exit(EXIT_FAILURE);
        }
        cache->process_id = getpid();
        printf("连接进程启动，PID: %d\n", getpid());
        
        // 子进程主循环
        while (cache->running) {
            pause();  // 等待信号
        }
        
        // 清理子进程资源
        pthread_join(cache->flush_thread, NULL);
        destroy_connection_cache(cache);
        exit(EXIT_SUCCESS);
    }

    cache->process_id = pid;
    shmdt(cache);
    return cache;
}


// 获取或创建连接缓存
struct hash_entry* get_or_create_hash_entry(struct cache_manager *mgr, 
                                           const struct connection_key *key) {
    if (!mgr || !key) return NULL;

    uint32_t hash_index = calculate_hash(key, mgr->hash_table_size);
    
    // 查找现有连接
    struct hash_entry *entry = mgr->hash_table[hash_index];
    while (entry) {
        if (connection_keys_equal(&entry->key, key)) {
            gettimeofday(&entry->cache->last_activity, NULL);
            return entry;
        }
        entry = entry->next;
    }
    
    // 检查连接数限制
    if (mgr->total_connections >= MAX_CONNECTIONS) {
        printf("达到最大连接数限制 (%zu/%d)\n", mgr->total_connections, MAX_CONNECTIONS);
        return NULL;
    }
    
    // 创建新连接缓存
    struct connection_cache *cache = create_connection_cache(key);
    if (!cache) return NULL;
    
    // 创建哈希表项
    struct hash_entry *new_entry = malloc(sizeof(struct hash_entry));
    if (!new_entry) {
        kill(cache->process_id, SIGTERM);
        waitpid(cache->process_id, NULL, 0);
        destroy_connection_cache(cache);
        return NULL;
    }
    
    new_entry->key = *key;
    new_entry->cache = cache;
    new_entry->process_id = cache->process_id;
    new_entry->shm_id = cache->shm_id;
    new_entry->next = mgr->hash_table[hash_index];
    mgr->hash_table[hash_index] = new_entry;
    mgr->total_connections++;
    
    // 打印连接信息
    struct in_addr src_in_addr, dst_in_addr;
    src_in_addr.s_addr = key->src_ip;
    dst_in_addr.s_addr = key->dst_ip;
    
    printf("创建新连接: %s:%u -> %s:%u (QP%u->QP%u)，进程ID: %d\n",
           inet_ntoa(src_in_addr), key->src_port,
           inet_ntoa(dst_in_addr), key->dst_port,
           key->src_qp, key->dest_qp,
           new_entry->process_id);
    
    return new_entry;
}

// 添加数据包到缓存系统
int add_packet_to_cache(const char *src_ip, const char *dst_ip,
                       uint16_t src_port, uint16_t dst_port,
                       uint32_t src_qp, uint32_t dest_qp,
                       uint32_t psn, const unsigned char *data, int data_len) {
    if (!g_cache_mgr || !src_ip || !dst_ip || !data || data_len <= 0) {
        return -1;
    }
    
    struct connection_key key = create_connection_key(src_ip, dst_ip, src_port, dst_port, src_qp, dest_qp);
    
    pthread_mutex_lock(&g_cache_mgr->global_lock);
    
    struct hash_entry *entry = get_or_create_hash_entry(g_cache_mgr, &key);
    if (!entry) {
        pthread_mutex_unlock(&g_cache_mgr->global_lock);
        return -1;
    }
    
    // 映射共享内存
    struct connection_cache *cache = (struct connection_cache*)shmat(entry->shm_id, NULL, 0);
    if (cache == (void*)-1) {
        perror("shmat failed in add_packet_to_cache");
        pthread_mutex_unlock(&g_cache_mgr->global_lock);
        return -1;
    }
    
    // 插入数据包到缓冲区
    int ret = insert_to_buffer(cache, psn, data, data_len);
    shmdt(cache);
    
    pthread_mutex_unlock(&g_cache_mgr->global_lock);
    return ret;
}

// 打印连接状态
void print_connection_status(struct connection_cache *cache) {
    if (!cache) return;

    struct in_addr src_in_addr, dst_in_addr;
    src_in_addr.s_addr = cache->key.src_ip;
    dst_in_addr.s_addr = cache->key.dst_ip;
    
    printf("连接: %s:%u -> %s:%u (QP%u->QP%u)\n",
           inet_ntoa(src_in_addr), cache->key.src_port,
           inet_ntoa(dst_in_addr), cache->key.dst_port,
           cache->key.src_qp, cache->key.dest_qp);
    
    printf("  进程ID: %d\n", cache->process_id);
    printf("  PSN范围: %u-%u\n", cache->min_psn, cache->max_psn);
    printf("  缓冲区状态: %d/%d 个包\n", cache->buffer.count, BUFFER_CAPACITY);
    
    // 计算总内存使用量
    size_t total_used = 0;
    size_t total_capacity = 0;
    struct linear_memory *current = cache->memory;
    while (current) {
        total_used += current->used;
        total_capacity += current->capacity;
        current = current->next;
    }
    printf("  内存使用: %zu/%zu 个块\n", total_used, total_capacity);
}

// 打印所有连接状态
void print_all_connections_status() {
    if (!g_cache_mgr) {
        printf("缓存管理器未初始化\n");
        return;
    }
    
    printf("\n===== 连接状态汇总 =====\n");
    printf("总连接数: %zu/%d\n", g_cache_mgr->total_connections, MAX_CONNECTIONS);
    
    pthread_mutex_lock(&g_cache_mgr->global_lock);
    
    for (size_t i = 0; i < g_cache_mgr->hash_table_size; i++) {
        struct hash_entry *entry = g_cache_mgr->hash_table[i];
        while (entry) {
            // 映射共享内存
            struct connection_cache *cache = (struct connection_cache*)shmat(entry->shm_id, NULL, 0);
            if (cache != (void*)-1) {
                print_connection_status(cache);
                shmdt(cache);
            }
            entry = entry->next;
        }
    }
    
    pthread_mutex_unlock(&g_cache_mgr->global_lock);
    printf("=======================\n");
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
    
    printf("初始化缓存管理器: 哈希表大小=%zu, 最大连接数=%d\n", 
           hash_size, MAX_CONNECTIONS);
    return mgr;
}

// 销毁缓存管理器
void destroy_cache_manager(struct cache_manager *mgr) {
    if (!mgr) return;
    
    for (size_t i = 0; i < mgr->hash_table_size; i++) {
        struct hash_entry *entry = mgr->hash_table[i];
        while (entry) {
            struct hash_entry *next = entry->next;
            
            // 终止子进程
            if (entry->process_id > 0) {
                kill(entry->process_id, SIGTERM);
                waitpid(entry->process_id, NULL, 0);
            }
            
            // 销毁缓存
            if (entry->cache) {
                destroy_connection_cache(entry->cache);
            }
            
            free(entry);
            entry = next;
        }
    }
    
    free(mgr->hash_table);
    pthread_mutex_destroy(&mgr->global_lock);
    free(mgr);
}

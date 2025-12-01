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

//w RDMA报文缓存管理系统
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

// 配置参数（优化批量处理阈值）
#define MAX_CONNECTIONS 5            // 最大连接数
#define RING_BUFFER_SIZE 1000        // 增大环形缓冲区
#define MAX_PACKET_SIZE 4096         // 最大包大小
#define BATCH_TIMEOUT_MS 10          // 缩短批量处理超时(ms)
#define BATCH_THRESHOLD 500          // 增大批量阈值适配高速发包

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
    size_t count;  // 添加计数字段
};

// 哈希表节点
struct hash_entry {
    struct connection_key key;
    pid_t process_id;                // 对应处理进程ID
    int msg_queue_id;                // 消息队列共享内存ID
    int shm_id;                      // 缓存共享内存ID
    struct connection_cache *cache;  // 共享内存地址
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

// 消息队列结构（用于进程间通信）
struct packet_msg {
    struct connection_key key;
    uint32_t psn;
    unsigned char data[MAX_PACKET_SIZE];
    int data_len;
    int processed;  // 0=未处理(空闲), 1=已处理(待清理)
};

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
    cache->window_size = 512;  // 大幅增大窗口
    cache->count = 0;  // 初始化为0
    gettimeofday(&cache->last_activity, NULL);
    
    if (pthread_mutex_init(&cache->lock, NULL) != 0) {
        free(cache);
        return NULL;
    }
    
    return cache;
}

// 计算PSN在环形缓冲区中的位置
static inline size_t psn_to_index(struct connection_cache *cache, uint32_t psn) {
    // 处理PSN回绕的情况
    if (psn < cache->base_psn) {
        uint32_t diff = cache->base_psn - psn;
        return RING_BUFFER_SIZE - (diff % RING_BUFFER_SIZE);
    }
    return (psn - cache->base_psn) % RING_BUFFER_SIZE;
}

// 滑动窗口调整函数
static void adjust_window(struct connection_cache *cache, uint32_t psn) {
    // 如果这是第一个数据包，设置窗口
    if (cache->count == 0) {
        cache->window_start = psn;
        cache->window_size = 512;
        return;
    }
    
    // 如果PSN在窗口内，不做调整
    if (psn >= cache->window_start && psn < cache->window_start + cache->window_size) {
        return;
    }
    
    // 如果PSN在窗口之前，可能需要扩展窗口
    if (psn < cache->window_start) {
        // 检查是否需要扩展窗口到更早的PSN
        if (cache->window_start - psn < 100) {  // 允许稍微扩展
            cache->window_start = psn;
        }
        return;
    }
    
    // 如果PSN在窗口之后，滑动窗口
    if (psn >= cache->window_start + cache->window_size) {
        // 滑动窗口，但保留一些历史数据
        uint32_t new_start = psn - cache->window_size / 2;
        if (new_start < cache->min_psn) {
            new_start = cache->min_psn;
        }
        
        printf("窗口滑动: [%u, %u] -> [%u, %u] (触发PSN=%u)\n",
               cache->window_start, cache->window_start + cache->window_size - 1,
               new_start, new_start + cache->window_size - 1,
               psn);
        
        cache->window_start = new_start;
    }
}

// 插入数据包到缓存
int insert_packet(struct connection_cache *cache, uint32_t psn, 
                 const unsigned char *data, int data_len) {
    if (!cache || !data || data_len <= 0 || data_len > MAX_PACKET_SIZE) {
        return -1;
    }
    
    pthread_mutex_lock(&cache->lock);
    
    // 调试输出
    printf("插入: PSN=%u, 当前窗口=[%u, %u], base_psn=%u\n", 
           psn, cache->window_start, 
           cache->window_start + cache->window_size - 1,
           cache->base_psn);
    
    // 动态调整窗口，但不拒绝窗口外的数据包
    adjust_window(cache, psn);
    
    // 计算位置
    size_t index = psn_to_index(cache, psn);
    
    // 检查是否在环形缓冲区范围内
    if (index >= RING_BUFFER_SIZE) {
        printf("索引计算错误: PSN=%u -> 索引=%zu\n", psn, index);
        pthread_mutex_unlock(&cache->lock);
        return -1;
    }
    
    // 检查该位置是否已有数据
    struct cached_packet *packet = &cache->ring[index];
    if (packet->valid) {
        printf("警告: 索引%zu已有数据(PSN=%u)，覆盖为PSN=%u\n", 
               index, packet->psn, psn);
        cache->total_bytes -= packet->data_len;
    }
    
    // 插入新数据
    memcpy(packet->data, data, data_len);
    packet->data_len = data_len;
    packet->psn = psn;
    packet->valid = 1;
    gettimeofday(&packet->timestamp, NULL);
    
    // 更新统计信息
    cache->total_bytes += data_len;
    if (cache->count == 0 || psn < cache->min_psn) cache->min_psn = psn;
    if (psn > cache->max_psn) cache->max_psn = psn;
    cache->count++;
    gettimeofday(&cache->last_activity, NULL);
    
    pthread_mutex_unlock(&cache->lock);
    return 0;
}

// 查找数据包（重传请求处理）
struct cached_packet* find_packet(struct connection_cache *cache, uint32_t psn) {
    if (!cache) {
        printf("查找失败: cache为空\n");
        return NULL;
    }
    
    pthread_mutex_lock(&cache->lock);
    
    printf("查找PSN=%u: 窗口范围[%u, %u], min_psn=%u, max_psn=%u\n", 
           psn, cache->window_start, 
           cache->window_start + cache->window_size - 1,
           cache->min_psn, cache->max_psn);
    
    // 先检查PSN是否在合理范围内
    if (cache->count == 0) {
        printf("查找失败: 缓存为空\n");
        pthread_mutex_unlock(&cache->lock);
        return NULL;
    }
    
    if (psn < cache->min_psn || psn > cache->max_psn) {
        printf("查找失败: PSN=%u不在范围[%u, %u]内\n", 
               psn, cache->min_psn, cache->max_psn);
        pthread_mutex_unlock(&cache->lock);
        return NULL;
    }
    
    // 计算索引
    size_t index = psn_to_index(cache, psn);
    struct cached_packet *packet = &cache->ring[index];
    
    // 检查数据是否有效
    if (!packet->valid) {
        printf("查找失败: 索引%zu的数据无效\n", index);
        pthread_mutex_unlock(&cache->lock);
        return NULL;
    }
    
    if (packet->psn != psn) {
        printf("查找失败: 索引%zu的PSN=%u不等于目标PSN=%u\n", 
               index, packet->psn, psn);
        pthread_mutex_unlock(&cache->lock);
        return NULL;
    }
    
    // 创建数据副本
    struct cached_packet *result = malloc(sizeof(struct cached_packet));
    if (result) {
        memcpy(result, packet, sizeof(struct cached_packet));
        result->data_len = packet->data_len;
        memcpy(result->data, packet->data, packet->data_len);
        printf("查找成功: 找到PSN=%u, 长度=%d\n", psn, packet->data_len);
    }
    
    pthread_mutex_unlock(&cache->lock);
    return result;
}

// 连接处理进程,实现真正的批量处理
static void connection_process(struct connection_cache *cache, int msg_queue_id) {
    struct packet_msg *msg_queue = (struct packet_msg*)shmat(msg_queue_id, NULL, 0);
    if (msg_queue == (void*)-1) {
        perror("shmat failed");
        exit(EXIT_FAILURE);
    }

    // 本地批量缓冲区（保存待批量处理的数据包）
    struct {
        uint32_t psn;
        unsigned char data[MAX_PACKET_SIZE];
        int data_len;
    } batch_buffer[50];  // 批量大小设为50
    
    int batch_count = 0;  // 当前批量中的数据包数量
    struct timeval batch_start_time;
    int batch_processing = 0;  // 是否正在批量处理
    
    gettimeofday(&batch_start_time, NULL);

    while (1) {
        int msg_processed = 0;
        
        // 扫描消息队列，收集未处理的消息到批量缓冲区
        for (int i = 0; i < BATCH_THRESHOLD && batch_count < 50; i++) {
            if (msg_queue[i].processed == 0 && msg_queue[i].data_len > 0) {
                // 复制到批量缓冲区
                batch_buffer[batch_count].psn = msg_queue[i].psn;
                batch_buffer[batch_count].data_len = msg_queue[i].data_len;
                memcpy(batch_buffer[batch_count].data, msg_queue[i].data, msg_queue[i].data_len);
                
                // 标记消息为已收集（但还未真正处理）
                msg_queue[i].processed = 1;
                batch_count++;
                msg_processed = 1;
                
                if (!batch_processing) {
                    batch_processing = 1;
                    gettimeofday(&batch_start_time, NULL);
                }
            }
        }
        
        // 检查是否达到批量处理条件：数量达到50或超时10ms
        struct timeval now;
        gettimeofday(&now, NULL);
        long elapsed_ms = (now.tv_sec - batch_start_time.tv_sec) * 1000 +
                         (now.tv_usec - batch_start_time.tv_usec) / 1000;
        
        if (batch_processing && (batch_count >= 50 || elapsed_ms >= 10)) {
            // 批量插入所有数据包
            for (int i = 0; i < batch_count; i++) {
                int ret = insert_packet(cache, batch_buffer[i].psn, 
                                       batch_buffer[i].data, batch_buffer[i].data_len);
                if (ret == 0) {
                    printf("子进程 %d: 批量插入 PSN=%u, 长度=%d\n", 
                           getpid(), batch_buffer[i].psn, batch_buffer[i].data_len);
                }
            }
            
            printf("子进程 %d: 完成批量处理，共 %d 个数据包，耗时 %ld ms\n", 
                   getpid(), batch_count, elapsed_ms);
            
            // 重置批量处理状态
            batch_count = 0;
            batch_processing = 0;
        }
        
        // 清理已处理的消息
        for (int i = 0; i < BATCH_THRESHOLD; i++) {
            if (msg_queue[i].processed == 1) {
                // 可以重新使用这个位置
                memset(&msg_queue[i], 0, sizeof(struct packet_msg));
            }
        }
        
        // 如果队列为空，休眠更长时间
        if (!msg_processed) {
            usleep(1000);  // 1ms
        } else {
            usleep(100);   // 更短的休眠，以便快速处理
        }
    }

    shmdt(msg_queue);
    exit(EXIT_SUCCESS);
}

// 修复：查找空闲消息队列位置（processed=0）
static int get_free_msg_slot(struct packet_msg *msg_queue) {
    for (int i = 0; i < BATCH_THRESHOLD; i++) {
        if (msg_queue[i].processed == 0) {  // 关键修复：找未处理的空闲位置
            return i;
        }
    }
    return -1; // 队列满
}

// 获取或创建连接缓存
struct hash_entry* get_or_create_hash_entry(struct cache_manager *mgr, 
                                           const struct connection_key *key,
                                           uint32_t first_psn) {
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
    
    if (mgr->total_connections >= MAX_CONNECTIONS) {
        printf("达到最大连接数限制 (%zu/%d)\n", mgr->total_connections, MAX_CONNECTIONS);
        return NULL;
    }
    
    // 创建消息队列共享内存
    int msg_queue_id = shmget(IPC_PRIVATE, sizeof(struct packet_msg) * BATCH_THRESHOLD, 0666 | IPC_CREAT);
    if (msg_queue_id == -1) {
        perror("shmget failed for msg queue");
        return NULL;
    }

    // 初始化消息队列：所有位置设为空闲(processed=0)
    struct packet_msg *msg_queue = (struct packet_msg*)shmat(msg_queue_id, NULL, 0);
    if (msg_queue == (void*)-1) {
        perror("shmat failed for msg queue");
        shmctl(msg_queue_id, IPC_RMID, NULL);
        return NULL;
    }
    memset(msg_queue, 0, sizeof(struct packet_msg) * BATCH_THRESHOLD); // 关键修复：初始化所有字段为0
    shmdt(msg_queue);

    // 创建缓存共享内存
    int shm_id = shmget(IPC_PRIVATE, sizeof(struct connection_cache), 0666 | IPC_CREAT);
    if (shm_id == -1) {
        perror("shmget failed for cache");
        shmctl(msg_queue_id, IPC_RMID, NULL);
        return NULL;
    }

    struct connection_cache *new_cache = (struct connection_cache*)shmat(shm_id, NULL, 0);
    if (new_cache == (void*)-1) {
        perror("shmat failed for cache");
        shmctl(shm_id, IPC_RMID, NULL);
        shmctl(msg_queue_id, IPC_RMID, NULL);
        return NULL;
    }

    // 初始化缓存
    memset(new_cache, 0, sizeof(struct connection_cache));
    new_cache->base_psn = first_psn;
    new_cache->min_psn = first_psn;
    new_cache->max_psn = first_psn;
    new_cache->window_start = first_psn;
    new_cache->window_size = 512;
    gettimeofday(&new_cache->last_activity, NULL);
    pthread_mutex_init(&new_cache->lock, NULL);

    // 创建子进程
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork failed");
        pthread_mutex_destroy(&new_cache->lock);
        shmdt(new_cache);
        shmctl(shm_id, IPC_RMID, NULL);
        shmctl(msg_queue_id, IPC_RMID, NULL);
        return NULL;
    } else if (pid == 0) {
        connection_process(new_cache, msg_queue_id);
        exit(EXIT_SUCCESS);
    }

    shmdt(new_cache);

    // 创建哈希表项
    struct hash_entry *new_entry = malloc(sizeof(struct hash_entry));
    if (!new_entry) {
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        shmctl(shm_id, IPC_RMID, NULL);
        shmctl(msg_queue_id, IPC_RMID, NULL);
        return NULL;
    }
    
    new_entry->key = *key;
    new_entry->process_id = pid;
    new_entry->msg_queue_id = msg_queue_id;
    new_entry->shm_id = shm_id;
    new_entry->cache = (struct connection_cache*)shmat(shm_id, NULL, 0);
    new_entry->next = mgr->hash_table[hash_index];
    mgr->hash_table[hash_index] = new_entry;
    mgr->total_connections++;
    
    // 正确打印IP地址
    struct in_addr src_in_addr, dst_in_addr;
    src_in_addr.s_addr = key->src_ip;
    dst_in_addr.s_addr = key->dst_ip;
    
    printf("创建新连接缓存(进程ID: %d): %s:%u -> %s:%u (QP%u->QP%u)\n",
           pid,
           inet_ntoa(src_in_addr), key->src_port,
           inet_ntoa(dst_in_addr), key->dst_port,
           key->src_qp, key->dest_qp);
    
    return new_entry;
}

// 批量插入数据包 - 最终修复版
int add_to_batch_queue(const char *src_ip, const char *dst_ip,
                      uint16_t src_port, uint16_t dst_port,
                      uint32_t src_qp, uint32_t dest_qp,
                      uint32_t psn, const unsigned char *app_data, int data_len) {
    if (!g_cache_mgr || !app_data || data_len <= 0 || data_len > MAX_PACKET_SIZE) {
        return -1;
    }
    
    struct connection_key key = create_connection_key(src_ip, dst_ip, src_port, dst_port, src_qp, dest_qp);
    
    pthread_mutex_lock(&g_cache_mgr->global_lock);
    
    struct hash_entry *entry = get_or_create_hash_entry(g_cache_mgr, &key, psn);
    if (!entry) {
        pthread_mutex_unlock(&g_cache_mgr->global_lock);
        return -1;
    }
    
    // 映射消息队列
    struct packet_msg *msg_queue = (struct packet_msg*)shmat(entry->msg_queue_id, NULL, 0);
    if (msg_queue == (void*)-1) {
        perror("shmat failed in add_to_batch_queue");
        pthread_mutex_unlock(&g_cache_mgr->global_lock);
        return -1;
    }
    
    // 查找空闲位置
    int slot = get_free_msg_slot(msg_queue);
    if (slot == -1) {
        printf("警告: 消息队列已满，PSN=%u等待重试...\n", psn);
        shmdt(msg_queue);
        pthread_mutex_unlock(&g_cache_mgr->global_lock);
        // 短暂重试：避免直接失败
        usleep(100);
        return add_to_batch_queue(src_ip, dst_ip, src_port, dst_port, 
                                 src_qp, dest_qp, psn, app_data, data_len);
    }
    
    // 添加到消息队列
    msg_queue[slot].key = key;
    msg_queue[slot].psn = psn;
    memcpy(msg_queue[slot].data, app_data, data_len);
    msg_queue[slot].data_len = data_len;
    msg_queue[slot].processed = 0;  // 明确标记为未处理
    
    shmdt(msg_queue);
    pthread_mutex_unlock(&g_cache_mgr->global_lock);
    return 0;
}

// 根据PSN查找数据包
struct cached_packet* find_packet_by_psn(const char *src_ip, const char *dst_ip,
                                        uint16_t src_port, uint16_t dst_port,
                                        uint32_t src_qp, uint32_t dest_qp,
                                        uint32_t psn) {
    if (!g_cache_mgr) return NULL;
    
    struct connection_key key = create_connection_key(src_ip, dst_ip, src_port, dst_port, src_qp, dest_qp);
    uint32_t hash_index = calculate_hash(&key, g_cache_mgr->hash_table_size);
    
    pthread_mutex_lock(&g_cache_mgr->global_lock);
    
    struct hash_entry *entry = g_cache_mgr->hash_table[hash_index];
    while (entry) {
        if (connection_keys_equal(&entry->key, &key)) {
            pthread_mutex_unlock(&g_cache_mgr->global_lock);
            return find_packet(entry->cache, psn);
        }
        entry = entry->next;
    }
    
    pthread_mutex_unlock(&g_cache_mgr->global_lock);
    return NULL;
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
            
            if (entry->process_id > 0) {
                kill(entry->process_id, SIGTERM);
                waitpid(entry->process_id, NULL, 0);
            }
            
            if (entry->cache) {
                pthread_mutex_destroy(&entry->cache->lock);
                shmdt(entry->cache);
            }
            if (entry->shm_id != 0) {
                shmctl(entry->shm_id, IPC_RMID, NULL);
            }
            if (entry->msg_queue_id != 0) {
                shmctl(entry->msg_queue_id, IPC_RMID, NULL);
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
            
            struct in_addr src_in_addr, dst_in_addr;
            src_in_addr.s_addr = key->src_ip;
            dst_in_addr.s_addr = key->dst_ip;
            
            printf("连接(进程ID: %d): %s:%u -> %s:%u (QP%u->QP%u)\n",
                   entry->process_id,
                   inet_ntoa(src_in_addr), key->src_port,
                   inet_ntoa(dst_in_addr), key->dst_port,
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

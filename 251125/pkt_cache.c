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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>   // 提供 fork() 声明
#include <sys/wait.h> // 提供 waitpid() 声明

// 缓存报文结构
struct cached_packet {
    unsigned char *app_data;      // 应用数据载荷
    int data_len;                 // 数据长度
    uint32_t dest_qp;             // 目标QP号
    uint32_t psn;                 // 包序列号
    struct timeval timestamp;     // 缓存时间戳
    int valid;                    // 数据有效性标记
};

// 连接标识键（五元组+QP信息）
struct connection_key {
    uint32_t src_ip;              // 源IP
    uint32_t dst_ip;              // 目的IP
    uint16_t src_port;            // 源端口
    uint16_t dst_port;            // 目的端口
    uint32_t dest_qp;             // 目标主机QP号
    uint32_t src_qp;              // 源主机QP号
    uint8_t service_type;         // RDMA服务类型
    uint16_t pkey;                // 分区键
};

// 单个连接的环形缓存
struct connection_ring_cache {
    struct cached_packet *ring;   // 环形缓冲区
    size_t ring_size;             // 环形缓冲区大小
    uint32_t base_psn;            // 基准PSN，用于计算偏移
    size_t total_bytes;           // 总字节数
    struct timeval last_activity; // 最后活动时间
    pthread_mutex_t lock;         // 连接级锁

    // 滑动窗口字段
    uint32_t next_expected_psn;   // 期望接收的下一个PSN
    uint32_t window_size;         // 窗口大小
    uint32_t highest_ack_psn;     // 已确认的最高PSN
};

// 哈希表节点
struct hash_table_entry {
    struct connection_key key;    // 连接标识
    struct connection_ring_cache *cache; // 对应的环形缓存
    struct hash_table_entry *next;// 哈希冲突链表
    pid_t process_id;             // 连接对应的进程ID
};

// 批量缓存临时节点
struct batch_node {
    struct connection_key key;    // 连接键
    uint32_t psn;                 // 包序列号
    unsigned char *app_data;      // 应用数据
    int data_len;                 // 数据长度
    uint32_t dest_qp;             // 目标QP
    struct batch_node *next;      // 链表节点
};

// 批量处理队列
struct batch_queue {
    struct batch_node *head;      // 队列头
    struct batch_node *tail;      // 队列尾
    size_t count;                 // 队列大小
    pthread_mutex_t lock;         // 队列锁
    pthread_cond_t cond;          // 条件变量
};

// 缓存管理器
struct cache_manager {
    struct hash_table_entry **hash_table; // 哈希表
    size_t hash_table_size;       // 哈希表大小
    size_t max_connections;       // 最大连接数（限制为10）
    size_t ring_cache_size;       // 每个连接的环形缓存大小
    size_t max_bytes_per_conn;    // 每连接最大字节数（100MB）
    int connection_timeout;       // 连接超时时间（秒）
    pthread_mutex_t global_lock;  // 全局锁
    size_t total_connections;     // 总连接数

    // 批量处理相关
    struct batch_queue batch_q;   // 批量处理队列
    pthread_t *batch_threads;     // 批量处理线程数组
    int batch_running;            // 批量线程运行标志
    size_t batch_threshold;       // 批量处理阈值
    int batch_timeout;            // 批量处理超时时间（毫秒）
    size_t num_batch_threads;     // 批量线程数量

    // 滑动窗口配置
    uint32_t default_window_size; // 默认窗口大小
};

// 全局缓存管理器
struct cache_manager *g_cache_mgr = NULL;

// 计算PSN在环形缓存中的偏移量（O(1)查找核心）
static inline size_t psn_to_offset(struct connection_ring_cache *cache, uint32_t psn) {
    if (psn >= cache->base_psn) {
        return (psn - cache->base_psn) % cache->ring_size;
    } else {
        return (psn + (0xFFFFFFFF - cache->base_psn) + 1) % cache->ring_size;
    }
}

// 哈希计算函数（O(1)定位核心）
uint32_t calculate_hash(const struct connection_key *key, size_t table_size) {
    uint32_t hash = 5381;
    hash = ((hash << 5) + hash) + key->src_ip;
    hash = ((hash << 5) + hash) + key->dst_ip;
    hash = ((hash << 5) + hash) + key->src_port;
    hash = ((hash << 5) + hash) + key->dst_port;
    hash = ((hash << 5) + hash) + key->src_qp;
    hash = ((hash << 5) + hash) + key->dest_qp;
    hash = ((hash << 5) + hash) + key->service_type;
    hash = ((hash << 5) + hash) + key->pkey;
    return hash % table_size;
}

// 连接键比较
int connection_keys_equal(const struct connection_key *a, const struct connection_key *b) {
    return (a->src_ip == b->src_ip &&
            a->dst_ip == b->dst_ip &&
            a->src_port == b->src_port &&
            a->dst_port == b->dst_port &&
            a->src_qp == b->src_qp &&
            a->dest_qp == b->dest_qp &&
            a->service_type == b->service_type &&
            a->pkey == b->pkey);
}

// 创建连接键
struct connection_key create_connection_key(const char *src_ip, const char *dst_ip,
                                            uint16_t src_port, uint16_t dst_port,
                                            uint32_t src_qp, uint32_t dest_qp,
                                            uint8_t service_type, uint16_t pkey) {
    struct connection_key key;
    memset(&key, 0, sizeof(key));
    inet_pton(AF_INET, src_ip, &key.src_ip);
    inet_pton(AF_INET, dst_ip, &key.dst_ip);
    key.src_port = src_port;
    key.dst_port = dst_port;
    key.src_qp = src_qp;
    key.dest_qp = dest_qp;
    key.service_type = service_type;
    key.pkey = pkey;
    return key;
}

// 创建连接的环形缓存
struct connection_ring_cache* create_ring_cache(size_t ring_size, uint32_t first_psn, uint32_t window_size) {
    struct connection_ring_cache *cache = malloc(sizeof(struct connection_ring_cache));
    if (!cache) {
        perror("malloc connection_ring_cache failed");
        return NULL;
    }

    // 预分配环形缓冲区（初期允许内存浪费）
    cache->ring = calloc(ring_size, sizeof(struct cached_packet));
    if (!cache->ring) {
        perror("calloc ring buffer failed");
        free(cache);
        return NULL;
    }

    cache->ring_size = ring_size;
    cache->base_psn = first_psn;
    cache->total_bytes = 0;
    gettimeofday(&cache->last_activity, NULL);

    // 滑动窗口初始化
    cache->next_expected_psn = first_psn;
    cache->window_size = window_size;
    cache->highest_ack_psn = first_psn - 1;

    if (pthread_mutex_init(&cache->lock, NULL) != 0) {
        perror("pthread_mutex_init ring cache lock failed");
        free(cache->ring);
        free(cache);
        return NULL;
    }

    return cache;
}

// 销毁连接缓存
void destroy_ring_cache(struct connection_ring_cache *cache) {
    if (!cache) return;

    pthread_mutex_lock(&cache->lock);
    for (size_t i = 0; i < cache->ring_size; i++) {
        if (cache->ring[i].valid) {
            free(cache->ring[i].app_data);
        }
    }
    free(cache->ring);
    pthread_mutex_unlock(&cache->lock);
    pthread_mutex_destroy(&cache->lock);
    free(cache);
}

// 滑动窗口：释放已确认的数据包
static void slide_window(struct connection_ring_cache *cache, uint32_t new_ack_psn) {
    if (new_ack_psn <= cache->highest_ack_psn) return;

    for (uint32_t psn = cache->highest_ack_psn + 1; psn <= new_ack_psn; psn++) {
        size_t offset = psn_to_offset(cache, psn);
        struct cached_packet *packet = &cache->ring[offset];
        
        if (packet->valid && packet->psn == psn) {
            free(packet->app_data);
            packet->app_data = NULL;
            packet->valid = 0;
            cache->total_bytes -= packet->data_len;
        }
    }

    cache->highest_ack_psn = new_ack_psn;
    cache->next_expected_psn = new_ack_psn + 1;
}

// 将数据包插入环形缓存（O(1)插入）
int insert_packet_to_ring(struct connection_ring_cache *cache, 
                         uint32_t psn, const unsigned char *app_data, 
                         int data_len, uint32_t dest_qp) {
    if (!cache || !app_data || data_len <= 0)
        return -1;
    
    pthread_mutex_lock(&cache->lock);

    // 计算偏移量（直接定位存储位置）
    size_t offset = psn_to_offset(cache, psn);
    struct cached_packet *packet = &cache->ring[offset];
    
    // 释放旧数据
    if (packet->valid) {
        cache->total_bytes -= packet->data_len;
        free(packet->app_data);
    }

    // 分配并复制新数据
    packet->app_data = malloc(data_len);
    if (!packet->app_data) {
        perror("malloc app_data failed");
        pthread_mutex_unlock(&cache->lock);
        return -1;
    }
    memcpy(packet->app_data, app_data, data_len);
    packet->data_len = data_len;
    packet->dest_qp = dest_qp;
    packet->psn = psn;
    gettimeofday(&packet->timestamp, NULL);
    packet->valid = 1;
    cache->total_bytes += data_len;

    // 滑动窗口（若为期望的下一个PSN）
    if (psn == cache->next_expected_psn) {
        slide_window(cache, psn);
    }

    gettimeofday(&cache->last_activity, NULL);
    pthread_mutex_unlock(&cache->lock);
    return 0;
}

// 获取或创建连接缓存（每个连接对应独立进程）
struct connection_ring_cache* get_or_create_ring_cache(
    struct cache_manager *mgr, const struct connection_key *key,
    uint32_t first_psn, pid_t *pid) {
    uint32_t hash_index = calculate_hash(key, mgr->hash_table_size);
    pthread_mutex_lock(&mgr->global_lock);

    // 查找现有连接
    struct hash_table_entry *entry = mgr->hash_table[hash_index];
    while (entry) {
        if (connection_keys_equal(&entry->key, key)) {
            gettimeofday(&entry->cache->last_activity, NULL);
            *pid = entry->process_id;
            pthread_mutex_unlock(&mgr->global_lock);
            return entry->cache;
        }
        entry = entry->next;
    }

    // 检查连接数限制（最多10个）
    if (mgr->total_connections >= mgr->max_connections) {
        fprintf(stderr, "达到最大连接数限制 (%zu/%zu)\n",
               mgr->total_connections, mgr->max_connections);
        pthread_mutex_unlock(&mgr->global_lock);
        return NULL;
    }

    // 创建新连接缓存
    struct connection_ring_cache *new_cache = create_ring_cache(
        mgr->ring_cache_size, first_psn, mgr->default_window_size);
    if (!new_cache) {
        pthread_mutex_unlock(&mgr->global_lock);
        return NULL;
    }

    // 创建子进程（每个连接对应一个进程）
    pid_t child_pid = fork();
    if (child_pid == -1) {
        perror("fork failed");
        destroy_ring_cache(new_cache);
        pthread_mutex_unlock(&mgr->global_lock);
        return NULL;
    }
    if (child_pid == 0) { // 子进程：负责当前连接的数据包处理
        pthread_mutex_unlock(&mgr->global_lock);
        return new_cache; // 子进程返回缓存指针，主进程继续
    }

    // 主进程：更新哈希表
    struct hash_table_entry *new_entry = malloc(sizeof(struct hash_table_entry));
    if (!new_entry) {
        perror("malloc hash_table_entry failed");
        destroy_ring_cache(new_cache);
        pthread_mutex_unlock(&mgr->global_lock);
        return NULL;
    }
    new_entry->key = *key;
    new_entry->cache = new_cache;
    new_entry->process_id = child_pid;
    new_entry->next = mgr->hash_table[hash_index];
    mgr->hash_table[hash_index] = new_entry;
    mgr->total_connections++;

    *pid = child_pid;
    pthread_mutex_unlock(&mgr->global_lock);
    return new_cache;
}

// 根据PSN查找数据包（O(1)查找）
struct cached_packet* find_packet_by_psn(const struct connection_key *key, uint32_t psn) {
    if (!g_cache_mgr) return NULL;

    uint32_t hash_index = calculate_hash(key, g_cache_mgr->hash_table_size);
    pthread_mutex_lock(&g_cache_mgr->global_lock);

    struct hash_table_entry *entry = g_cache_mgr->hash_table[hash_index];
    while (entry) {
        if (connection_keys_equal(&entry->key, key)) {
            struct connection_ring_cache *cache = entry->cache;
            pthread_mutex_lock(&cache->lock);

            // 直接计算偏移量定位数据包
            size_t offset = psn_to_offset(cache, psn);
            struct cached_packet *packet = &cache->ring[offset];
            
            if (packet->valid && packet->psn == psn) {
                // 返回数据副本
                struct cached_packet *result = malloc(sizeof(struct cached_packet));
                if (result) {
                    result->app_data = malloc(packet->data_len);
                    if (result->app_data) {
                        memcpy(result->app_data, packet->app_data, packet->data_len);
                        result->data_len = packet->data_len;
                        result->dest_qp = packet->dest_qp;
                        result->psn = packet->psn;
                        result->timestamp = packet->timestamp;
                        result->valid = 1;
                    } else {
                        free(result);
                        result = NULL;
                    }
                }
                pthread_mutex_unlock(&cache->lock);
                pthread_mutex_unlock(&g_cache_mgr->global_lock);
                return result;
            }
            
            pthread_mutex_unlock(&cache->lock);
            break;
        }
        entry = entry->next;
    }

    pthread_mutex_unlock(&g_cache_mgr->global_lock);
    return NULL;
}

// 确认PSN（触发窗口滑动）
int acknowledge_psn(const struct connection_key *key, uint32_t ack_psn) {
    if (!g_cache_mgr) return -1;
    
    uint32_t hash_index = calculate_hash(key, g_cache_mgr->hash_table_size);
    pthread_mutex_lock(&g_cache_mgr->global_lock);
    
    struct hash_table_entry *entry = g_cache_mgr->hash_table[hash_index];
    while (entry) {
        if (connection_keys_equal(&entry->key, key)) {
            struct connection_ring_cache *cache = entry->cache;
            pthread_mutex_lock(&cache->lock);
            slide_window(cache, ack_psn);
            gettimeofday(&cache->last_activity, NULL);
            pthread_mutex_unlock(&cache->lock);
            pthread_mutex_unlock(&g_cache_mgr->global_lock);
            return 0;
        }
        entry = entry->next;
    }
    
    pthread_mutex_unlock(&g_cache_mgr->global_lock);
    return -1;
}

// 处理NACK重传
void handle_nack_retransmit(const struct connection_key *key, uint32_t nack_psn) {
    struct cached_packet *packet = find_packet_by_psn(key, nack_psn);
    if (packet && packet->valid) {
        printf("重传报文: PSN=%u, 长度=%d\n", packet->psn, packet->data_len);
        // 实际重传逻辑（如调用IBV发送接口）
        free(packet->app_data);
        free(packet);
    } else {
        printf("未找到需要重传的报文: PSN=%u\n", nack_psn);
    }
}

// 批量处理线程函数（每个线程独立处理部分队列）
void *batch_processor_thread(void *arg) {
    struct cache_manager *mgr = (struct cache_manager *)arg;
    struct timespec timeout;
    
    while (mgr->batch_running) {
        pthread_mutex_lock(&mgr->batch_q.lock);
        
        // 计算超时时间（避免发包慢的连接阻塞）
        clock_gettime(CLOCK_REALTIME, &timeout);
        timeout.tv_nsec += mgr->batch_timeout * 1000000;
        if (timeout.tv_nsec >= 1000000000) {
            timeout.tv_sec += 1;
            timeout.tv_nsec -= 1000000000;
        }
        
        // 等待阈值或超时
        while (mgr->batch_q.count < mgr->batch_threshold && mgr->batch_running) {
            int ret = pthread_cond_timedwait(&mgr->batch_q.cond, 
                                            &mgr->batch_q.lock, &timeout);
            if (ret == ETIMEDOUT) break; // 超时强制处理
        }
        
        // 取出队列数据
        struct batch_node *head = mgr->batch_q.head;
        struct batch_node *tail = mgr->batch_q.tail;
        size_t count = mgr->batch_q.count;
        mgr->batch_q.head = NULL;
        mgr->batch_q.tail = NULL;
        mgr->batch_q.count = 0;
        
        pthread_mutex_unlock(&mgr->batch_q.lock);
        
        if (count > 0) {
            struct batch_node *current = head;
            while (current) {
                struct batch_node *next = current->next;
                pid_t conn_pid;
                struct connection_ring_cache *conn_cache = get_or_create_ring_cache(
                    mgr, &current->key, current->psn, &conn_pid);
                
                if (conn_cache) {
                    insert_packet_to_ring(conn_cache, current->psn, 
                                         current->app_data, current->data_len,
                                         current->dest_qp);
                }
                
                free(current->app_data);
                free(current);
                current = next;
            }
        }
    }
    return NULL;
}

// 添加数据包到批量处理队列
int add_to_batch_queue(const char *src_ip, const char *dst_ip,
                      uint16_t src_port, uint16_t dst_port,
                      uint32_t src_qp, uint32_t dest_qp,
                      uint8_t service_type, uint16_t pkey,
                      uint32_t psn, const unsigned char *app_data, int data_len) {
    if (!g_cache_mgr || !app_data || data_len <= 0) {
        fprintf(stderr, "无效参数\n");
        return -1;
    }
    
    struct connection_key key = create_connection_key(src_ip, dst_ip, 
                                                     src_port, dst_port,
                                                     src_qp, dest_qp,
                                                     service_type, pkey);
    
    struct batch_node *node = malloc(sizeof(struct batch_node));
    if (!node) {
        perror("malloc batch_node failed");
        return -1;
    }
    
    node->key = key;
    node->psn = psn;
    node->data_len = data_len;
    node->dest_qp = dest_qp;
    node->app_data = malloc(data_len);
    node->next = NULL;
    
    if (!node->app_data) {
        perror("malloc app_data failed");
        free(node);
        return -1;
    }
    memcpy(node->app_data, app_data, data_len);
    
    pthread_mutex_lock(&g_cache_mgr->batch_q.lock);
    if (g_cache_mgr->batch_q.tail) {
        g_cache_mgr->batch_q.tail->next = node;
    } else {
        g_cache_mgr->batch_q.head = node;
    }
    g_cache_mgr->batch_q.tail = node;
    g_cache_mgr->batch_q.count++;
    
    // 达到阈值唤醒线程
    if (g_cache_mgr->batch_q.count >= g_cache_mgr->batch_threshold) {
        pthread_cond_signal(&g_cache_mgr->batch_q.cond);
    }
    pthread_mutex_unlock(&g_cache_mgr->batch_q.lock);
    return 0;
}

// 启动批量处理线程
int start_batch_processor(struct cache_manager *mgr) {
    mgr->batch_threads = malloc(sizeof(pthread_t) * mgr->num_batch_threads);
    if (!mgr->batch_threads) {
        perror("malloc batch_threads failed");
        return -1;
    }
    for (size_t i = 0; i < mgr->num_batch_threads; i++) {
        if (pthread_create(&mgr->batch_threads[i], NULL, batch_processor_thread, mgr) != 0) {
            perror("pthread_create batch processor failed");
            return -1;
        }
    }
    return 0;
}

// 停止批量处理线程
void stop_batch_processor(struct cache_manager *mgr) {
    if (!mgr) return;
    
    mgr->batch_running = 0;
    pthread_cond_broadcast(&mgr->batch_q.cond); // 唤醒所有线程
    for (size_t i = 0; i < mgr->num_batch_threads; i++) {
        pthread_join(mgr->batch_threads[i], NULL);
    }
    
    // 清理剩余节点
    struct batch_node *current = mgr->batch_q.head;
    while (current) {
        struct batch_node *next = current->next;
        free(current->app_data);
        free(current);
        current = next;
    }
    
    free(mgr->batch_threads);
    pthread_mutex_destroy(&mgr->batch_q.lock);
    pthread_cond_destroy(&mgr->batch_q.cond);
}

// 清理过期连接
void cleanup_expired_connections() {
    if (!g_cache_mgr) return;
    
    struct timeval now;
    gettimeofday(&now, NULL);
    
    pthread_mutex_lock(&g_cache_mgr->global_lock);
    
    for (size_t i = 0; i < g_cache_mgr->hash_table_size; i++) {
        struct hash_table_entry **entry_ptr = &g_cache_mgr->hash_table[i];
        while (*entry_ptr) {
            struct hash_table_entry *entry = *entry_ptr;
            struct connection_ring_cache *cache = entry->cache;
            
            pthread_mutex_lock(&cache->lock);
            long idle_seconds = now.tv_sec - cache->last_activity.tv_sec;
            
            if (idle_seconds > g_cache_mgr->connection_timeout) {
                // 终止子进程
                kill(entry->process_id, SIGTERM);
                waitpid(entry->process_id, NULL, 0);
                
                *entry_ptr = entry->next;
                g_cache_mgr->total_connections--;
                pthread_mutex_unlock(&cache->lock);
                destroy_ring_cache(cache);
                free(entry);
            } else {
                pthread_mutex_unlock(&cache->lock);
                entry_ptr = &(*entry_ptr)->next;
            }
        }
    }
    
    pthread_mutex_unlock(&g_cache_mgr->global_lock);
}

// 初始化缓存管理器（限制10连接，每连接100MB）
struct cache_manager* init_cache_manager(size_t hash_size, 
                                        size_t max_conns,
                                        size_t ring_size,
                                        size_t max_bytes_per_conn_mb,
                                        int conn_timeout_seconds,
                                        size_t batch_threshold,
                                        int batch_timeout_ms,
                                        uint32_t window_size,
                                        size_t num_batch_threads) {
    struct cache_manager *mgr = malloc(sizeof(struct cache_manager));
    if (!mgr) {
        perror("malloc cache_manager failed");
        return NULL;
    }
    
    mgr->hash_table_size = hash_size;
    mgr->hash_table = calloc(hash_size, sizeof(struct hash_table_entry*));
    if (!mgr->hash_table) {
        perror("calloc hash_table failed");
        free(mgr);
        return NULL;
    }
    
    mgr->max_connections = max_conns; // 限制为10
    mgr->ring_cache_size = ring_size;
    mgr->max_bytes_per_conn = max_bytes_per_conn_mb * 1024 * 1024; // 100MB
    mgr->connection_timeout = conn_timeout_seconds;
    mgr->total_connections = 0;
    mgr->default_window_size = window_size;
    
    // 初始化批量队列
    mgr->batch_q.head = NULL;
    mgr->batch_q.tail = NULL;
    mgr->batch_q.count = 0;
    mgr->batch_threshold = batch_threshold;
    mgr->batch_timeout = batch_timeout_ms;
    mgr->batch_running = 1;
    mgr->num_batch_threads = num_batch_threads;
    
    if (pthread_mutex_init(&mgr->batch_q.lock, NULL) != 0 ||
        pthread_cond_init(&mgr->batch_q.cond, NULL) != 0) {
        perror("init batch queue mutex/cond failed");
        free(mgr->hash_table);
        free(mgr);
        return NULL;
    }
    
    if (pthread_mutex_init(&mgr->global_lock, NULL) != 0) {
        perror("pthread_mutex_init global lock failed");
        pthread_mutex_destroy(&mgr->batch_q.lock);
        pthread_cond_destroy(&mgr->batch_q.cond);
        free(mgr->hash_table);
        free(mgr);
        return NULL;
    }
    
    return mgr;
}

// 销毁缓存管理器
void destroy_cache_manager(struct cache_manager *mgr) {
    if (!mgr) return;
    
    stop_batch_processor(mgr);
    
    // 释放哈希表资源
    for (size_t i = 0; i < mgr->hash_table_size; i++) {
        struct hash_table_entry *entry = mgr->hash_table[i];
        while (entry) {
            struct hash_table_entry *next = entry->next;
            // 终止子进程
            kill(entry->process_id, SIGTERM);
            waitpid(entry->process_id, NULL, 0);
            destroy_ring_cache(entry->cache);
            free(entry);
            entry = next;
        }
    }
    
    free(mgr->hash_table);
    pthread_mutex_destroy(&mgr->global_lock);
    free(mgr);
}

// 打印所有连接状态
void print_all_connections_status() {
    if (!g_cache_mgr) {
        printf("缓存管理器未初始化\n");
        return;
    }

    pthread_mutex_lock(&g_cache_mgr->global_lock);
    printf("\n===== 所有连接状态 =====\n");
    printf("总连接数: %zu/%zu (当前/最大)\n", 
           g_cache_mgr->total_connections, g_cache_mgr->max_connections);
    printf("------------------------------------------------------------------------\n");
    printf("%-15s | %-15s | %-6s | %-6s | %-8s | %-8s | %-8s | %-6s\n",
           "源IP", "目标IP", "源端口", "目标端口", "源QP", "目标QP", "缓存使用率", "状态");
    printf("------------------------------------------------------------------------\n");

    for (size_t i = 0; i < g_cache_mgr->hash_table_size; i++) {
        struct hash_table_entry *entry = g_cache_mgr->hash_table[i];
        while (entry) {
            struct connection_key *key = &entry->key;
            struct connection_ring_cache *cache = entry->cache;
            
            // 转换IP地址格式
            char src_ip_str[INET_ADDRSTRLEN];
            char dst_ip_str[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &key->src_ip, src_ip_str, INET_ADDRSTRLEN);
            inet_ntop(AF_INET, &key->dst_ip, dst_ip_str, INET_ADDRSTRLEN);
            
            // 计算缓存使用率
            pthread_mutex_lock(&cache->lock);
            double usage = (double)cache->total_bytes / g_cache_mgr->max_bytes_per_conn * 100;
            
            // 计算空闲时间
            struct timeval now;
            gettimeofday(&now, NULL);
            long idle_seconds = now.tv_sec - cache->last_activity.tv_sec;
            const char *status = (idle_seconds < g_cache_mgr->connection_timeout / 2) ? 
                               "活跃" : "空闲";
            
            // 打印连接详情
            printf("%-15s | %-15s | %-6hu | %-6hu | %-8u | %-8u | %-6.2f%% | %-6s\n",
                   src_ip_str, dst_ip_str,
                   ntohs(key->src_port), ntohs(key->dst_port),
                   key->src_qp, key->dest_qp,
                   usage, status);
            
            // 打印滑动窗口信息
            printf("  滑动窗口: 期望PSN=%u, 已确认最高PSN=%u, 窗口大小=%u\n",
                   cache->next_expected_psn,
                   cache->highest_ack_psn,
                   cache->window_size);
                   
            pthread_mutex_unlock(&cache->lock);
            entry = entry->next;
        }
    }

    printf("------------------------------------------------------------------------\n");
    pthread_mutex_unlock(&g_cache_mgr->global_lock);
}
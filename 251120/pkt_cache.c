// RDMA报文缓存管理系统（单连接环形缓存+多连接批量处理版本）
/*
整体逻辑：
1. 单连接管理：每个连接使用独立环形内存区，通过PSN直接计算存储偏移
2. 多连接管理：使用哈希表管理所有连接，连接键基于五元组+QP信息
3. 批量缓存机制：定期集中处理待缓存数据包，平衡内存分配效率与实时性
4. 重传优化：通过PSN直接定位数据包，实现快速查找

编译命令：
gcc pkt_cache.c -o pkt_cache -lpthread -lrdmacm -libverbs

运行命令：
sudo ./pkt_cache
*/

#include <pthread.h>
#include <time.h>
#include <sys/time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netinet/in.h>
#include <errno.h>  // 新增：解决ETIMEDOUT未定义问题

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

// 单个连接的环形缓存（新增滑动窗口字段）
struct connection_ring_cache {
    struct cached_packet *ring;   // 环形缓冲区
    size_t ring_size;             // 环形缓冲区大小
    uint32_t base_psn;            // 基准PSN，用于计算偏移
    uint32_t min_psn;             // 最小PSN
    uint32_t max_psn;             // 最大PSN
    size_t valid_count;           // 有效数据包数量
    size_t total_bytes;           // 总字节数
    struct timeval last_activity; // 最后活动时间
    pthread_mutex_t lock;         // 连接级锁

    // 滑动窗口新增字段
    uint32_t next_expected_psn;   // 窗口左沿：期望接收的下一个PSN
    uint32_t window_size;         // 窗口大小
    uint32_t highest_ack_psn;     // 已确认的最高PSN
};

// 哈希表节点
struct hash_table_entry {
    struct connection_key key;    // 连接标识
    struct connection_ring_cache *cache; // 对应的环形缓存
    struct hash_table_entry *next;// 哈希冲突链表
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

// 全局缓存管理器（新增窗口大小配置）
struct cache_manager {
    struct hash_table_entry **hash_table; // 哈希表
    size_t hash_table_size;       // 哈希表大小
    size_t max_connections;       // 最大连接数
    size_t ring_cache_size;       // 每个连接的环形缓存大小
    size_t max_bytes_per_conn;    // 每连接最大字节数
    int connection_timeout;       // 连接超时时间（秒）
    pthread_mutex_t global_lock;  // 全局锁
    size_t total_connections;     // 总连接数
    
    // 批量处理相关
    struct batch_queue batch_q;   // 批量处理队列
    pthread_t batch_thread;       // 批量处理线程
    int batch_running;            // 批量线程运行标志
    size_t batch_threshold;       // 批量处理阈值
    int batch_timeout;            // 批量处理超时时间（毫秒）

    // 滑动窗口配置
    uint32_t default_window_size; // 默认窗口大小
};

// 全局缓存管理器实例
struct cache_manager *g_cache_mgr = NULL;

// 哈希计算函数
uint32_t calculate_hash(const struct connection_key *key, size_t table_size)
{
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
int connection_keys_equal(const struct connection_key *a,
                          const struct connection_key *b)
{
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
                                            uint8_t service_type, uint16_t pkey)
{
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

// 打印连接键信息
void print_connection_key(const struct connection_key *key) {
    char src_ip[INET_ADDRSTRLEN];
    char dst_ip[INET_ADDRSTRLEN];

    inet_ntop(AF_INET, &key->src_ip, src_ip, INET_ADDRSTRLEN);
    inet_ntop(AF_INET, &key->dst_ip, dst_ip, INET_ADDRSTRLEN);

    printf("连接: %s:%d (QP=%u) -> %s:%d (QP=%u), 服务类型=%d, pkey=0x%04x",
           src_ip, ntohs(key->src_port), key->src_qp,
           dst_ip, ntohs(key->dst_port), key->dest_qp,
           key->service_type, key->pkey);
}

// 初始化缓存管理器（新增窗口大小参数）
struct cache_manager* init_cache_manager(size_t hash_size, 
                                        size_t max_conns,
                                        size_t ring_size,
                                        size_t max_bytes_per_conn_mb,
                                        int conn_timeout_seconds,
                                        size_t batch_threshold,
                                        int batch_timeout_ms,
                                        uint32_t window_size)  // 新增窗口大小参数
{
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
    
    mgr->max_connections = max_conns;
    mgr->ring_cache_size = ring_size;
    mgr->max_bytes_per_conn = max_bytes_per_conn_mb * 1024 * 1024;
    mgr->connection_timeout = conn_timeout_seconds;
    mgr->total_connections = 0;
    mgr->default_window_size = window_size;  // 初始化窗口大小
    
    // 初始化批量处理队列
    mgr->batch_q.head = NULL;
    mgr->batch_q.tail = NULL;
    mgr->batch_q.count = 0;
    mgr->batch_threshold = batch_threshold;
    mgr->batch_timeout = batch_timeout_ms;
    mgr->batch_running = 1;
    
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
    
    printf("初始化缓存管理器: 哈希表大小=%zu, 最大连接数=%zu, 环形缓存大小=%zu, 窗口大小=%u, 批量阈值=%zu, 批量超时=%dms\n",
           hash_size, max_conns, ring_size, window_size, batch_threshold, batch_timeout_ms);
    
    return mgr;
}

// 创建连接的环形缓存（初始化滑动窗口参数）
struct connection_ring_cache* create_ring_cache(size_t ring_size, uint32_t first_psn, uint32_t window_size)
{
    struct connection_ring_cache *cache = malloc(sizeof(struct connection_ring_cache));
    if (!cache) {
        perror("malloc connection_ring_cache");
        return NULL;
    }
    
    // 分配环形缓冲区
    cache->ring = calloc(ring_size, sizeof(struct cached_packet));
    if (!cache->ring) {
        perror("calloc ring buffer");
        free(cache);
        return NULL;
    }
    
    cache->ring_size = ring_size;
    cache->base_psn = first_psn;
    cache->min_psn = first_psn;
    cache->max_psn = first_psn;
    cache->valid_count = 0;
    cache->total_bytes = 0;
    gettimeofday(&cache->last_activity, NULL);
    
    // 滑动窗口初始化
    cache->next_expected_psn = first_psn;      // 初始期望PSN为第一个包的PSN
    cache->window_size = window_size;          // 窗口大小
    cache->highest_ack_psn = first_psn - 1;    // 初始已确认PSN为第一个包之前
    
    if (pthread_mutex_init(&cache->lock, NULL) != 0) {
        perror("pthread_mutex_init ring cache lock");
        free(cache->ring);
        free(cache);
        return NULL;
    }
    
    return cache;
}

// 销毁连接缓存
void destroy_ring_cache(struct connection_ring_cache *cache)
{
    if (!cache) return;
    
    pthread_mutex_lock(&cache->lock);
    
    // 释放所有数据包数据
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

// 获取或创建连接缓存（传递窗口大小参数）
struct connection_ring_cache* get_or_create_ring_cache(
    struct cache_manager *mgr, const struct connection_key *key,
    uint32_t first_psn)
{
    uint32_t hash_index = calculate_hash(key, mgr->hash_table_size);
    
    pthread_mutex_lock(&mgr->global_lock);
    
    // 查找现有连接
    struct hash_table_entry *entry = mgr->hash_table[hash_index];
    while (entry) {
        if (connection_keys_equal(&entry->key, key)) {
            gettimeofday(&entry->cache->last_activity, NULL);
            pthread_mutex_unlock(&mgr->global_lock);
            return entry->cache;
        }
        entry = entry->next;
    }
    
    // 检查连接数限制
    if (mgr->total_connections >= mgr->max_connections) {
        printf("达到最大连接数限制 (%zu/%zu)\n",
               mgr->total_connections, mgr->max_connections);
        pthread_mutex_unlock(&mgr->global_lock);
        return NULL;
    }
    
    // 创建新连接缓存（传入窗口大小）
    struct connection_ring_cache *new_cache = create_ring_cache(
        mgr->ring_cache_size, first_psn, mgr->default_window_size);
    if (!new_cache) {
        pthread_mutex_unlock(&mgr->global_lock);
        return NULL;
    }
    
    struct hash_table_entry *new_entry = malloc(sizeof(struct hash_table_entry));
    if (!new_entry) {
        perror("malloc hash_table_entry");
        destroy_ring_cache(new_cache);
        pthread_mutex_unlock(&mgr->global_lock);
        return NULL;
    }
    
    new_entry->key = *key;
    new_entry->cache = new_cache;
    new_entry->next = mgr->hash_table[hash_index];
    mgr->hash_table[hash_index] = new_entry;
    mgr->total_connections++;
    
    pthread_mutex_unlock(&mgr->global_lock);
    
    printf("创建新连接缓存: ");
    print_connection_key(key);
    printf(", 环形大小=%zu, 基准PSN=%u, 窗口大小=%u\n", 
           new_cache->ring_size, new_cache->base_psn, new_cache->window_size);
    
    return new_cache;
}

// 计算PSN在环形缓存中的偏移量
static inline size_t psn_to_offset(struct connection_ring_cache *cache, uint32_t psn)
{
    // 处理PSN回绕情况
    if (psn >= cache->base_psn) {
        return (psn - cache->base_psn) % cache->ring_size;
    } else {
        // 当PSN小于基准PSN时，说明发生了回绕
        return (psn + (0xFFFFFFFF - cache->base_psn) + 1) % cache->ring_size;
    }
}

// 滑动窗口：释放已确认的数据包（新增函数）
static void slide_window(struct connection_ring_cache *cache, uint32_t new_ack_psn)
{
    if (new_ack_psn <= cache->highest_ack_psn) return;
    
    // 从当前已确认PSN+1开始释放，直到新确认的PSN
    for (uint32_t psn = cache->highest_ack_psn + 1; psn <= new_ack_psn; psn++) {
        size_t offset = psn_to_offset(cache, psn);
        struct cached_packet *packet = &cache->ring[offset];
        
        if (packet->valid && packet->psn == psn) {
            free(packet->app_data);
            packet->app_data = NULL;
            packet->valid = 0;
            cache->total_bytes -= packet->data_len;
            cache->valid_count--;
        }
    }
    
    // 更新窗口状态
    cache->highest_ack_psn = new_ack_psn;
    cache->next_expected_psn = new_ack_psn + 1;
}

// 确认PSN（供重传模块调用，新增接口）
int acknowledge_psn(const struct connection_key *key, uint32_t ack_psn)
{
    if (!g_cache_mgr) return -1;
    
    uint32_t hash_index = calculate_hash(key, g_cache_mgr->hash_table_size);
    pthread_mutex_lock(&g_cache_mgr->global_lock);
    
    struct hash_table_entry *entry = g_cache_mgr->hash_table[hash_index];
    while (entry) {
        if (connection_keys_equal(&entry->key, key)) {
            struct connection_ring_cache *cache = entry->cache;
            pthread_mutex_lock(&cache->lock);
            
            // 滑动窗口释放已确认数据包
            slide_window(cache, ack_psn);
            gettimeofday(&cache->last_activity, NULL);
            
            pthread_mutex_unlock(&cache->lock);
            pthread_mutex_unlock(&g_cache_mgr->global_lock);
            return 0;
        }
        entry = entry->next;
    }
    
    pthread_mutex_unlock(&g_cache_mgr->global_lock);
    return -1;  // 未找到连接
}

// 将数据包插入环形缓存（增加窗口检查）
int insert_packet_to_ring(struct connection_ring_cache *cache, 
                         uint32_t psn, const unsigned char *app_data, 
                         int data_len, uint32_t dest_qp)
{
    if (!cache || !app_data || data_len <= 0)
        return -1;
    
    pthread_mutex_lock(&cache->lock);
    
    // 检查是否在窗口范围内（允许窗口外乱序包，但标记为待处理）
    uint32_t window_right = cache->next_expected_psn + cache->window_size - 1;
    int is_in_window = (psn >= cache->next_expected_psn && psn <= window_right);
    
    // 计算偏移量
    size_t offset = psn_to_offset(cache, psn);
    struct cached_packet *packet = &cache->ring[offset];
    
    // 如果已有有效数据，先释放
    if (packet->valid) {
        cache->total_bytes -= packet->data_len;
        free(packet->app_data);
        cache->valid_count--;
    }
    
    // 检查是否超过最大字节限制
    if (cache->total_bytes + data_len > g_cache_mgr->max_bytes_per_conn) {
        printf("连接缓存超出最大字节限制\n");
        pthread_mutex_unlock(&cache->lock);
        return -1;
    }
    
    // 分配并复制数据
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
    
    // 更新缓存统计信息
    cache->total_bytes += data_len;
    cache->valid_count++;
    
    // 更新PSN范围
    if (psn < cache->min_psn)
        cache->min_psn = psn;
    if (psn > cache->max_psn)
        cache->max_psn = psn;
    
    // 如果是期望的下一个PSN，尝试滑动窗口
    if (psn == cache->next_expected_psn) {
        slide_window(cache, psn);  // 确认当前PSN，触发窗口滑动
    }
    
    // 更新最后活动时间
    gettimeofday(&cache->last_activity, NULL);
    
    pthread_mutex_unlock(&cache->lock);
    return 0;
}

// 批量处理线程函数
void *batch_processor_thread(void *arg)
{
    struct cache_manager *mgr = (struct cache_manager *)arg;
    struct timespec timeout;
    
    printf("批量处理线程启动\n");
    
    while (mgr->batch_running) {
        pthread_mutex_lock(&mgr->batch_q.lock);
        
        // 计算超时时间
        clock_gettime(CLOCK_REALTIME, &timeout);
        timeout.tv_nsec += mgr->batch_timeout * 1000000; // 转换为纳秒
        if (timeout.tv_nsec >= 1000000000) {
            timeout.tv_sec += 1;
            timeout.tv_nsec -= 1000000000;
        }
        
        // 等待直到达到阈值或超时
        while (mgr->batch_q.count < mgr->batch_threshold && mgr->batch_running) {
            int ret = pthread_cond_timedwait(&mgr->batch_q.cond, 
                                            &mgr->batch_q.lock, &timeout);
            if (ret == ETIMEDOUT) {  // 已包含<errno.h>，可正常使用
                // 超时，即使未达阈值也处理
                break;
            }
        }
        
        // 取出队列中的所有报文
        struct batch_node *head = mgr->batch_q.head;
        struct batch_node *tail = mgr->batch_q.tail;
        size_t count = mgr->batch_q.count;
        
        // 清空队列
        mgr->batch_q.head = NULL;
        mgr->batch_q.tail = NULL;
        mgr->batch_q.count = 0;
        
        pthread_mutex_unlock(&mgr->batch_q.lock);
        
        if (count > 0) {
            printf("批量处理 %zu 个报文\n", count);
            
            // 处理每个报文
            struct batch_node *current = head;
            while (current) {
                struct batch_node *next = current->next;
                
                // 获取或创建连接缓存
                struct connection_ring_cache *conn_cache = get_or_create_ring_cache(
                    mgr, &current->key, current->psn);
                
                if (conn_cache) {
                    // 插入到环形缓存
                    insert_packet_to_ring(conn_cache, current->psn, 
                                         current->app_data, current->data_len,
                                         current->dest_qp);
                }
                
                // 释放临时数据
                free(current->app_data);
                free(current);
                current = next;
            }
        }
    }
    
    printf("批量处理线程退出\n");
    return NULL;
}

// 启动批量处理线程
int start_batch_processor(struct cache_manager *mgr)
{
    if (pthread_create(&mgr->batch_thread, NULL, batch_processor_thread, mgr) != 0) {
        perror("pthread_create batch processor failed");
        return -1;
    }
    return 0;
}

// 停止批量处理线程
void stop_batch_processor(struct cache_manager *mgr)
{
    if (!mgr) return;
    
    mgr->batch_running = 0;
    pthread_cond_signal(&mgr->batch_q.cond);
    pthread_join(mgr->batch_thread, NULL);
    
    // 处理剩余报文
    struct batch_node *current = mgr->batch_q.head;
    while (current) {
        struct batch_node *next = current->next;
        free(current->app_data);
        free(current);
        current = next;
    }
    
    pthread_mutex_destroy(&mgr->batch_q.lock);
    pthread_cond_destroy(&mgr->batch_q.cond);
}

// 添加数据包到批量处理队列
int add_to_batch_queue(const char *src_ip, const char *dst_ip,
                      uint16_t src_port, uint16_t dst_port,
                      uint32_t src_qp, uint32_t dest_qp,
                      uint8_t service_type, uint16_t pkey,
                      uint32_t psn, const unsigned char *app_data, int data_len)
{
    if (!g_cache_mgr || !app_data || data_len <= 0) {
        fprintf(stderr, "无效参数\n");
        return -1;
    }
    
    // 创建连接键
    struct connection_key key = create_connection_key(src_ip, dst_ip, 
                                                     src_port, dst_port,
                                                     src_qp, dest_qp,
                                                     service_type, pkey);
    
    // 创建批量节点
    struct batch_node *node = malloc(sizeof(struct batch_node));
    if (!node) {
        perror("malloc batch_node");
        return -1;
    }
    
    node->key = key;
    node->psn = psn;
    node->data_len = data_len;
    node->dest_qp = dest_qp;
    node->app_data = malloc(data_len);
    node->next = NULL;
    
    if (!node->app_data) {
        perror("malloc app_data");
        free(node);
        return -1;
    }
    
    memcpy(node->app_data, app_data, data_len);
    
    // 添加到批量队列
    pthread_mutex_lock(&g_cache_mgr->batch_q.lock);
    
    if (g_cache_mgr->batch_q.tail) {
        g_cache_mgr->batch_q.tail->next = node;
    } else {
        g_cache_mgr->batch_q.head = node;
    }
    g_cache_mgr->batch_q.tail = node;
    g_cache_mgr->batch_q.count++;
    
    // 如果达到阈值，唤醒处理线程
    if (g_cache_mgr->batch_q.count >= g_cache_mgr->batch_threshold) {
        pthread_cond_signal(&g_cache_mgr->batch_q.cond);
    }
    
    pthread_mutex_unlock(&g_cache_mgr->batch_q.lock);
    return 0;
}

// 根据PSN查找数据包
struct cached_packet* find_packet_by_psn(const struct connection_key *key, uint32_t psn)
{
    if (!g_cache_mgr) return NULL;
    
    uint32_t hash_index = calculate_hash(key, g_cache_mgr->hash_table_size);
    
    pthread_mutex_lock(&g_cache_mgr->global_lock);
    
    struct hash_table_entry *entry = g_cache_mgr->hash_table[hash_index];
    while (entry) {
        if (connection_keys_equal(&entry->key, key)) {
            struct connection_ring_cache *cache = entry->cache;
            pthread_mutex_lock(&cache->lock);
            
            size_t offset = psn_to_offset(cache, psn);
            struct cached_packet *packet = &cache->ring[offset];
            
            // 检查数据有效性和PSN匹配（处理回绕情况）
            if (packet->valid && packet->psn == psn) {
                // 创建副本返回，避免锁竞争
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

// 处理NACK重传
void handle_nack_retransmit(const struct connection_key *key, uint32_t nack_psn)
{
    if (!g_cache_mgr) return;

    struct cached_packet *packet = find_packet_by_psn(key, nack_psn);
    if (packet && packet->valid) {
        printf("重传报文: PSN=%u, 长度=%d\n", packet->psn, packet->data_len);
        // 这里添加实际的重传逻辑
        free(packet->app_data);
        free(packet);
    } else {
        printf("未找到需要重传的报文: PSN=%u\n", nack_psn);
    }
}

// 清理过期连接
void cleanup_expired_connections()
{
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
                printf("清理过期连接: ");
                print_connection_key(&entry->key);
                printf(", 空闲时间=%ld秒\n", idle_seconds);
                
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

// 打印所有连接状态（增加窗口信息）
void print_all_connections_status() {
    if (!g_cache_mgr) return;
    
    printf("=== 缓存状态 ===\n");
    printf("总连接数: %zu/%zu\n", g_cache_mgr->total_connections, g_cache_mgr->max_connections);
    
    pthread_mutex_lock(&g_cache_mgr->global_lock);
    
    for (size_t i = 0; i < g_cache_mgr->hash_table_size; i++) {
        struct hash_table_entry *entry = g_cache_mgr->hash_table[i];
        while (entry) {
            struct connection_ring_cache *cache = entry->cache;
            pthread_mutex_lock(&cache->lock);
            
            struct timeval now;
            gettimeofday(&now, NULL);
            
            printf("  ");
            print_connection_key(&entry->key);
            printf("\n    有效报文数=%zu/%zu, PSN范围=[%u, %u], 窗口=[%u, %u], 已确认PSN=%u, 数据量=%zu字节, 空闲=%ld秒\n",
                   cache->valid_count, cache->ring_size,
                   cache->min_psn, cache->max_psn,
                   cache->next_expected_psn,  // 窗口左沿
                   cache->next_expected_psn + cache->window_size - 1,  // 窗口右沿
                   cache->highest_ack_psn,    // 已确认最高PSN
                   cache->total_bytes,
                   now.tv_sec - cache->last_activity.tv_sec);
            
            pthread_mutex_unlock(&cache->lock);
            entry = entry->next;
        }
    }
    
    pthread_mutex_unlock(&g_cache_mgr->global_lock);
    printf("================\n");
}

// 销毁缓存管理器
void destroy_cache_manager(struct cache_manager *mgr) {
    if (!mgr) return;
    
    // 停止批量处理线程
    stop_batch_processor(mgr);
    
    // 释放哈希表资源
    for (size_t i = 0; i < mgr->hash_table_size; i++) {
        struct hash_table_entry *entry = mgr->hash_table[i];
        while (entry) {
            struct hash_table_entry *next = entry->next;
            destroy_ring_cache(entry->cache);
            free(entry);
            entry = next;
        }
    }
    
    // 释放其他资源
    free(mgr->hash_table);
    pthread_mutex_destroy(&mgr->global_lock);
    free(mgr);
    mgr = NULL;
}

// // 测试用的生产者线程函数
// void *test_producer_thread(void *arg) {
//     size_t thread_id = *(size_t *)arg;
//     unsigned char data[128];
//     snprintf((char *)data, sizeof(data), "producer_%zu_test_data", thread_id);
    
//     // 每个线程发送20个报文
//     for (uint32_t psn = 1; psn <= 20; psn++) {
//         // 不同线程使用不同的源端口，模拟不同连接
//         uint16_t src_port = 1234 + thread_id;
//         uint32_t actual_psn = psn + thread_id * 1000; // 避免PSN冲突
        
//         add_to_batch_queue("192.168.239.230", "192.168.239.235",
//                          htons(src_port), htons(4791), // 4791是RoCEv2默认端口
//                          thread_id + 100, 200, 0, 0xffff,
//                          actual_psn, data, strlen((char *)data) + 1);
        
//         usleep(50000); // 每50ms发送一个
//     }
//     return NULL;
// }

// // 测试用的确认线程（模拟重传模块确认PSN）
// void *test_ack_thread(void *arg) {
//     sleep(2); // 等待生产者发送部分数据
    
//     // 确认第一个连接的前10个PSN
//     struct connection_key key = create_connection_key("192.168.239.230", "192.168.239.235",
//                                                      htons(1234), htons(4791), 
//                                                      100, 200, 0, 0xffff);
    
//     for (uint32_t psn = 1; psn <= 10; psn++) {
//         uint32_t actual_psn = psn + 0 * 1000; // 对应第一个生产者线程的PSN
//         acknowledge_psn(&key, actual_psn);
//         printf("已确认PSN: %u\n", actual_psn);
//         usleep(100000);
//     }
//     return NULL;
// }

// // 主函数测试
// int main() {
//     // 初始化缓存管理器（新增窗口大小参数）
//     g_cache_mgr = init_cache_manager(
//         1024,       // 哈希表大小
//         1000,       // 最大连接数
//         4096,       // 环形缓存大小
//         100,        // 每连接最大字节数(MB)
//         300,        // 连接超时时间(秒)
//         100,        // 批量处理阈值
//         100,        // 批量处理超时(ms)
//         32);        // 滑动窗口大小（新增参数）
    
//     if (!g_cache_mgr) return 1;

//     // 启动批量处理线程
//     if (start_batch_processor(g_cache_mgr) != 0) {
//         fprintf(stderr, "启动批量处理线程失败\n");
//         return 1;
//     }

//     // 创建3个生产者线程模拟多连接输入
//     pthread_t producer_threads[3];
//     size_t thread_ids[3];
//     for (size_t i = 0; i < 3; i++) {
//         thread_ids[i] = i;
//         pthread_create(&producer_threads[i], NULL, test_producer_thread, &thread_ids[i]);
//     }

//     // 创建确认线程（模拟重传模块的确认操作）
//     pthread_t ack_thread;
//     pthread_create(&ack_thread, NULL, test_ack_thread, NULL);

//     // 等待生产者线程完成
//     for (size_t i = 0; i < 3; i++) {
//         pthread_join(producer_threads[i], NULL);
//     }

//     // 等待确认线程完成
//     pthread_join(ack_thread, NULL);

//     // 等待最后一次批量处理完成
//     sleep(1);

//     // 打印状态（包含窗口信息）
//     print_all_connections_status();

//     // 模拟NACK重传（尝试重传已确认和未确认的包）
//     struct connection_key key = create_connection_key("192.168.239.230", "192.168.239.235",
//                                                      htons(1234), htons(4791), 
//                                                      100, 200, 0, 0xffff);
//     printf("\n触发NACK重传（已确认的PSN=5）:\n");
//     handle_nack_retransmit(&key, 5);  // 已确认的包应该不存在
//     printf("触发NACK重传（未确认的PSN=1005）:\n");
//     handle_nack_retransmit(&key, 1005);

//     // 清理过期连接
//     cleanup_expired_connections();
    
//     // 停止批量处理线程并清理资源
//     stop_batch_processor(g_cache_mgr);
    
//     // 释放哈希表资源
//     for (size_t i = 0; i < g_cache_mgr->hash_table_size; i++) {
//         struct hash_table_entry *entry = g_cache_mgr->hash_table[i];
//         while (entry) {
//             struct hash_table_entry *next = entry->next;
//             destroy_ring_cache(entry->cache);
//             free(entry);
//             entry = next;
//         }
//     }
    
//     free(g_cache_mgr->hash_table);
//     pthread_mutex_destroy(&g_cache_mgr->global_lock);
//     free(g_cache_mgr);
    
//     return 0;
// }
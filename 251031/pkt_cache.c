// RDMA报文多连接哈希存储

#include <pthread.h>
#include <time.h>
#include <sys/time.h>

// 缓存报文结构
struct cached_packet {
    unsigned char *app_data;          // 应用数据载荷
    int data_len;                     // 数据长度
    uint32_t dest_qp;                 // 目标QP号
    uint32_t psn;                     // 包序列号
    struct timeval timestamp;         // 缓存时间戳
    struct cached_packet *next;       // 下一个节点（按PSN排序）
    struct cached_packet *prev;       // 前一个节点（双向链表）
};

// 每个连接的缓存队列
struct connection_cache {
    struct cached_packet *head;       // 队列头（最小PSN）
    struct cached_packet *tail;       // 队列尾（最大PSN）
    size_t count;                     // 当前缓存数量
    size_t total_bytes;               // 总字节数
    uint32_t min_psn;                 // 最小PSN
    uint32_t max_psn;                 // 最大PSN
    struct timeval last_activity;     // 最后活动时间
    pthread_mutex_t lock;             // 连接级锁
};

// IPv4连接标识键（用于哈希表）
struct connection_key {
    uint32_t src_ip;                  // 源IP
    uint32_t dst_ip;                  // 目的IP
    uint16_t src_port;                // 源端口
    uint16_t dst_port;                // 目的端口
    uint32_t dest_qp;                 // 目标主机QP号 // TODO: 新增字段, 后面代码需要适配
    uint32_t src_qp;                  // 源主机QP号   // TODO: 新增字段, 后面代码需要适配
    uint8_t service_type;             // RDMA服务类型 // TODO: 待分析, 可能不需要此字段
    uint16_t pkey;                    // 分区键
};

// 哈希表节点
struct hash_table_entry {
    struct connection_key key;        // 连接标识
    struct connection_cache *cache;   // 对应的缓存队列
    struct hash_table_entry *next;    // 哈希冲突链表
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

// 哈希表和相关工具函数
// 计算连接键的哈希值
uint32_t calculate_hash(const struct connection_key *key, size_t table_size)
{
    uint32_t hash = 5381;
    
    hash = ((hash << 5) + hash) + key->src_ip;
    hash = ((hash << 5) + hash) + key->dst_ip;
    hash = ((hash << 5) + hash) + key->src_port;
    hash = ((hash << 5) + hash) + key->dst_port;
    hash = ((hash << 5) + hash) + key->service_type;
    hash = ((hash << 5) + hash) + key->pkey;
    
    return hash % table_size;
}

// 比较两个连接键是否相等
int connection_keys_equal(const struct connection_key *a,
                          const struct connection_key *b)
{
    return (a->src_ip == b->src_ip &&
            a->dst_ip == b->dst_ip &&
            a->src_port == b->src_port &&
            a->dst_port == b->dst_port &&
            a->service_type == b->service_type &&
            a->pkey == b->pkey);
}

// 创建连接键
struct connection_key create_connection_key(const char *src_ip, const char *dst_ip,
                                            uint16_t src_port, uint16_t dst_port,
                                            uint8_t service_type, uint16_t pkey)
{
    struct connection_key key;
    
    inet_pton(AF_INET, src_ip, &key.src_ip);
    inet_pton(AF_INET, dst_ip, &key.dst_ip);
    key.src_port = src_port;
    key.dst_port = dst_port;
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
    
    printf("连接: %s:%d -> %s:%d, 服务类型=%d, pkey=0x%04x",
           src_ip, key->src_port, dst_ip, key->dst_port, 
           key->service_type, key->pkey);
}

// 缓存管理器初始化
// 全局缓存管理器实例
static struct cache_manager *g_cache_mgr = NULL;

// 初始化缓存管理器
struct cache_manager* init_cache_manager(size_t hash_size, 
                                        size_t max_conns,
                                        size_t max_packets_per_conn,
                                        size_t max_bytes_per_conn_mb,
                                        int conn_timeout_seconds)
{
    struct cache_manager *mgr = malloc(sizeof(struct cache_manager));
    if (!mgr) {
        perror("malloc cache_manager");
        return NULL;
    }
    
    // 分配哈希表
    mgr->hash_table_size = hash_size;
    mgr->hash_table = calloc(hash_size, sizeof(struct hash_table_entry*));
    if (!mgr->hash_table) {
        perror("calloc hash_table");
        free(mgr);
        return NULL;
    }
    
    mgr->max_connections = max_conns;
    mgr->max_packets_per_conn = max_packets_per_conn;
    // TODO: 数字修改成宏定义, 方便全局统一修改缓存最大容量
    mgr->max_bytes_per_conn = max_bytes_per_conn_mb * 1024 * 1024;
    mgr->connection_timeout = conn_timeout_seconds;
    mgr->total_connections = 0;
    
    if (pthread_mutex_init(&mgr->global_lock, NULL) != 0) {
        perror("pthread_mutex_init global_lock");
        free(mgr->hash_table);
        free(mgr);
        return NULL;
    }
    
    printf("初始化缓存管理器: 哈希表大小=%zu, 最大连接数=%zu, 每连接最大报文数=%zu\n",
           hash_size, max_conns, max_packets_per_conn);
    
    return mgr;
}

// 连接缓存管理
// 创建新的连接缓存
struct connection_cache* create_connection_cache()
{
    struct connection_cache *cache = malloc(sizeof(struct connection_cache));
    if (!cache) {
        perror("malloc connection_cache");
        return NULL;
    }
    
    cache->head = NULL;
    cache->tail = NULL;
    cache->count = 0;
    cache->total_bytes = 0;
    cache->min_psn = 0;
    cache->max_psn = 0;
    gettimeofday(&cache->last_activity, NULL);
    
    if (pthread_mutex_init(&cache->lock, NULL) != 0) {
        perror("pthread_mutex_init connection_cache lock");
        free(cache);
        return NULL;
    }
    
    return cache;
}

// 销毁连接缓存
void destroy_connection_cache(struct connection_cache *cache)
{
    if (!cache) return;
    
    pthread_mutex_lock(&cache->lock);
    
    // 清空所有缓存报文
    struct cached_packet *current = cache->head;
    while (current) {
        struct cached_packet *next = current->next;
        free(current->app_data);
        free(current);
        current = next;
    }
    
    pthread_mutex_unlock(&cache->lock);
    pthread_mutex_destroy(&cache->lock);
    free(cache);
}

// 内部函数声明
int connection_keys_equal(const struct connection_key *a,
                          const struct connection_key *b);

// 查找或创建连接缓存
struct connection_cache* get_or_create_connection_cache(
        struct cache_manager *mgr, const struct connection_key *key)
{
    uint32_t hash_index = calculate_hash(key, mgr->hash_table_size);
    
    pthread_mutex_lock(&mgr->global_lock);
    
    // 查找现有连接
    struct hash_table_entry *entry = mgr->hash_table[hash_index];
    while (entry) {
        if (connection_keys_equal(&entry->key, key)) {
            // 更新最后活动时间
            gettimeofday(&entry->cache->last_activity, NULL);
            pthread_mutex_unlock(&mgr->global_lock);
            return entry->cache;
        }
        entry = entry->next;
    }
    
    // 检查连接数限制
    if (mgr->total_connections >= mgr->max_connections) {
        printf("达到最大连接数限制 (%zu/%zu), 无法创建新连接缓存\n",
               mgr->total_connections, mgr->max_connections);
        pthread_mutex_unlock(&mgr->global_lock);
        return NULL;
    }
    
    // 创建新连接缓存
    struct connection_cache *new_cache = create_connection_cache();
    if (!new_cache) {
        pthread_mutex_unlock(&mgr->global_lock);
        return NULL;
    }
    
    // 创建哈希表条目
    struct hash_table_entry *new_entry = malloc(sizeof(struct hash_table_entry));
    if (!new_entry) {
        perror("malloc hash_table_entry");
        destroy_connection_cache(new_cache);
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
    printf("\n");
    
    return new_cache;
}

// 有序插入缓存报文（按PSN排序）
// 在连接缓存中按PSN顺序插入报文
int insert_packet_sorted(struct connection_cache *cache, 
                         struct cached_packet *new_packet)
{
    if (!cache || !new_packet)
        return -1;
    
    pthread_mutex_lock(&cache->lock);
    
    // 检查缓存限制
    size_t new_total_bytes = cache->total_bytes + new_packet->data_len + sizeof(struct cached_packet);
    if (cache->count >= g_cache_mgr->max_packets_per_conn ||
        new_total_bytes >= g_cache_mgr->max_bytes_per_conn) {
        printf("连接缓存已满 (%zu/%zu 报文, %zu/%zu 字节)\n",
               cache->count, g_cache_mgr->max_packets_per_conn,
               cache->total_bytes, g_cache_mgr->max_bytes_per_conn);
        pthread_mutex_unlock(&cache->lock);
        return -1;
    }
    
    // 更新PSN范围
    if (cache->count == 0) {
        cache->min_psn = new_packet->psn;
        cache->max_psn = new_packet->psn;
    } else {
        if (new_packet->psn < cache->min_psn)
            cache->min_psn = new_packet->psn;
        if (new_packet->psn > cache->max_psn)
            cache->max_psn = new_packet->psn;
    }
    
    // 按PSN顺序插入到双向链表中
    struct cached_packet *current = cache->head;
    struct cached_packet *prev = NULL;
    
    // 查找插入位置
    while (current && current->psn < new_packet->psn) {
        prev = current;
        current = current->next;
    }
    
    // 处理重复PSN（理论上不应该出现）
    if (current && current->psn == new_packet->psn) {
        printf("警告: 重复PSN %u, 替换现有报文\n", new_packet->psn);
        // 移除现有报文
        if (prev) {
            prev->next = current->next;
        } else {
            cache->head = current->next;
        }
        if (current == cache->tail) {
            cache->tail = prev;
        }
        if (current->next) {
            current->next->prev = prev;
        }
        
        cache->total_bytes -= (current->data_len + sizeof(struct cached_packet));
        cache->count--;
        
        free(current->app_data);
        free(current);
    }
    
    // 插入新报文
    new_packet->next = current;
    new_packet->prev = prev;
    
    if (prev) {
        prev->next = new_packet;
    } else {
        cache->head = new_packet;
    }
    
    if (current) {
        current->prev = new_packet;
    } else {
        cache->tail = new_packet;
    }
    
    cache->count++;
    cache->total_bytes += (new_packet->data_len + sizeof(struct cached_packet));
    
    // 更新最后活动时间
    gettimeofday(&cache->last_activity, NULL);
    
    pthread_mutex_unlock(&cache->lock);
    
    return 0;
}

// 添加报文到对应的连接缓存
int add_to_connection_cache(const char *src_ip, const char *dst_ip,
                           uint16_t src_port, uint16_t dst_port,
                           uint8_t service_type, uint16_t pkey,
                           uint32_t dest_qp, uint32_t psn,
                           const unsigned char *app_data, int data_len)
{
    if (!g_cache_mgr) {
        fprintf(stderr, "缓存管理器未初始化\n");
        return -1;
    }
    
    // 创建连接键
    struct connection_key key = create_connection_key(src_ip, dst_ip, src_port, 
                                                     dst_port, service_type, pkey);
    
    // 获取或创建连接缓存
    struct connection_cache *conn_cache = get_or_create_connection_cache(g_cache_mgr, &key);
    if (!conn_cache) {
        return -1;
    }
    
    // 创建缓存报文
    struct cached_packet *packet = malloc(sizeof(struct cached_packet));
    if (!packet) {
        perror("malloc cached_packet");
        return -1;
    }
    
    packet->app_data = malloc(data_len);
    if (!packet->app_data) {
        perror("malloc app_data");
        free(packet);
        return -1;
    }
    
    memcpy(packet->app_data, app_data, data_len);
    packet->data_len = data_len;
    packet->dest_qp = dest_qp;
    packet->psn = psn;
    gettimeofday(&packet->timestamp, NULL);
    packet->next = NULL;
    packet->prev = NULL;
    
    // 按PSN顺序插入
    if (insert_packet_sorted(conn_cache, packet) != 0) {
        free(packet->app_data);
        free(packet);
        return -1;
    }
    
    printf("成功缓存报文: ");
    print_connection_key(&key);
    printf(", QP=%u, PSN=%u, 大小=%d bytes\n", dest_qp, psn, data_len);
    
    return 0;
}

// 根据连接键和PSN范围查找报文
struct cached_packet* find_packets_by_psn_range(const struct connection_key *key,
                                               uint32_t start_psn, uint32_t end_psn,
                                               int *found_count)
{
    if (!g_cache_mgr || !found_count) {
        return NULL;
    }
    
    uint32_t hash_index = calculate_hash(key, g_cache_mgr->hash_table_size);
    struct cached_packet *result_head = NULL;
    struct cached_packet *result_tail = NULL;
    int count = 0;
    
    pthread_mutex_lock(&g_cache_mgr->global_lock);
    
    // 查找连接
    struct hash_table_entry *entry = g_cache_mgr->hash_table[hash_index];
    while (entry) {
        if (connection_keys_equal(&entry->key, key)) {
            struct connection_cache *cache = entry->cache;
            
            pthread_mutex_lock(&cache->lock);
            
            // 在有序链表中查找PSN范围内的报文
            struct cached_packet *current = cache->head;
            while (current && current->psn <= end_psn) {
                if (current->psn >= start_psn) {
                    // TODO: 查找到满足条件的节点后，原始节点不要摘链
                    // 因为本次重传可能还失败，后面还需要重传。
                    // 所有报文都接收到后会收到ACK，收到ACK后时再摘链
                    // 从原链表中移除
                    if (current->prev) {
                        current->prev->next = current->next;
                    } else {
                        cache->head = current->next;
                    }
                    if (current->next) {
                        current->next->prev = current->prev;
                    } else {
                        cache->tail = current->prev;
                    }
                    
                    // TODO: 原始报文不摘链的情况下，当前节点不能直接挂在其他链表
                    // 添加到结果链表
                    struct cached_packet *matched = current;
                    current = current->next;
                    
                    matched->next = NULL;
                    matched->prev = NULL;
                    
                    if (result_tail) {
                        result_tail->next = matched;
                        matched->prev = result_tail;
                        result_tail = matched;
                    } else {
                        result_head = matched;
                        result_tail = matched;
                    }
                    
                    cache->count--;
                    cache->total_bytes -= (matched->data_len + sizeof(struct cached_packet));
                    count++;
                } else {
                    current = current->next;
                }
            }
            
            pthread_mutex_unlock(&cache->lock);
            break;
        }
        entry = entry->next;
    }
    
    pthread_mutex_unlock(&g_cache_mgr->global_lock);
    
    *found_count = count;
    return result_head;
}

// 获取连接的PSN范围
int get_connection_psn_range(const struct connection_key *key,
                            uint32_t *min_psn, uint32_t *max_psn)
{
    if (!g_cache_mgr || !min_psn || !max_psn) {
        return -1;
    }
    
    uint32_t hash_index = calculate_hash(key, g_cache_mgr->hash_table_size);
    int found = 0;
    
    pthread_mutex_lock(&g_cache_mgr->global_lock);
    
    struct hash_table_entry *entry = g_cache_mgr->hash_table[hash_index];
    while (entry) {
        if (connection_keys_equal(&entry->key, key)) {
            pthread_mutex_lock(&entry->cache->lock);
            *min_psn = entry->cache->min_psn;
            *max_psn = entry->cache->max_psn;
            pthread_mutex_unlock(&entry->cache->lock);
            found = 1;
            break;
        }
        entry = entry->next;
    }
    
    pthread_mutex_unlock(&g_cache_mgr->global_lock);
    
    return found ? 0 : -1;
}

// NACK触发GBN批量重传
void handle_nack_batch_retransmit(const struct connection_key *key,
                                  uint32_t nack_epsn)
{
    if (!g_cache_mgr || !found_count) {
        return NULL;
    }

    uint32_t hash_index = calculate_hash(key, g_cache_mgr->hash_table_size);
    
    pthread_mutex_lock(&g_cache_mgr->global_lock);
    
    // 查找连接
    struct hash_table_entry *entry = g_cache_mgr->hash_table[hash_index];
    while (entry) {
        if (connection_keys_equal(&entry->key, key)) {
            struct connection_cache *cache = entry->cache;
            pthread_mutex_lock(&cache->lock);

            // 在有序链表中查找PSN范围内的报文
            struct cached_packet *current = cache->head;
            while (current) {
                if (current->psn >= nack_epsn) {
                    // 通过AF_PACKET修改源MAC方式重传缓存的报文
                    retransmit_rdma_packet(key, current->app_data, current->data_len, 
                                           current->dest_qp, current->psn);
                } else {
                    // TODO: 摘除已经收到报文缓存节点
                    // 如果本次重传仍未收到，后面再次触发重传时前面已收到报文节点就不用重复遍历
                }
                current = current->next;
            }
            pthread_mutex_unlock(&cache->lock);
            break;
        }
        entry = entry->next;
    }
    
    pthread_mutex_unlock(&g_cache_mgr->global_lock);
}

// 连接老化和清理
// 清理过期连接
void cleanup_expired_connections()
{
    if (!g_cache_mgr)
        return;
    
    struct timeval now;
    gettimeofday(&now, NULL);
    
    pthread_mutex_lock(&g_cache_mgr->global_lock);
    
    for (size_t i = 0; i < g_cache_mgr->hash_table_size; i++) {
        struct hash_table_entry **entry_ptr = &g_cache_mgr->hash_table[i];
        while (*entry_ptr) {
            struct hash_table_entry *entry = *entry_ptr;
            struct connection_cache *cache = entry->cache;
            
            pthread_mutex_lock(&cache->lock);
            
            long idle_seconds = now.tv_sec - cache->last_activity.tv_sec;
            
            if (idle_seconds > g_cache_mgr->connection_timeout || cache->count == 0) {
                // 移除过期连接
                printf("清理过期连接: ");
                print_connection_key(&entry->key);
                printf(", 空闲时间=%ld秒, 缓存报文=%zu\n", 
                       idle_seconds, cache->count);
                
                *entry_ptr = entry->next;
                g_cache_mgr->total_connections--;
                
                pthread_mutex_unlock(&cache->lock);
                destroy_connection_cache(cache);
                free(entry);
            } else {
                pthread_mutex_unlock(&cache->lock);
                entry_ptr = &(*entry_ptr)->next;
            }
        }
    }
    
    pthread_mutex_unlock(&g_cache_mgr->global_lock);
}

// 打印所有连接状态
void print_all_connections_status() {
    if (!g_cache_mgr) return;
    
    printf("=== 所有连接状态 ===\n");
    printf("总连接数: %zu/%zu\n", g_cache_mgr->total_connections, g_cache_mgr->max_connections);
    
    pthread_mutex_lock(&g_cache_mgr->global_lock);
    
    for (size_t i = 0; i < g_cache_mgr->hash_table_size; i++) {
        struct hash_table_entry *entry = g_cache_mgr->hash_table[i];
        while (entry) {
            struct connection_cache *cache = entry->cache;
            
            pthread_mutex_lock(&cache->lock);
            
            struct timeval now;
            gettimeofday(&now, NULL);
            long idle_seconds = now.tv_sec - cache->last_activity.tv_sec;
            
            printf("  ");
            print_connection_key(&entry->key);
            printf(", 报文数=%zu, PSN范围=[%u, %u], 空闲=%ld秒\n",
                   cache->count, cache->min_psn, cache->max_psn, idle_seconds);
            
            pthread_mutex_unlock(&cache->lock);
            entry = entry->next;
        }
    }
    
    pthread_mutex_unlock(&g_cache_mgr->global_lock);
    printf("===================\n");
}

// 主函数初始化
int main() {
    // 初始化缓存管理器：哈希表大小1024，最大连接数1000，每连接最大1000报文，100MB内存，超时300秒
    g_cache_mgr = init_cache_manager(1024, 1000, 1000, 100, 300);
    if (!g_cache_mgr) {
        return 1;
    }
    
    // ... 其他初始化代码 ...
    
    // 创建2个线程：报文接收&缓存线程，连接老化线程
    // 定期清理任务
    while (1) {
        sleep(60); // 每分钟清理一次
        cleanup_expired_connections();
        print_all_connections_status();
    }
    
    return 0;
}

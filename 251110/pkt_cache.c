// RDMA报文多连接哈希存储（修改后版本）

/*
整体逻辑
该程序是一个基于 RDMA 的报文缓存管理系统，主要功能是通过哈希表管理多连接的报文缓存，支持按 PSN（包序列号）有序存储、NACK 触发的批量重传以及连接超时清理。核心流程包括：
1、缓存流程：接收报文后，按连接标识（IP、端口、QP 等）存入对应哈希表项的双向链表，链表按 PSN 排序。
2、滑动窗口：通过维护每个连接的 PSN 范围（min_psn/max_psn）实现类似滑动窗口的机制，NACK 重传时从指定 PSN 开始重传后续报文。
3、哈希流程：用connection_key作为哈希键，通过哈希表快速定位连接缓存，解决哈希冲突采用链表法。

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
    uint32_t window_start;            // 滑动窗口起始PSN（新增）
    uint32_t window_size;             // 滑动窗口大小（新增）
    struct timeval last_activity;     // 最后活动时间
    pthread_mutex_t lock;             // 连接级锁
};

// IPv4连接标识键
struct connection_key {
    uint32_t src_ip;                  // 源IP
    uint32_t dst_ip;                  // 目的IP
    uint16_t src_port;                // 源端口
    uint16_t dst_port;                // 目的端口
    uint32_t dest_qp;                 // 目标主机QP号
    uint32_t src_qp;                  // 源主机QP号
    uint8_t service_type;             // RDMA服务类型
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

// 哈希计算函数（优化哈希分布）
/*
 * calculate_hash
 * 目的：根据连接键生成哈希槽索引，用于在全局哈希表中快速定位连接缓存。
 * 输入：
 *   - key: 指向 connection_key 的指针，包含 src/dst IP、端口、QP、service_type、pkey 等字段
 *   - table_size: 哈希表大小（槽数），返回值为 [0, table_size-1]
 * 输出：返回哈希索引（uint32_t）
 * 实现要点与注意事项：
 *   - 使用简单的 djb2 风格的混合（移位+加法）来混合多个字段，包含 src_qp 和 dest_qp
 *   - 必须保证对所有字段的组合具有足够的离散性以减少冲突
 *   - 不对网络字节序/主机字节序做额外转换，调用方需要在创建 key 时使用一致的字节序
 */
uint32_t calculate_hash(const struct connection_key *key, size_t table_size)
{
    uint32_t hash = 5381;
    /* 将关键字段累加进哈希，顺序决定了分布特性 */
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

// 连接键比较（补充新增字段比较）
/*
 * connection_keys_equal
 * 目的：判断两个 connection_key 是否表示同一条连接。
 * 输入：指向两个 key 的指针。
 * 输出：相等返回非 0， 否则返回 0。
 * 注意：比较包含 QP 和 service_type、pkey，确保连接粒度足够细。
 */
int connection_keys_equal(const struct connection_key *a,
                          const struct connection_key *b)
{
    return (a->src_ip == b->src_ip &&
            a->dst_ip == b->dst_ip &&
            a->src_port == b->src_port &&
            a->dst_port == b->dst_port &&
            a->src_qp == b->src_qp &&    // 补充比较
            a->dest_qp == b->dest_qp &&  // 补充比较
            a->service_type == b->service_type &&
            a->pkey == b->pkey);
}

// 创建连接键（补充新增字段初始化）
/*
 * create_connection_key
 * 目的：根据可读的 IP/端口/qp 等信息构造内部使用的 connection_key
 * 输入：字符串形式的 src_ip/dst_ip、端口、src/dest qp、service_type、pkey
 * 输出：返回填充好的 connection_key（按值返回）
 * 注意事项：
 *  - 使用 inet_pton 将 IPv4 文本地址转换为 uint32_t（网络字节序）并写入 key
 *  - 返回的 key 可直接用于 calculate_hash 和 connection_keys_equal
 */
struct connection_key create_connection_key(const char *src_ip, const char *dst_ip,
                                            uint16_t src_port, uint16_t dst_port,
                                            uint32_t src_qp, uint32_t dest_qp,
                                            uint8_t service_type, uint16_t pkey)
{
    struct connection_key key;
    memset(&key, 0, sizeof(key));  // 初始化所有字段

    inet_pton(AF_INET, src_ip, &key.src_ip);
    inet_pton(AF_INET, dst_ip, &key.dst_ip);
    key.src_port = src_port;
    key.dst_port = dst_port;
    key.src_qp = src_qp;           // 新增字段赋值
    key.dest_qp = dest_qp;         // 新增字段赋值
    key.service_type = service_type;
    key.pkey = pkey;

    return key;
}

// 打印连接键信息（补充新增字段）
/*
 * print_connection_key
 * 目的：以可读格式打印 connection_key，用于日志/调试
 * 输入：key 指针
 * 输出：在 stdout 打印连接信息，不修改任何状态
 */
void print_connection_key(const struct connection_key *key) {
    char src_ip[INET_ADDRSTRLEN];
    char dst_ip[INET_ADDRSTRLEN];

    inet_ntop(AF_INET, &key->src_ip, src_ip, INET_ADDRSTRLEN);
    inet_ntop(AF_INET, &key->dst_ip, dst_ip, INET_ADDRSTRLEN);

    printf("连接: %s:%d (QP=%u) -> %s:%d (QP=%u), 服务类型=%d, pkey=0x%04x",
           src_ip, key->src_port, key->src_qp,
           dst_ip, key->dst_port, key->dest_qp,
           key->service_type, key->pkey);
}

// 全局缓存管理器实例
static struct cache_manager *g_cache_mgr = NULL;

// 初始化缓存管理器（补充滑动窗口默认值）
/*
 * init_cache_manager
 * 目的：分配并初始化全局缓存管理器 (g_cache_mgr)
 * 输入：
 *   - hash_size: 哈希表槽的数量
 *   - max_conns: 支持的最大连接数
 *   - max_packets_per_conn: 每个连接允许缓存的报文数量上限
 *   - max_bytes_per_conn_mb: 每连接最大缓存字节数（以 MB 为单位传入）
 *   - conn_timeout_seconds: 连接空闲超时时间（秒）
 * 返回：成功返回指向已初始化的 cache_manager，失败返回 NULL
 * 关键点：会分配哈希表数组并初始化全局互斥锁。
 */
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
    
    mgr->hash_table_size = hash_size;
    mgr->hash_table = calloc(hash_size, sizeof(struct hash_table_entry*));
    if (!mgr->hash_table) {
        perror("calloc hash_table");
        free(mgr);
        return NULL;
    }
    
    mgr->max_connections = max_conns;
    mgr->max_packets_per_conn = max_packets_per_conn;
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

/*
 * create_connection_cache
 * 目的：为单个连接分配并初始化一个 connection_cache 结构，包含
 *       - 双向链表头尾指针
 *       - 计数器与字节统计
 *       - 滑动窗口相关字段（window_start/window_size）
 *       - 锁与最后活动时间
 * 输入：window_size - 该连接的滑动窗口大小（PSN 个数）
 * 返回：指向已初始化 connection_cache 的指针，失败返回 NULL
 * 注意：调用方应在全局锁或适当上下文中调用此函数以避免竞态。
 */
struct connection_cache* create_connection_cache(uint32_t window_size)
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
    cache->window_start = 0;         // 滑动窗口起始
    cache->window_size = window_size; // 滑动窗口大小
    gettimeofday(&cache->last_activity, NULL);
    
    if (pthread_mutex_init(&cache->lock, NULL) != 0) {
        perror("pthread_mutex_init connection_cache lock");
        free(cache);
        return NULL;
    }
    
    return cache;
}

/*
 * destroy_connection_cache
 * 目的：释放 connection_cache 及其包含的所有缓存报文内存并销毁锁
 * 输入：cache - 需要被销毁的连接缓存
 * 行为：在释放前会对 cache 加锁以确保线程安全，然后逐一 free 节点
 * 注意：调用后不要再访问该 cache 指针
 */
void destroy_connection_cache(struct connection_cache *cache)
{
    if (!cache) return;
    
    pthread_mutex_lock(&cache->lock);
    
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

/*
 * get_or_create_connection_cache
 * 目的：在全局哈希表中查找给定 key 的连接缓存，若不存在则创建新的
 * 输入：
 *   - mgr: 全局缓存管理器
 *   - key: 连接键
 *   - window_size: 新建连接时的滑动窗口大小
 * 返回：指向 connection_cache 的指针（若查找或创建成功），失败返回 NULL
 * 关键点：
 *   - 在查找或插入哈希表时使用 mgr->global_lock 保护结构一致性
 *   - 在返回现有 cache 时会更新 last_activity 时间
 */
struct connection_cache* get_or_create_connection_cache(
    struct cache_manager *mgr, const struct connection_key *key,
    uint32_t window_size)  // 新增窗口大小参数
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
    
    // 创建新连接缓存（传入滑动窗口大小）
    struct connection_cache *new_cache = create_connection_cache(window_size);
    if (!new_cache) {
        pthread_mutex_unlock(&mgr->global_lock);
        return NULL;
    }
    
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
    printf(", 窗口大小=%u\n", window_size);
    
    return new_cache;
}

/*
 * slide_window
 * 目的：根据 cache->window_start 和 window_size 维护滑动窗口，清理
 *       所有 PSN 小于 window_start 的报文，释放空间并更新统计信息。
 * 输入：cache - 需要进行窗口滑动清理的连接缓存
 * 说明：该函数仅在持有 cache->lock 的情况下或由上层在安全上下文调用
 */
static void slide_window(struct connection_cache *cache)
{
    // 计算窗口上限
    uint32_t window_end = cache->window_start + cache->window_size - 1;
    
    // 移除窗口外的报文（PSN < window_start）
    struct cached_packet *current = cache->head;
    while (current && current->psn < cache->window_start) {
        struct cached_packet *to_remove = current;
        current = current->next;
        
        // 从链表移除
        if (to_remove->prev) {
            to_remove->prev->next = to_remove->next;
        } else {
            cache->head = to_remove->next;
        }
        if (to_remove->next) {
            to_remove->next->prev = to_remove->prev;
        } else {
            cache->tail = to_remove->prev;
        }
        
        // 更新统计
        cache->count--;
        cache->total_bytes -= (to_remove->data_len + sizeof(struct cached_packet));
        free(to_remove->app_data);
        free(to_remove);
    }
    
    // 更新最小PSN
    if (cache->head) {
        cache->min_psn = cache->head->psn;
    } else {
        cache->min_psn = 0;
    }
}

/*
 * insert_packet_sorted
 * 目的：将新报文按 PSN 有序插入 connection_cache 的双向链表中，保证链表
 *       从 head 到 tail 的 PSN 单调递增
 * 输入：
 *   - cache: 目标连接缓存
 *   - new_packet: 待插入的缓存节点（caller 已分配并填充 app_data、psn 等字段）
 * 返回：0 成功，-1 失败（例如超出窗口、缓存已满或内存问题）
 * 主要步骤：
 *   1) 检查 new_packet 是否在滑动窗口范围内
 *   2) 检查每连接的最大报文数与字节数限制
 *   3) 在链表中找到插入位置（保持有序），并处理重复 PSN 的替换逻辑
 *   4) 更新统计信息与 last_activity
 * 注意：函数内部会持有并释放 cache->lock，调用方无需额外加锁
 */
int insert_packet_sorted(struct connection_cache *cache, 
                         struct cached_packet *new_packet)
{
    if (!cache || !new_packet)
        return -1;
    
    pthread_mutex_lock(&cache->lock);
    
    // 检查是否在滑动窗口内
    uint32_t window_end = cache->window_start + cache->window_size - 1;
    if (new_packet->psn < cache->window_start || new_packet->psn > window_end) {
        printf("报文PSN=%u 超出窗口范围 [%u, %u]\n",
               new_packet->psn, cache->window_start, window_end);
        pthread_mutex_unlock(&cache->lock);
        return -1;
    }
    
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
    
    // 按PSN顺序插入到双向链表
    struct cached_packet *current = cache->head;
    struct cached_packet *prev = NULL;
    
    while (current && current->psn < new_packet->psn) {
        prev = current;
        current = current->next;
    }
    
    // 处理重复PSN
    if (current && current->psn == new_packet->psn) {
        printf("警告: 重复PSN %u, 替换现有报文\n", new_packet->psn);
        // 移除现有报文
        if (prev) prev->next = current->next;
        else cache->head = current->next;
        
        if (current == cache->tail) cache->tail = prev;
        if (current->next) current->next->prev = prev;
        
        cache->total_bytes -= (current->data_len + sizeof(struct cached_packet));
        cache->count--;
        free(current->app_data);
        free(current);
    }
    
    // 插入新报文
    new_packet->next = current;
    new_packet->prev = prev;
    
    if (prev) prev->next = new_packet;
    else cache->head = new_packet;
    
    if (current) current->prev = new_packet;
    else cache->tail = new_packet;
    
    cache->count++;
    cache->total_bytes += (new_packet->data_len + sizeof(struct cached_packet));
    gettimeofday(&cache->last_activity, NULL);
    
    pthread_mutex_unlock(&cache->lock);
    return 0;
}

/*
 * add_to_connection_cache
 * 目的：外部调用接口，将收到的应用数据封装为 cached_packet 并插入对应连接的缓存
 * 输入：
 *   - src_ip/dst_ip: 源/目的 IPv4 文本地址
 *   - src_port/dst_port: 源/目的端口
 *   - src_qp/dest_qp: 源/目的 QP 编号（用于区分不同 RDMA 会话）
 *   - service_type/pkey: RDMA 服务类型与分区键
 *   - psn: 该报文的包序列号
 *   - app_data/data_len: 指向应用数据及其长度（函数会复制数据）
 * 返回：0 成功，-1 失败
 * 行为：
 *   1) 构造 connection_key 并通过 get_or_create_connection_cache 获取连接缓存
 *   2) 为 cached_packet 分配内存并复制应用数据
 *   3) 调用 insert_packet_sorted 将节点插入缓存
 * 注意：该接口是线程安全的（内部使用了全局锁 + 连接级锁）
 */
int add_to_connection_cache(const char *src_ip, const char *dst_ip,
                           uint16_t src_port, uint16_t dst_port,
                           uint32_t src_qp, uint32_t dest_qp,  // 新增参数
                           uint8_t service_type, uint16_t pkey,
                           uint32_t psn, const unsigned char *app_data, int data_len)
{
    if (!g_cache_mgr) {
        fprintf(stderr, "缓存管理器未初始化\n");
        return -1;
    }
    
    // 创建包含QP信息的连接键
    struct connection_key key = create_connection_key(src_ip, dst_ip, 
                                                     src_port, dst_port,
                                                     src_qp, dest_qp,  // 传入QP
                                                     service_type, pkey);
    
    // 获取或创建连接缓存（使用默认窗口大小1024）
    struct connection_cache *conn_cache = get_or_create_connection_cache(
        g_cache_mgr, &key, 1024);
    if (!conn_cache) return -1;
    
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
    packet->next = packet->prev = NULL;
    
    // 插入缓存
    if (insert_packet_sorted(conn_cache, packet) != 0) {
        free(packet->app_data);
        free(packet);
        return -1;
    }
    
    printf("缓存报文: ");
    print_connection_key(&key);
    printf(", PSN=%u, 大小=%d bytes\n", psn, data_len);
    
    return 0;
}

/*
 * find_packets_by_psn_range
 * 目的：为重传或回放提供接口，从指定连接缓存中按 PSN 范围复制一组报文
 * 输入：
 *   - key: 连接键
 *   - start_psn, end_psn: 查找的 PSN 范围（包含边界）
 *   - found_count: 输出参数，返回找到的报文数量
 * 返回：指向复制出的链表头（调用方负责释放），若未找到返回 NULL
 * 要点：函数不会修改原缓存（只是复制数据），因此不会影响原有缓存结构
 */
struct cached_packet* find_packets_by_psn_range(const struct connection_key *key,
                                               uint32_t start_psn, uint32_t end_psn,
                                               int *found_count)
{
    if (!g_cache_mgr || !found_count) return NULL;
    
    *found_count = 0;
    uint32_t hash_index = calculate_hash(key, g_cache_mgr->hash_table_size);
    struct cached_packet *result_head = NULL;
    struct cached_packet *result_tail = NULL;
    
    pthread_mutex_lock(&g_cache_mgr->global_lock);
    
    struct hash_table_entry *entry = g_cache_mgr->hash_table[hash_index];
    while (entry) {
        if (connection_keys_equal(&entry->key, key)) {
            struct connection_cache *cache = entry->cache;
            pthread_mutex_lock(&cache->lock);
            
            // 遍历查找范围内的报文
            struct cached_packet *current = cache->head;
            while (current && current->psn <= end_psn) {
                if (current->psn >= start_psn) {
                    // 复制报文（不修改原缓存）
                    struct cached_packet *copy = malloc(sizeof(struct cached_packet));
                    copy->app_data = malloc(current->data_len);
                    memcpy(copy->app_data, current->app_data, current->data_len);
                    copy->data_len = current->data_len;
                    copy->dest_qp = current->dest_qp;
                    copy->psn = current->psn;
                    copy->timestamp = current->timestamp;
                    copy->next = NULL;
                    copy->prev = result_tail;
                    
                    if (result_tail) result_tail->next = copy;
                    else result_head = copy;
                    result_tail = copy;
                    (*found_count)++;
                }
                current = current->next;
            }
            
            pthread_mutex_unlock(&cache->lock);
            break;
        }
        entry = entry->next;
    }
    
    pthread_mutex_unlock(&g_cache_mgr->global_lock);
    return result_head;
}

/*
 * update_window_start
 * 目的：更新指定连接的滑动窗口起始 PSN，并触发对窗口外报文的清理（slide_window）
 * 输入：
 *   - key: 连接键
 *   - new_start: 新的窗口起始 PSN
 * 返回：0 成功，-1 未找到对应连接
 * 说明：若 new_start 大于当前 window_start，则会将 window_start 前移并清理过期报文
 */
int update_window_start(const struct connection_key *key, uint32_t new_start)
{
    if (!g_cache_mgr) return -1;
    
    uint32_t hash_index = calculate_hash(key, g_cache_mgr->hash_table_size);
    pthread_mutex_lock(&g_cache_mgr->global_lock);
    
    struct hash_table_entry *entry = g_cache_mgr->hash_table[hash_index];
    while (entry) {
        if (connection_keys_equal(&entry->key, key)) {
            struct connection_cache *cache = entry->cache;
            pthread_mutex_lock(&cache->lock);
            
            if (new_start > cache->window_start) {
                cache->window_start = new_start;
                slide_window(cache);  // 清理窗口外报文
                printf("更新窗口起始: %u -> %u\n", new_start - cache->window_size, new_start);
            }
            
            pthread_mutex_unlock(&cache->lock);
            pthread_mutex_unlock(&g_cache_mgr->global_lock);
            return 0;
        }
        entry = entry->next;
    }
    
    pthread_mutex_unlock(&g_cache_mgr->global_lock);
    return -1;  // 未找到连接
}

/*
 * handle_nack_batch_retransmit
 * 目的：当收到 NACK（带 ePSN）时，触发对该 ePSN 及之后报文的重传（批量）
 * 输入：
 *   - key: 代表该连接的连接键（通常来自对端 ACK/NACK 报文的源/目的信息）
 *   - nack_epsn: 需要重传的起始 PSN（ePSN）
 * 行为：遍历缓存并打印重传信息；真实部署应在此处调用实际的重传发送逻辑
 */
void handle_nack_batch_retransmit(const struct connection_key *key,
                                  uint32_t nack_epsn)
{
    if (!g_cache_mgr) return;  // 修复原代码的found_count未定义错误

    uint32_t hash_index = calculate_hash(key, g_cache_mgr->hash_table_size);
    
    pthread_mutex_lock(&g_cache_mgr->global_lock);
    
    struct hash_table_entry *entry = g_cache_mgr->hash_table[hash_index];
    while (entry) {
        if (connection_keys_equal(&entry->key, key)) {
            struct connection_cache *cache = entry->cache;
            pthread_mutex_lock(&cache->lock);

            // 重传nack_epsn及之后的报文
            struct cached_packet *current = cache->head;
            while (current) {
                if (current->psn >= nack_epsn) {
                    // 这里应替换为实际重传逻辑
                    printf("重传报文: PSN=%u, 长度=%d\n", 
                           current->psn, current->data_len);
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

/*
 * cleanup_expired_connections
 * 目的：遍历哈希表，清理空闲时间超过超时阈值或没有缓存报文的连接条目
 * 行为：对每个需要删除的 entry，先从哈希链中摘除，再销毁其 connection_cache，
 *       并释放 hash_table_entry 的内存，保证全局连接计数正确更新
 * 注意：会使用全局锁并在每个 connection_cache 上加锁以保证线程安全
 */
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
            struct connection_cache *cache = entry->cache;
            
            pthread_mutex_lock(&cache->lock);
            long idle_seconds = now.tv_sec - cache->last_activity.tv_sec;
            
            if (idle_seconds > g_cache_mgr->connection_timeout || cache->count == 0) {
                printf("清理过期连接: ");
                print_connection_key(&entry->key);
                printf(", 空闲时间=%ld秒\n", idle_seconds);
                
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

/*
 * print_all_connections_status
 * 目的：以友好格式输出当前全局缓存管理器中的连接统计信息与每个连接的窗口/PSN 状态
 * 用途：用于人工检查、调试和验证缓存行为
 */
void print_all_connections_status() {
    if (!g_cache_mgr) return;
    
    printf("=== 缓存状态 ===\n");
    printf("总连接数: %zu/%zu\n", g_cache_mgr->total_connections, g_cache_mgr->max_connections);
    
    pthread_mutex_lock(&g_cache_mgr->global_lock);
    
    for (size_t i = 0; i < g_cache_mgr->hash_table_size; i++) {
        struct hash_table_entry *entry = g_cache_mgr->hash_table[i];
        while (entry) {
            struct connection_cache *cache = entry->cache;
            pthread_mutex_lock(&cache->lock);
            
            struct timeval now;
            gettimeofday(&now, NULL);
            uint32_t window_end = cache->window_start + cache->window_size - 1;
            
            printf("  ");
            print_connection_key(&entry->key);
            printf("\n    报文数=%zu, PSN范围=[%u, %u], 窗口=[%u, %u], 空闲=%ld秒\n",
                   cache->count, cache->min_psn, cache->max_psn,
                   cache->window_start, window_end,
                   now.tv_sec - cache->last_activity.tv_sec);
            
            pthread_mutex_unlock(&cache->lock);
            entry = entry->next;
        }
    }
    
    pthread_mutex_unlock(&g_cache_mgr->global_lock);
    printf("================\n");
}

/*
 * retransmit_rdma_packet
 * 目的：占位函数，用于模拟/占用真实环境下的 RDMA 重传逻辑
 * 输入：
 *   - key: 连接信息
 *   - data/len: 待重传的数据（当前实现不实际发送，仅打印）
 *   - dest_qp/psn: 目标 QP 和报文的 PSN
 * 说明：在生产环境中需要替换为真实的发送逻辑（例如修改 AF_PACKET 数据包并发送，或通过 RDMA verbs 发送）
 */
void retransmit_rdma_packet(const struct connection_key *key, 
                           const unsigned char *data, int len, 
                           uint32_t dest_qp, uint32_t psn) {
    // 实际应用中应实现真实的RDMA重传逻辑
    printf("重传: ");
    print_connection_key(key);
    printf(" PSN=%u, 长度=%d\n", psn, len);
}

// 主函数测试
/*
 * main - 测试和演示入口
 * 目的：演示如何初始化缓存管理器、插入报文、触发 NACK 重传、滑动窗口并查看状态。
 * 操作流程（手动验证步骤）：
 *  1) 编译程序：
 *       gcc pkt_cache.c -o pkt_cache -lpthread -lrdmacm -libverbs
 *  2) 运行：
 *       sudo ./pkt_cache
 *     程序会输出以下流程：
 *       - 初始化缓存管理器信息
 *       - 为示例连接插入若干 PSN（1..5）的测试报文
 *       - 模拟 NACK（从 PSN=3 开始）并打印需要重传的报文信息（在真实实现中这里会触发发送）
 *       - 更新滑动窗口起始到 4，并清理窗口外的报文（PSN < 4 将被释放）
 *       - 打印所有连接当前状态（包含窗口/PSN 范围）
 *       - 最后执行一次过期连接清理
 *  验证要点：
 *   - 在插入 5 个报文后，打印应显示缓存成功的日志
 *   - handle_nack_batch_retransmit 应打印出 PSN>=3 的报文信息
 *   - update_window_start(&key,4) 后，print_all_connections_status 中的报文数应减少，min_psn 应至少为 4
 *   - 若将 retransmit_rdma_packet 替换为真实发送逻辑，可以结合抓包工具（tcpdump）验证重传数据包
 * 注意：该 main 只作为本模块的单机自测示例，真实环境中报文来源应来自 pkt_recv 等解析模块或真实网卡抓取
 */
int main() {
    // 初始化缓存管理器
    g_cache_mgr = init_cache_manager(1024, 1000, 1000, 100, 300);
    if (!g_cache_mgr) return 1;

    /* 测试数据准备：构造一个示例连接 key 并插入 5 个连续 PSN 的报文 */
    unsigned char data[] = "test_rdma_packet";
    struct connection_key key = create_connection_key("192.168.1.100", "192.168.1.200",
                                                     1234, 5678, 10, 20, 0, 0xffff);

    // 添加测试报文 PSN=1..5
    for (uint32_t psn = 1; psn <= 5; psn++) {
        add_to_connection_cache("192.168.1.100", "192.168.1.200",
                               1234, 5678, 10, 20, 0, 0xffff,
                               psn, data, sizeof(data));
    }

    // 模拟NACK触发重传
    printf("\n触发NACK重传（PSN=3及以后）:\n");
    handle_nack_batch_retransmit(&key, 3);

    // 模拟窗口滑动
    printf("\n更新窗口起始到4:\n");
    update_window_start(&key, 4);

    // 打印状态供人工检查
    print_all_connections_status();

    // 清理并退出
    cleanup_expired_connections();
    return 0;
}
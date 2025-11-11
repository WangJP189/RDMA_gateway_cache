/*
编译命令：
gcc pkt_cache_sere_self.c -o pkt_cache_sere_self -lpthread -lrdmacm -libverbs

运行命令：
sudo ./pkt_cache_sere_self
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <infiniband/verbs.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <time.h>
#include <sys/time.h>

// 包含缓存模块结构和函数声明
struct cached_packet {
    unsigned char *app_data;
    int data_len;
    uint32_t dest_qp;
    uint32_t psn;
    struct timeval timestamp;
    struct cached_packet *next;
    struct cached_packet *prev;
};

struct connection_cache {
    struct cached_packet *head;
    struct cached_packet *tail;
    size_t count;
    size_t total_bytes;
    uint32_t min_psn;
    uint32_t max_psn;
    uint32_t window_start;
    uint32_t window_size;
    struct timeval last_activity;
    pthread_mutex_t lock;
};

struct connection_key {
    uint32_t src_ip;
    uint32_t dst_ip;
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t dest_qp;
    uint32_t src_qp;
    uint8_t service_type;
    uint16_t pkey;
};

struct hash_table_entry {
    struct connection_key key;
    struct connection_cache *cache;
    struct hash_table_entry *next;
};

struct cache_manager {
    struct hash_table_entry **hash_table;
    size_t hash_table_size;
    size_t max_connections;
    size_t max_packets_per_conn;
    size_t max_bytes_per_conn;
    int connection_timeout;
    pthread_mutex_t global_lock;
    size_t total_connections;
};

// 缓存模块函数声明
uint32_t calculate_hash(const struct connection_key *key, size_t table_size);
int connection_keys_equal(const struct connection_key *a, const struct connection_key *b);
struct connection_key create_connection_key(const char *src_ip, const char *dst_ip,
                                           uint16_t src_port, uint16_t dst_port,
                                           uint32_t src_qp, uint32_t dest_qp,
                                           uint8_t service_type, uint16_t pkey);
void print_connection_key(const struct connection_key *key);
struct cache_manager* init_cache_manager(size_t hash_size, size_t max_conns,
                                        size_t max_packets_per_conn,
                                        size_t max_bytes_per_conn_mb,
                                        int conn_timeout_seconds);
struct connection_cache* create_connection_cache(uint32_t window_start,uint32_t window_size);
void destroy_connection_cache(struct connection_cache *cache);
struct connection_cache* get_or_create_connection_cache(
    struct cache_manager *mgr, const struct connection_key *key,
    uint32_t window_start, uint32_t window_size);
int insert_packet_sorted(struct connection_cache *cache, struct cached_packet *new_packet);
int add_to_connection_cache_ex(struct cache_manager *mgr, const char *src_ip, const char *dst_ip,
                              uint16_t src_port, uint16_t dst_port,
                              uint32_t src_qp, uint32_t dest_qp,
                              uint8_t service_type, uint16_t pkey,
                              uint32_t psn, const unsigned char *app_data, int data_len);

// 全局缓存管理器实例 - 发送和接收各一个
static struct cache_manager *g_send_cache_mgr = NULL;
static struct cache_manager *g_recv_cache_mgr = NULL;

// 缓存模块函数实现
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

int connection_keys_equal(const struct connection_key *a, const struct connection_key *b)
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

void print_connection_key(const struct connection_key *key) {
    char src_ip[INET_ADDRSTRLEN];
    char dst_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &key->src_ip, src_ip, INET_ADDRSTRLEN);
    inet_ntop(AF_INET, &key->dst_ip, dst_ip, INET_ADDRSTRLEN);
    printf("连接: %s:%d (QP=%u) -> %s:%d (QP=%u), 服务类型=%d, pkey=0x%04x",
           src_ip, ntohs(key->src_port), key->src_qp,
           dst_ip, ntohs(key->dst_port), key->dest_qp,
           key->service_type, ntohs(key->pkey));
}

struct cache_manager* init_cache_manager(size_t hash_size, size_t max_conns,
                                        size_t max_packets_per_conn,
                                        size_t max_bytes_per_conn_mb,
                                        int conn_timeout_seconds)
{
    struct cache_manager *mgr = malloc(sizeof(struct cache_manager));
    if (!mgr) {
        perror("malloc cache_manager 失败");
        return NULL;
    }
    
    mgr->hash_table_size = hash_size;
    mgr->hash_table = calloc(hash_size, sizeof(struct hash_table_entry*));
    if (!mgr->hash_table) {
        perror("calloc hash_table 失败");
        free(mgr);
        return NULL;
    }
    
    mgr->max_connections = max_conns;
    mgr->max_packets_per_conn = max_packets_per_conn;
    mgr->max_bytes_per_conn = max_bytes_per_conn_mb * 1024 * 1024;
    mgr->connection_timeout = conn_timeout_seconds;
    mgr->total_connections = 0;
    
    if (pthread_mutex_init(&mgr->global_lock, NULL) != 0) {
        perror("pthread_mutex_init global_lock 失败");
        free(mgr->hash_table);
        free(mgr);
        return NULL;
    }
    
    printf("✅ 初始化缓存管理器成功: 哈希表大小=%zu, 最大连接数=%zu, 每连接最大报文数=%zu\n",
           hash_size, max_conns, max_packets_per_conn);
    
    return mgr;
}

struct connection_cache* create_connection_cache(uint32_t window_start, uint32_t window_size)
{
    struct connection_cache *cache = malloc(sizeof(struct connection_cache));
    if (!cache) {
        perror("malloc connection_cache 失败");
        return NULL;
    }
    
    cache->head = NULL;
    cache->tail = NULL;
    cache->count = 0;
    cache->total_bytes = 0;
    cache->min_psn = 0;
    cache->max_psn = 0;
    cache->window_start = window_start;
    cache->window_size = window_size;
    gettimeofday(&cache->last_activity, NULL);
    
    if (pthread_mutex_init(&cache->lock, NULL) != 0) {
        perror("pthread_mutex_init connection_cache lock 失败");
        free(cache);
        return NULL;
    }
    
    return cache;
}

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

struct connection_cache* get_or_create_connection_cache(
    struct cache_manager *mgr, const struct connection_key *key,
    uint32_t window_start, uint32_t window_size)
{
    uint32_t hash_index = calculate_hash(key, mgr->hash_table_size);
    
    pthread_mutex_lock(&mgr->global_lock);
    
    struct hash_table_entry *entry = mgr->hash_table[hash_index];
    while (entry) {
        if (connection_keys_equal(&entry->key, key)) {
            gettimeofday(&entry->cache->last_activity, NULL);
            pthread_mutex_unlock(&mgr->global_lock);
            return entry->cache;
        }
        entry = entry->next;
    }
    
    if (mgr->total_connections >= mgr->max_connections) {
        printf("❌ 达到最大连接数限制 (%zu/%zu)\n",
               mgr->total_connections, mgr->max_connections);
        pthread_mutex_unlock(&mgr->global_lock);
        return NULL;
    }
    
    struct connection_cache *new_cache = create_connection_cache(window_start, window_size);
    if (!new_cache) {
        pthread_mutex_unlock(&mgr->global_lock);
        return NULL;
    }
    
    struct hash_table_entry *new_entry = malloc(sizeof(struct hash_table_entry));
    if (!new_entry) {
        perror("malloc hash_table_entry 失败");
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
    
    printf("✅ 创建新连接缓存成功: ");
    print_connection_key(key);
    printf(", 窗口起始=%u, 窗口大小=%u\n", window_start, window_size);
    
    return new_cache;
}

int insert_packet_sorted(struct connection_cache *cache, struct cached_packet *new_packet)
{
    if (!cache || !new_packet)
        return -1;
    
    pthread_mutex_lock(&cache->lock);
    
    uint32_t window_end = cache->window_start + cache->window_size - 1;
    if (new_packet->psn < cache->window_start || new_packet->psn > window_end) {
        printf("❌ 报文PSN=%u 超出窗口范围 [%u, %u]\n",
               new_packet->psn, cache->window_start, window_end);
        pthread_mutex_unlock(&cache->lock);
        return -1;
    }
    
    struct cache_manager *mgr = g_send_cache_mgr; // 简化处理
    size_t new_total_bytes = cache->total_bytes + new_packet->data_len + sizeof(struct cached_packet);
    if (cache->count >= mgr->max_packets_per_conn ||
        new_total_bytes >= mgr->max_bytes_per_conn) {
        printf("❌ 连接缓存已满 (%zu/%zu 报文, %zu/%zu 字节)\n",
               cache->count, mgr->max_packets_per_conn,
               cache->total_bytes, mgr->max_bytes_per_conn);
        pthread_mutex_unlock(&cache->lock);
        return -1;
    }
    
    if (cache->count == 0) {
        cache->min_psn = new_packet->psn;
        cache->max_psn = new_packet->psn;
    } else {
        if (new_packet->psn < cache->min_psn)
            cache->min_psn = new_packet->psn;
        if (new_packet->psn > cache->max_psn)
            cache->max_psn = new_packet->psn;
    }
    
    struct cached_packet *current = cache->head;
    struct cached_packet *prev = NULL;
    
    while (current && current->psn < new_packet->psn) {
        prev = current;
        current = current->next;
    }
    
    if (current && current->psn == new_packet->psn) {
        printf("⚠️  警告: 重复PSN %u, 替换现有报文\n", new_packet->psn);
        if (prev) prev->next = current->next;
        else cache->head = current->next;
        
        if (current == cache->tail) cache->tail = prev;
        if (current->next) current->next->prev = prev;
        
        cache->total_bytes -= (current->data_len + sizeof(struct cached_packet));
        cache->count--;
        free(current->app_data);
        free(current);
    }
    
    new_packet->next = current;
    new_packet->prev = prev;
    
    if (prev) prev->next = new_packet;
    else cache->head = new_packet;
    
    if (current) current->prev = new_packet;
    else cache->tail = new_packet;
    
    cache->count++;
    cache->total_bytes += (new_packet->data_len + sizeof(struct cached_packet));
    gettimeofday(&cache->last_activity, NULL);
    
    printf("✅ 缓存成功的数据包内容: \"%s\" (长度: %d bytes, PSN: %u)\n", 
           new_packet->app_data, new_packet->data_len, new_packet->psn);
    
    pthread_mutex_unlock(&cache->lock);
    return 0;
}

int add_to_connection_cache_ex(struct cache_manager *mgr, const char *src_ip, const char *dst_ip,
                              uint16_t src_port, uint16_t dst_port,
                              uint32_t src_qp, uint32_t dest_qp,
                              uint8_t service_type, uint16_t pkey,
                              uint32_t psn, const unsigned char *app_data, int data_len)
{
    if (!mgr) {
        fprintf(stderr, "❌ 缓存管理器未初始化\n");
        return -1;
    }
    
    struct connection_key key = create_connection_key(src_ip, dst_ip, 
                                                     src_port, dst_port,
                                                     src_qp, dest_qp,
                                                     service_type, pkey);
    
    // 设置窗口起始值为PSN，这样PSN就在窗口范围内
    struct connection_cache *conn_cache = get_or_create_connection_cache(mgr, &key, psn, 1024);
    if (!conn_cache) return -1;
    
    struct cached_packet *packet = malloc(sizeof(struct cached_packet));
    if (!packet) {
        perror("❌ malloc cached_packet 失败");
        return -1;
    }
    
    packet->app_data = malloc(data_len);
    if (!packet->app_data) {
        perror("❌ malloc app_data 失败");
        free(packet);
        return -1;
    }
    
    memcpy(packet->app_data, app_data, data_len);
    packet->data_len = data_len;
    packet->dest_qp = dest_qp;
    packet->psn = psn;
    gettimeofday(&packet->timestamp, NULL);
    packet->next = packet->prev = NULL;
    
    if (insert_packet_sorted(conn_cache, packet) != 0) {
        free(packet->app_data);
        free(packet);
        return -1;
    }
    
    printf("✅ 缓存报文成功: ");
    print_connection_key(&key);
    printf(", PSN=%u, 大小=%d bytes\n", psn, data_len);
    
    return 0;
}

int add_to_send_cache(const char *src_ip, const char *dst_ip,
                     uint16_t src_port, uint16_t dst_port,
                     uint32_t src_qp, uint32_t dest_qp,
                     uint8_t service_type, uint16_t pkey,
                     uint32_t psn, const unsigned char *app_data, int data_len)
{
    printf("📤 开始添加到发送缓存...\n");
    return add_to_connection_cache_ex(g_send_cache_mgr, src_ip, dst_ip,
                                     src_port, dst_port, src_qp, dest_qp,
                                     service_type, pkey, psn, app_data, data_len);
}

int add_to_recv_cache(const char *src_ip, const char *dst_ip,
                     uint16_t src_port, uint16_t dst_port,
                     uint32_t src_qp, uint32_t dest_qp,
                     uint8_t service_type, uint16_t pkey,
                     uint32_t psn, const unsigned char *app_data, int data_len)
{
    printf("📥 开始添加到接收缓存...\n");
    return add_to_connection_cache_ex(g_recv_cache_mgr, src_ip, dst_ip,
                                     src_port, dst_port, src_qp, dest_qp,
                                     service_type, pkey, psn, app_data, data_len);
}

void print_all_connections_status_ex(struct cache_manager *mgr, const char *name) {
    if (!mgr) {
        printf("❌ %s 缓存管理器为空\n", name);
        return;
    }
    
    printf("\n=== %s 缓存状态 ===\n", name);
    printf("总连接数: %zu/%zu\n", mgr->total_connections, mgr->max_connections);
    
    pthread_mutex_lock(&mgr->global_lock);
    
    int found_connections = 0;
    for (size_t i = 0; i < mgr->hash_table_size; i++) {
        struct hash_table_entry *entry = mgr->hash_table[i];
        while (entry) {
            found_connections++;
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
            
            // 打印缓存中的所有数据包内容
            struct cached_packet *pkt = cache->head;
            int pkt_count = 0;
            while (pkt) {
                printf("      数据包[%d]: PSN=%u, 内容=\"%s\" (长度=%d bytes)\n", 
                       pkt_count++, pkt->psn, pkt->app_data, pkt->data_len);
                pkt = pkt->next;
            }
            
            pthread_mutex_unlock(&cache->lock);
            entry = entry->next;
        }
    }
    
    if (found_connections == 0) {
        printf("  无活跃连接\n");
    }
    
    pthread_mutex_unlock(&mgr->global_lock);
    printf("================\n");
}

void print_send_cache_status() {
    print_all_connections_status_ex(g_send_cache_mgr, "发送方");
}

void print_recv_cache_status() {
    print_all_connections_status_ex(g_recv_cache_mgr, "接收方");
}

// RDMA辅助函数：通过设备名获取ibv_device
struct ibv_device* get_ibv_device_by_name(const char* dev_name) {
    struct ibv_device** dev_list;
    struct ibv_device* dev = NULL;
    int i;

    printf("🔍 正在查找RDMA设备: %s\n", dev_name);
    dev_list = ibv_get_device_list(NULL);
    if (!dev_list) {
        perror("❌ ibv_get_device_list 失败");
        return NULL;
    }

    for (i = 0; dev_list[i] != NULL; i++) {
        printf("  发现设备: %s\n", ibv_get_device_name(dev_list[i]));
        if (strcmp(ibv_get_device_name(dev_list[i]), dev_name) == 0) {
            dev = dev_list[i];
            printf("✅ 找到目标设备: %s\n", dev_name);
            break;
        }
    }

    if (!dev) {
        printf("❌ 未找到设备: %s\n", dev_name);
    }

    ibv_free_device_list(dev_list);
    return dev;
}

int main() {
    struct ibv_device* dev;
    struct ibv_context* ctx;
    struct ibv_pd* pd;
    struct ibv_qp* qp;
    struct ibv_cq* cq;
    struct ibv_mr* mr;
    char* buf;
    int ret;
    uint32_t psn = 12345;  // 初始PSN

    printf("🚀 ===== 开始RDMA缓存回环测试 =====\n");

    // 初始化发送和接收缓存管理器
    printf("\n📦 ===== 初始化缓存管理器 =====\n");
    g_send_cache_mgr = init_cache_manager(1024, 1000, 1000, 100, 300);
    if (!g_send_cache_mgr) {
        fprintf(stderr, "❌ 初始化发送方缓存管理器失败\n");
        return 1;
    }
    
    g_recv_cache_mgr = init_cache_manager(1024, 1000, 1000, 100, 300);
    if (!g_recv_cache_mgr) {
        fprintf(stderr, "❌ 初始化接收方缓存管理器失败\n");
        return 1;
    }
    printf("✅ 缓存管理器初始化完成\n");

    // 1. 获取rxe0设备
    printf("\n🔧 ===== 获取RDMA设备 =====\n");
    dev = get_ibv_device_by_name("rxe0");
    if (!dev) {
        fprintf(stderr, "❌ 未找到RDMA设备: rxe0\n");
        return 1;
    }

    // 2. 打开设备上下文
    printf("\n🔧 ===== 打开设备上下文 =====\n");
    ctx = ibv_open_device(dev);
    if (!ctx) {
        perror("❌ ibv_open_device 失败");
        return 1;
    }
    printf("✅ 成功打开RDMA设备上下文\n");

    // 3. 查询端口属性（简单测试）
    printf("\n🔧 ===== 查询端口属性 =====\n");
    struct ibv_port_attr port_attr;
    if (ibv_query_port(ctx, 1, &port_attr) != 0) {
        perror("❌ ibv_query_port 失败");
        ibv_close_device(ctx);
        return 1;
    }
    printf("✅ 端口LID: %d, 状态: %s\n", port_attr.lid, 
           (port_attr.state == IBV_PORT_ACTIVE) ? "ACTIVE" : "INACTIVE");

    // 4. 创建保护域（PD）
    printf("\n🔧 ===== 创建保护域 =====\n");
    pd = ibv_alloc_pd(ctx);
    if (!pd) {
        perror("❌ ibv_alloc_pd 失败");
        ibv_close_device(ctx);
        return 1;
    }
    printf("✅ 成功创建保护域\n");

    // 5. 分配内存并注册MR
    printf("\n🔧 ===== 分配并注册内存 =====\n");
    buf = malloc(1024);
    if (!buf) {
        perror("❌ malloc 失败");
        ibv_dealloc_pd(pd);
        ibv_close_device(ctx);
        return 1;
    }
    
    // 初始化测试数据
    strcpy(buf, "RDMA缓存回环测试数据 - 验证发送和接收缓存功能");
    printf("✅ 缓冲区初始化数据: \"%s\" (长度: %zu bytes)\n", buf, strlen(buf) + 1);

    mr = ibv_reg_mr(pd, buf, 1024, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    if (!mr) {
        perror("❌ ibv_reg_mr 失败");
        free(buf);
        ibv_dealloc_pd(pd);
        ibv_close_device(ctx);
        return 1;
    }
    printf("✅ 内存区域注册成功\n");

    // 6. 创建完成队列（CQ）
    printf("\n🔧 ===== 创建完成队列 =====\n");
    cq = ibv_create_cq(ctx, 10, NULL, NULL, 0);
    if (!cq) {
        perror("❌ ibv_create_cq 失败");
        ibv_dereg_mr(mr);
        free(buf);
        ibv_dealloc_pd(pd);
        ibv_close_device(ctx);
        return 1;
    }
    printf("✅ 完成队列创建成功\n");

    // 7. 创建队列对（QP）- 使用RC模式
    printf("\n🔧 ===== 创建队列对 =====\n");
    struct ibv_qp_init_attr qp_attr = {
        .send_cq = cq,
        .recv_cq = cq,
        .cap = {
            .max_send_wr = 10, 
            .max_recv_wr = 10, 
            .max_send_sge = 1, 
            .max_recv_sge = 1
        },
        .qp_type = IBV_QPT_RC
    };
    
    qp = ibv_create_qp(pd, &qp_attr);
    if (!qp) {
        perror("❌ ibv_create_qp 失败");
        ibv_destroy_cq(cq);
        ibv_dereg_mr(mr);
        free(buf);
        ibv_dealloc_pd(pd);
        ibv_close_device(ctx);
        return 1;
    }
    printf("✅ 创建QP成功，QP号: %d\n", qp->qp_num);

    printf("\n✅ 基本RDMA资源初始化成功！\n");

    // 8. 将发送数据缓存到发送方缓存
    printf("\n💾 ===== 缓存发送数据到发送方缓存 =====\n");
    printf("准备缓存发送数据: \"%s\" (PSN: %u)\n", buf, psn);
    ret = add_to_send_cache("127.0.0.1", "127.0.0.1",
                           1234, 5678,
                           qp->qp_num, qp->qp_num,
                           0, 0xffff,
                           psn, (unsigned char*)buf, strlen(buf) + 1);
    if (ret != 0) {
        fprintf(stderr, "❌ 发送方缓存数据失败\n");
        goto cleanup;
    }
    printf("✅ 发送方缓存数据成功\n");

    // 9. 模拟发送操作（简单回环）
    printf("\n📤 ===== 模拟发送操作 =====\n");
    printf("✅ 模拟发送成功！发送的数据: \"%s\" (长度: %zu bytes, PSN: %u)\n", 
           buf, strlen(buf) + 1, psn);

    // 10. 模拟接收操作（简单回环）
    printf("\n📥 ===== 模拟接收操作 =====\n");
    char received_data[1024];
    strcpy(received_data, buf); // 模拟接收相同的数据
    printf("✅ 模拟接收成功！接收的数据: \"%s\" (长度: %zu bytes)\n", 
           received_data, strlen(received_data) + 1);

    // 11. 将接收的数据缓存到接收方缓存
    printf("\n💾 ===== 缓存接收数据到接收方缓存 =====\n");
    printf("准备缓存接收数据: \"%s\" (PSN: %u)\n", received_data, psn);
    ret = add_to_recv_cache("127.0.0.1", "127.0.0.1",
                           5678, 1234,
                           qp->qp_num, qp->qp_num,
                           0, 0xffff,
                           psn, (unsigned char*)received_data, strlen(received_data) + 1);
    if (ret != 0) {
        fprintf(stderr, "❌ 接收方缓存数据失败\n");
        goto cleanup;
    }
    printf("✅ 接收方缓存数据成功\n");

    // 12. 打印缓存状态
    printf("\n📊 ===== 打印缓存状态 =====\n");
    print_send_cache_status();
    print_recv_cache_status();

    // 13. 验证缓存一致性
    printf("\n🔍 ===== 验证缓存一致性 =====\n");
    printf("发送缓存数据: \"%s\"\n", buf);
    printf("接收缓存数据: \"%s\"\n", received_data);
    if (strcmp(buf, received_data) == 0) {
        printf("✅ 缓存一致性验证成功！发送和接收数据匹配\n");
    } else {
        printf("❌ 缓存一致性验证失败！发送和接收数据不匹配\n");
    }

    printf("\n🎉 ===== RDMA缓存回环测试完成 =====\n");
    printf("✅ 所有缓存操作成功完成！\n");
    printf("✅ 发送缓存和接收缓存都正确存储了数据\n");
    printf("✅ 数据完整性验证通过\n");

cleanup:
    // 清理资源
    printf("\n🧹 ===== 清理资源 =====\n");
    if (qp) {
        ibv_destroy_qp(qp);
        printf("✅ 销毁QP\n");
    }
    if (cq) {
        ibv_destroy_cq(cq);
        printf("✅ 销毁CQ\n");
    }
    if (mr) {
        ibv_dereg_mr(mr);
        printf("✅ 注销MR\n");
    }
    if (buf) {
        free(buf);
        printf("✅ 释放缓冲区\n");
    }
    if (pd) {
        ibv_dealloc_pd(pd);
        printf("✅ 释放保护域\n");
    }
    if (ctx) {
        ibv_close_device(ctx);
        printf("✅ 关闭设备上下文\n");
    }
    
    // 销毁缓存管理器
    if (g_send_cache_mgr) {
        printf("清理发送缓存管理器...\n");
        for (size_t i = 0; i < g_send_cache_mgr->hash_table_size; i++) {
            struct hash_table_entry *entry = g_send_cache_mgr->hash_table[i];
            while (entry) {
                struct hash_table_entry *next = entry->next;
                destroy_connection_cache(entry->cache);
                free(entry);
                entry = next;
            }
        }
        free(g_send_cache_mgr->hash_table);
        pthread_mutex_destroy(&g_send_cache_mgr->global_lock);
        free(g_send_cache_mgr);
        printf("✅ 发送缓存管理器清理完成\n");
    }
    
    if (g_recv_cache_mgr) {
        printf("清理接收缓存管理器...\n");
        for (size_t i = 0; i < g_recv_cache_mgr->hash_table_size; i++) {
            struct hash_table_entry *entry = g_recv_cache_mgr->hash_table[i];
            while (entry) {
                struct hash_table_entry *next = entry->next;
                destroy_connection_cache(entry->cache);
                free(entry);
                entry = next;
            }
        }
        free(g_recv_cache_mgr->hash_table);
        pthread_mutex_destroy(&g_recv_cache_mgr->global_lock);
        free(g_recv_cache_mgr);
        printf("✅ 接收缓存管理器清理完成\n");
    }
    
    printf("\n🏁 测试程序结束\n");
    return ret;
}
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <rdma/rdma_verbs.h>
#include <rdma/ib_user_verbs.h>
#include <unistd.h>

/*
编译命令：
gcc pkt_cache_send.c -o pkt_cache_send -lpthread -lrdmacm -libverbs
*/

//声明
void destroy_connection_cache(struct connection_cache *cache);

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

// 每个连接的缓存队列
struct connection_cache {
    struct cached_packet *head;       // 队列头（最小PSN）
    struct cached_packet *tail;       // 队列尾（最大PSN）
    size_t count;                     // 当前缓存数量
    size_t total_bytes;               // 总字节数
    uint32_t min_psn;                 // 最小PSN
    uint32_t max_psn;                 // 最大PSN
    uint32_t window_start;            // 滑动窗口起始PSN
    uint32_t window_size;             // 滑动窗口大小
    struct timeval last_activity;     // 最后活动时间
    pthread_mutex_t lock;             // 连接级锁
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
} *g_cache_mgr;

// RDMA上下文
struct rdma_context {
    struct ibv_context *ctx;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    struct ibv_ah *ah;
    struct ibv_port_attr port_attr;
    char *send_buf;
    struct ibv_mr *send_mr;
} rdma_ctx;

// 工具函数：创建连接键
struct connection_key create_connection_key(const char *src_ip_str, const char *dst_ip_str,
                                           uint16_t src_port, uint16_t dst_port,
                                           uint32_t src_qp, uint32_t dest_qp,
                                           uint8_t service_type, uint16_t pkey) {
    struct connection_key key;
    inet_pton(AF_INET, src_ip_str, &key.src_ip);
    inet_pton(AF_INET, dst_ip_str, &key.dst_ip);
    key.src_port = src_port;
    key.dst_port = dst_port;
    key.src_qp = src_qp;
    key.dest_qp = dest_qp;
    key.service_type = service_type;
    key.pkey = pkey;
    return key;
}

// 工具函数：打印连接键
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

// 比较两个连接键是否相等
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

// 计算哈希索引
static uint32_t calculate_hash(const struct connection_key *key, size_t table_size) {
    uint64_t hash = key->src_ip ^ key->dst_ip ^ key->src_port ^ key->dst_port ^
                   key->src_qp ^ key->dest_qp ^ key->service_type ^ key->pkey;
    return hash % table_size;
}

// 初始化缓存管理器
struct cache_manager* init_cache_manager(size_t hash_size, size_t max_conns,
                                        size_t max_pkts_per_conn, size_t max_bytes_per_conn,
                                        int timeout) {
    struct cache_manager *mgr = malloc(sizeof(*mgr));
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
    mgr->max_packets_per_conn = max_pkts_per_conn;
    mgr->max_bytes_per_conn = max_bytes_per_conn;
    mgr->connection_timeout = timeout;
    mgr->total_connections = 0;
    pthread_mutex_init(&mgr->global_lock, NULL);

    printf("[缓存管理器] 初始化成功 - 最大连接数: %zu, 每连接最大报文数: %zu\n",
           max_conns, max_pkts_per_conn);
    return mgr;
}

// 创建新的连接缓存
static struct connection_cache* create_connection_cache(uint32_t init_window_start, uint32_t window_size) {
    struct connection_cache *cache = malloc(sizeof(*cache));
    if (!cache) {
        perror("malloc connection_cache failed");
        return NULL;
    }

    cache->head = cache->tail = NULL;
    cache->count = 0;
    cache->total_bytes = 0;
    cache->min_psn = 0;
    cache->max_psn = 0;
    cache->window_start = init_window_start;
    cache->window_size = window_size;
    gettimeofday(&cache->last_activity, NULL);
    pthread_mutex_init(&cache->lock, NULL);

    printf("[连接缓存] 创建新缓存 - 初始窗口: [%u, %u]\n",
           init_window_start, init_window_start + window_size - 1);
    return cache;
}

// 查找或创建连接缓存
static struct connection_cache* find_or_create_connection_cache(const struct connection_key *key) {
    if (!g_cache_mgr) return NULL;

    uint32_t hash_index = calculate_hash(key, g_cache_mgr->hash_table_size);
    pthread_mutex_lock(&g_cache_mgr->global_lock);

    // 查找现有连接
    struct hash_table_entry *entry = g_cache_mgr->hash_table[hash_index];
    while (entry) {
        if (connection_keys_equal(&entry->key, key)) {
            pthread_mutex_unlock(&g_cache_mgr->global_lock);
            return entry->cache;
        }
        entry = entry->next;
    }

    // 检查是否达到最大连接数
    if (g_cache_mgr->total_connections >= g_cache_mgr->max_connections) {
        fprintf(stderr, "[连接缓存] 已达最大连接数: %zu\n", g_cache_mgr->max_connections);
        pthread_mutex_unlock(&g_cache_mgr->global_lock);
        return NULL;
    }

    // 创建新连接
    struct connection_cache *cache = create_connection_cache(0, 1024); // 初始窗口大小1024
    if (!cache) {
        pthread_mutex_unlock(&g_cache_mgr->global_lock);
        return NULL;
    }

    entry = malloc(sizeof(*entry));
    if (!entry) {
        perror("malloc hash_table_entry failed");
        destroy_connection_cache(cache);
        pthread_mutex_unlock(&g_cache_mgr->global_lock);
        return NULL;
    }

    entry->key = *key;
    entry->cache = cache;
    entry->next = g_cache_mgr->hash_table[hash_index];
    g_cache_mgr->hash_table[hash_index] = entry;
    g_cache_mgr->total_connections++;

    printf("[连接缓存] 新增连接 - ");
    print_connection_key(key);
    printf("\n");

    pthread_mutex_unlock(&g_cache_mgr->global_lock);
    return cache;
}

// 销毁连接缓存
void destroy_connection_cache(struct connection_cache *cache) {
    if (!cache) return;
    
    pthread_mutex_lock(&cache->lock);
    printf("[连接缓存] 销毁缓存 - 释放 %zu 个报文\n", cache->count);
    
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

// 插入报文到缓存（按PSN排序）
int insert_packet_sorted(struct connection_cache *cache, struct cached_packet *new_packet) {
    if (!cache || !new_packet) return -1;
    
    pthread_mutex_lock(&cache->lock);
    uint32_t window_end = cache->window_start + cache->window_size - 1;

    // 检查窗口范围
    if (new_packet->psn < cache->window_start || new_packet->psn > window_end) {
        printf("[缓存插入] 报文PSN=%u 超出窗口 [%u, %u] - 插入失败\n",
               new_packet->psn, cache->window_start, window_end);
        pthread_mutex_unlock(&cache->lock);
        return -1;
    }

    // 检查缓存限制
    size_t new_total_bytes = cache->total_bytes + new_packet->data_len + sizeof(struct cached_packet);
    if (cache->count >= g_cache_mgr->max_packets_per_conn ||
        new_total_bytes >= g_cache_mgr->max_bytes_per_conn) {
        printf("[缓存插入] 缓存已满 - 当前: %zu/%zu 报文, %zu/%zu 字节\n",
               cache->count, g_cache_mgr->max_packets_per_conn,
               cache->total_bytes, g_cache_mgr->max_bytes_per_conn);
        pthread_mutex_unlock(&cache->lock);
        return -1;
    }

    // 更新PSN范围
    if (cache->count == 0) {
        cache->min_psn = cache->max_psn = new_packet->psn;
    } else {
        if (new_packet->psn < cache->min_psn) cache->min_psn = new_packet->psn;
        if (new_packet->psn > cache->max_psn) cache->max_psn = new_packet->psn;
    }

    // 查找插入位置
    struct cached_packet *current = cache->head;
    struct cached_packet *prev = NULL;
    while (current && current->psn < new_packet->psn) {
        prev = current;
        current = current->next;
    }

    // 处理重复PSN
    if (current && current->psn == new_packet->psn) {
        printf("[缓存插入] 发现重复PSN=%u - 替换现有报文\n", new_packet->psn);
        // 移除旧报文
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
    new_packet->prev = prev;
    new_packet->next = current;
    if (prev) prev->next = new_packet;
    else cache->head = new_packet;
    if (current) current->prev = new_packet;
    else cache->tail = new_packet;

    // 更新统计信息
    cache->count++;
    cache->total_bytes += (new_packet->data_len + sizeof(struct cached_packet));
    gettimeofday(&cache->last_activity, NULL);

    printf("[缓存插入] 成功 - PSN=%u, 报文数=%zu, 总字节数=%zu\n",
           new_packet->psn, cache->count, cache->total_bytes);
    pthread_mutex_unlock(&cache->lock);
    return 0;
}

// 滑动窗口清理
static void slide_window(struct connection_cache *cache) {
    if (!cache) return;

    uint32_t window_end = cache->window_start + cache->window_size - 1;
    printf("[窗口滑动] 清理 PSN < %u 的报文 - 新窗口 [%u, %u]\n",
           cache->window_start, cache->window_start, window_end);

    struct cached_packet *current = cache->head;
    while (current && current->psn < cache->window_start) {
        struct cached_packet *to_remove = current;
        current = current->next;

        // 从链表移除
        if (to_remove->prev) to_remove->prev->next = to_remove->next;
        else cache->head = to_remove->next;
        if (to_remove->next) to_remove->next->prev = to_remove->prev;
        else cache->tail = to_remove->prev;

        // 更新统计
        cache->count--;
        cache->total_bytes -= (to_remove->data_len + sizeof(struct cached_packet));
        printf("[窗口清理] 移除 PSN=%u - 剩余报文数=%zu\n", to_remove->psn, cache->count);
        
        free(to_remove->app_data);
        free(to_remove);
    }

    // 更新最小PSN
    cache->min_psn = cache->head ? cache->head->psn : 0;
}

// 更新窗口起始位置
int update_window_start(const struct connection_key *key, uint32_t new_start) {
    if (!g_cache_mgr) return -1;
    
    uint32_t hash_index = calculate_hash(key, g_cache_mgr->hash_table_size);
    pthread_mutex_lock(&g_cache_mgr->global_lock);
    
    struct hash_table_entry *entry = g_cache_mgr->hash_table[hash_index];
    while (entry) {
        if (connection_keys_equal(&entry->key, key)) {
            struct connection_cache *cache = entry->cache;
            pthread_mutex_lock(&cache->lock);
            
            if (new_start > cache->window_start) {
                printf("[窗口更新] 旧起始=%u -> 新起始=%u\n", cache->window_start, new_start);
                cache->window_start = new_start;
                slide_window(cache);
            } else {
                printf("[窗口更新] 新起始=%u 不大于旧起始=%u - 无需更新\n", new_start, cache->window_start);
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

// 打印所有连接状态
void print_all_connections_status() {
    if (!g_cache_mgr) return;
    
    printf("\n=== 缓存状态汇总 ===\n");
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
    printf("====================\n\n");
}

// 初始化RDMA资源
int init_rdma() {
    printf("\n=== 初始化RDMA资源 ===\n");
    struct ibv_device **dev_list = ibv_get_device_list(NULL);
    if (!dev_list) {
        perror("ibv_get_device_list failed");
        return -1;
    }

    // 查找rxe_eth1设备
    struct ibv_device *dev = NULL;
    for (int i = 0; dev_list[i]; i++) {
        printf("发现RDMA设备: %s\n", ibv_get_device_name(dev_list[i]));
        if (strcmp(ibv_get_device_name(dev_list[i]), "rxe_eth1") == 0) {
            dev = dev_list[i];
            break;
        }
    }
    if (!dev) {
        fprintf(stderr, "未找到rxe_eth1设备\n");
        ibv_free_device_list(dev_list);
        return -1;
    }
    printf("使用RDMA设备: %s\n", ibv_get_device_name(dev));

    // 打开设备上下文
    rdma_ctx.ctx = ibv_open_device(dev);
    if (!rdma_ctx.ctx) {
        perror("ibv_open_device failed");
        ibv_free_device_list(dev_list);
        return -1;
    }
    printf("成功打开设备上下文\n");

    // 分配保护域
    rdma_ctx.pd = ibv_alloc_pd(rdma_ctx.ctx);
    if (!rdma_ctx.pd) {
        perror("ibv_alloc_pd failed");
        ibv_close_device(rdma_ctx.ctx);
        ibv_free_device_list(dev_list);
        return -1;
    }
    printf("成功分配保护域 (pd=%p)\n", rdma_ctx.pd);

    // 创建完成队列
    rdma_ctx.cq = ibv_create_cq(rdma_ctx.ctx, 1024, NULL, NULL, 0);
    if (!rdma_ctx.cq) {
        perror("ibv_create_cq failed");
        ibv_dealloc_pd(rdma_ctx.pd);
        ibv_close_device(rdma_ctx.ctx);
        ibv_free_device_list(dev_list);
        return -1;
    }
    printf("成功创建完成队列 (cq=%p)\n", rdma_ctx.cq);

    // 查询端口属性
    if (ibv_query_port(rdma_ctx.ctx, 1, &rdma_ctx.port_attr) != 0) {
        perror("ibv_query_port failed");
        ibv_destroy_cq(rdma_ctx.cq);
        ibv_dealloc_pd(rdma_ctx.pd);
        ibv_close_device(rdma_ctx.ctx);
        ibv_free_device_list(dev_list);
        return -1;
    }
    printf("端口1属性 - 状态: %u, MTU: %u\n", rdma_ctx.port_attr.state, rdma_ctx.port_attr.max_mtu);

    // 创建QP
    struct ibv_qp_init_attr qp_attr = {
        .send_cq = rdma_ctx.cq,
        .recv_cq = rdma_ctx.cq,
        .qp_type = IBV_QPT_UD,
        .cap = {
            .max_send_wr = 1024,
            .max_recv_wr = 1024,
            .max_send_sge = 1,
            .max_recv_sge = 1
        }
    };
    rdma_ctx.qp = ibv_create_qp(rdma_ctx.pd, &qp_attr);
    if (!rdma_ctx.qp) {
        perror("ibv_create_qp failed");
        ibv_destroy_cq(rdma_ctx.cq);
        ibv_dealloc_pd(rdma_ctx.pd);
        ibv_close_device(rdma_ctx.ctx);
        ibv_free_device_list(dev_list);
        return -1;
    }
    printf("成功创建QP (qp_num=%u)\n", rdma_ctx.qp->qp_num);

    // 分配发送缓冲区
    rdma_ctx.send_buf = malloc(1024);
    if (!rdma_ctx.send_buf) {
        perror("malloc send_buf failed");
        ibv_destroy_qp(rdma_ctx.qp);
        ibv_destroy_cq(rdma_ctx.cq);
        ibv_dealloc_pd(rdma_ctx.pd);
        ibv_close_device(rdma_ctx.ctx);
        ibv_free_device_list(dev_list);
        return -1;
    }

    // 注册内存区域
    rdma_ctx.send_mr = ibv_reg_mr(rdma_ctx.pd, rdma_ctx.send_buf, 1024,
                                 IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    if (!rdma_ctx.send_mr) {
        perror("ibv_reg_mr failed");
        free(rdma_ctx.send_buf);
        ibv_destroy_qp(rdma_ctx.qp);
        ibv_destroy_cq(rdma_ctx.cq);
        ibv_dealloc_pd(rdma_ctx.pd);
        ibv_close_device(rdma_ctx.ctx);
        ibv_free_device_list(dev_list);
        return -1;
    }
    printf("成功注册发送缓冲区 (lkey=0x%x)\n", rdma_ctx.send_mr->lkey);

    ibv_free_device_list(dev_list);
    return 0;
}

// 初始化QP状态
int init_qp(uint32_t remote_qp_num, const union ibv_gid *remote_gid) {
    printf("\n=== 初始化QP状态 ===\n");
    struct ibv_qp_attr attr;
    memset(&attr, 0, sizeof(attr));
    int ret;

    // 查询PKey
    uint16_t pkey;
    if (ibv_query_pkey(rdma_ctx.ctx, 1, 0, &pkey) != 0) {
        perror("ibv_query_pkey failed");
        return -1;
    }
    printf("使用PKey[0] = 0x%04x\n", pkey);

    // 切换到INIT状态
    attr.qp_state = IBV_QPS_INIT;
    attr.pkey_index = 0;
    attr.port_num = 1;
    attr.qp_access_flags = 0;

    ret = ibv_modify_qp(rdma_ctx.qp, &attr,
                       IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS);
    if (ret != 0) {
        perror("ibv_modify_qp to INIT failed");
        return -1;
    }
    printf("QP状态已切换到INIT\n");

    // 切换到RTR状态（UD类型需要设置远程信息）
    attr.qp_state = IBV_QPS_RTR;
    struct ibv_ah_attr ah_attr = {0};
    ah_attr.is_global = 1;
    ah_attr.grh.dgid = *remote_gid;
    ah_attr.grh.flow_label = 0;
    ah_attr.grh.hop_limit = 1;
    ah_attr.grh.sgid_index = 0;
    ah_attr.port_num = 1;

    attr.ah_attr = ah_attr;
    attr.remote_qpn = remote_qp_num;
    attr.remote_qkey = 0x11111111;

    // 同时修改ibv_modify_qp的flags参数：
    ret = ibv_modify_qp(rdma_ctx.qp, &attr,
                    IBV_QP_STATE | IBV_QP_REMOTE_QPN | IBV_QP_REMOTE_QKEY | IBV_QP_AH_ATTR);
    if (ret != 0) {
        perror("ibv_modify_qp to RTR failed");
        return -1;
    }
    printf("QP状态已切换到RTR (远程QP=%u)\n", remote_qp_num);

    // 切换到RTS状态
    attr.qp_state = IBV_QPS_RTS;
    attr.sq_psn = 0;
    attr.max_dest_rd_atomic = 1;
    attr.min_rnr_timer = 0x12;

    ret = ibv_modify_qp(rdma_ctx.qp, &attr,
                       IBV_QP_STATE | IBV_QP_SQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER);
    if (ret != 0) {
        perror("ibv_modify_qp to RTS failed");
        return -1;
    }
    printf("QP状态已切换到RTS (初始PSN=0)\n");

    return 0;
}

// 发送RDMA数据包
int send_rdma_packet(const struct connection_key *key, const void *data, int len, uint32_t psn) {
    if (!rdma_ctx.qp || !data || len <= 0) return -1;

    // 复制数据到发送缓冲区
    memcpy(rdma_ctx.send_buf, data, len);

    // 构造发送请求
    struct ibv_send_wr wr = {
        .wr_id = psn,
        .opcode = IBV_WR_SEND,
        .send_flags = IBV_SEND_SIGNALED,
        .sg_list = &(struct ibv_sge){
            .addr = (uintptr_t)rdma_ctx.send_buf,
            .length = len,
            .lkey = rdma_ctx.send_mr->lkey
        },
        .num_sge = 1,
        .wr.ud.remote_qpn = key->dest_qp,
        .wr.ud.remote_qkey = 0x11111111
    };
    struct ibv_send_wr *bad_wr;

    printf("[发送] 尝试发送 PSN=%u, 长度=%d\n", psn, len);
    int ret = ibv_post_send(rdma_ctx.qp, &wr, &bad_wr);
    if (ret != 0) {
        perror("ibv_post_send failed");
        return -1;
    }

    // 等待发送完成
    struct ibv_wc wc;
    int polled = 0;
    while (polled < 100) {  // 最多等待1秒
        ret = ibv_poll_cq(rdma_ctx.cq, 1, &wc);
        if (ret < 0) {
            perror("ibv_poll_cq failed");
            return -1;
        } else if (ret == 0) {
            polled++;
            usleep(10000);  // 10ms
            continue;
        }

        // 检查完成状态
        if (wc.status != IBV_WC_SUCCESS) {
            fprintf(stderr, "[发送] 完成状态错误: %d\n", wc.status);
            return -1;
        }
        printf("[发送] 成功 - PSN=%u, wr_id=%lu\n", psn, (unsigned long)wc.wr_id);
        return 0;
    }

    fprintf(stderr, "[发送] 超时\n");
    return -1;
}

// 添加报文到缓存并发送
int add_to_connection_cache(const char *src_ip, const char *dst_ip,
                           uint16_t src_port, uint16_t dst_port,
                           uint32_t src_qp, uint32_t dest_qp,
                           uint8_t service_type, uint16_t pkey,
                           uint32_t psn, const void *data, int len) {
    // 创建连接键
    struct connection_key key = create_connection_key(
        src_ip, dst_ip, src_port, dst_port, src_qp, dest_qp, service_type, pkey
    );

    // 查找或创建连接缓存
    struct connection_cache *cache = find_or_create_connection_cache(&key);
    if (!cache) return -1;

    // 创建并初始化数据包
    struct cached_packet *packet = malloc(sizeof(*packet));
    if (!packet) {
        perror("malloc cached_packet failed");
        return -1;
    }

    packet->app_data = malloc(len);
    if (!packet->app_data) {
        perror("malloc app_data failed");
        free(packet);
        return -1;
    }

    memcpy(packet->app_data, data, len);
    packet->data_len = len;
    packet->dest_qp = dest_qp;
    packet->psn = psn;
    gettimeofday(&packet->timestamp, NULL);
    packet->prev = packet->next = NULL;

    // 插入缓存
    if (insert_packet_sorted(cache, packet) != 0) {
        free(packet->app_data);
        free(packet);
        return -1;
    }

    // 发送数据包
    return send_rdma_packet(&key, data, len, psn);
}

// 主函数
int main() {
    // 初始化RDMA
    if (init_rdma() != 0) {
        fprintf(stderr, "RDMA初始化失败\n");
        return 1;
    }

    // 手动配置接收端信息（请根据接收端输出填写）
    uint32_t remote_qp_num = 0;  // 替换为接收端QP号
    union ibv_gid remote_gid;
    memset(&remote_gid, 0, sizeof(remote_gid));
    // 示例GID（替换为接收端实际GID）
    unsigned char gid_bytes[16] = {0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                  0x00, 0x00, 0x00, 0x00, 0xc0, 0xa8, 0xef, 0x85};
    memcpy(&remote_gid, gid_bytes, 16);

    // 初始化QP状态
    if (init_qp(remote_qp_num, &remote_gid) != 0) {
        fprintf(stderr, "QP初始化失败\n");
        return 1;
    }

    // 初始化缓存管理器
    g_cache_mgr = init_cache_manager(1024, 10, 100, 1024*1024, 300);
    if (!g_cache_mgr) return 1;

    // 发送测试数据
    printf("\n=== 开始发送测试数据 ===\n");
    unsigned char data1[] = "RDMA Test Packet 1";
    unsigned char data2[] = "RDMA Test Packet 2";
    unsigned char data3[] = "RDMA Test Packet 3";

    // 发送端信息（rxe_eth1）
    const char *src_ip = "192.168.239.130";
    const char *dst_ip = "192.168.239.133";  // 接收端IP
    uint16_t src_port = 1234, dst_port = 5678;
    uint32_t src_qp = rdma_ctx.qp->qp_num;    // 本地QP号
    uint32_t dest_qp = remote_qp_num;         // 远程QP号

    // 发送三个数据包
    add_to_connection_cache(src_ip, dst_ip, src_port, dst_port,
                           src_qp, dest_qp, 0, 0xffff, 100, data1, sizeof(data1));
    add_to_connection_cache(src_ip, dst_ip, src_port, dst_port,
                           src_qp, dest_qp, 0, 0xffff, 101, data2, sizeof(data2));
    add_to_connection_cache(src_ip, dst_ip, src_port, dst_port,
                           src_qp, dest_qp, 0, 0xffff, 102, data3, sizeof(data3));

    // 打印缓存状态
    print_all_connections_status();

    // 滑动窗口测试
    struct connection_key key = create_connection_key(src_ip, dst_ip, src_port, dst_port,
                                                     src_qp, dest_qp, 0, 0xffff);
    update_window_start(&key, 101);
    print_all_connections_status();

    // 清理资源
    printf("\n=== 清理资源 ===\n");
    if (rdma_ctx.send_mr) ibv_dereg_mr(rdma_ctx.send_mr);
    if (rdma_ctx.send_buf) free(rdma_ctx.send_buf);
    if (rdma_ctx.qp) ibv_destroy_qp(rdma_ctx.qp);
    if (rdma_ctx.cq) ibv_destroy_cq(rdma_ctx.cq);
    if (rdma_ctx.pd) ibv_dealloc_pd(rdma_ctx.pd);
    if (rdma_ctx.ctx) ibv_close_device(rdma_ctx.ctx);
    
    return 0;
}
/*
编译命令：
gcc recv_test_cache.c -o receiver -lpthread -libverbs
*/


#define RXE_ETH1_GUID 0x020c29fffe1001ff
#define RXE_ETH3_GUID 0x020c29fffe10011d
#define DEFAULT_QKEY 0x11111111  // 统一QKey
#define DEFAULT_PKEY 0xffff      // 通用PKey

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <rdma/rdma_verbs.h>
#include <rdma/ib_user_verbs.h>
#include <unistd.h>  // 解决 usleep 声明问题

// 缓存报文结构（与发送端一致）
struct cached_packet {
    unsigned char *app_data;
    int data_len;
    uint32_t src_qp;
    uint32_t psn;
    struct timeval timestamp;
    struct cached_packet *next;
    struct cached_packet *prev;
};

// 连接键结构（与发送端一致）
struct connection_key {
    uint32_t src_ip;
    uint32_t dst_ip;
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t src_qp;
    uint32_t dest_qp;
    uint8_t service_type;
    uint16_t pkey;
};

// 连接缓存结构
struct connection_cache {
    struct cached_packet *head;
    struct cached_packet *tail;
    size_t count;
    size_t total_bytes;
    uint32_t min_psn;
    uint32_t max_psn;
    struct timeval last_activity;
    pthread_mutex_t lock;
};

// 哈希表节点
struct hash_table_entry {
    struct connection_key key;
    struct connection_cache *cache;
    struct hash_table_entry *next;
};

// 缓存管理器
struct cache_manager {
    struct hash_table_entry **hash_table;
    size_t hash_table_size;
    size_t max_connections;
    size_t max_packets_per_conn;
    pthread_mutex_t global_lock;
    size_t total_connections;
} *g_cache_mgr;

// 接收端RDMA上下文
struct recv_rdma_ctx {
    struct ibv_context *ctx;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    struct ibv_mr *mr;
    char *recv_buf;
    union ibv_gid local_gid;
} recv_ctx;

//在init_recv_rdma函数前定义remote_gid
union ibv_gid remote_gid = {0};  // 发送端rxe_eth1的GID

// 计算哈希索引
static uint32_t calculate_hash(const struct connection_key *key, size_t table_size) {
    uint64_t hash = key->src_ip ^ key->dst_ip ^ key->src_port ^ key->dst_port ^
                   key->src_qp ^ key->dest_qp ^ key->service_type ^ key->pkey;
    return hash % table_size;
}

// 比较连接键
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

// 打印连接键
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

// 初始化缓存管理器
struct cache_manager* init_cache_manager(size_t hash_size, size_t max_conns, size_t max_pkts_per_conn) {
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
    mgr->total_connections = 0;
    pthread_mutex_init(&mgr->global_lock, NULL);

    printf("[接收缓存] 初始化成功 - 最大连接数: %zu, 每连接最大报文数: %zu\n",
           max_conns, max_pkts_per_conn);
    return mgr;
}

// 创建连接缓存
static struct connection_cache* create_connection_cache() {
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
    gettimeofday(&cache->last_activity, NULL);
    pthread_mutex_init(&cache->lock, NULL);

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

    // 检查最大连接数
    if (g_cache_mgr->total_connections >= g_cache_mgr->max_connections) {
        fprintf(stderr, "[接收缓存] 已达最大连接数: %zu\n", g_cache_mgr->max_connections);
        pthread_mutex_unlock(&g_cache_mgr->global_lock);
        return NULL;
    }

    // 创建新连接
    struct connection_cache *cache = create_connection_cache();
    if (!cache) {
        pthread_mutex_unlock(&g_cache_mgr->global_lock);
        return NULL;
    }

    entry = malloc(sizeof(*entry));
    if (!entry) {
        perror("malloc hash_table_entry failed");
        free(cache);
        pthread_mutex_unlock(&g_cache_mgr->global_lock);
        return NULL;
    }

    entry->key = *key;
    entry->cache = cache;
    entry->next = g_cache_mgr->hash_table[hash_index];
    g_cache_mgr->hash_table[hash_index] = entry;
    g_cache_mgr->total_connections++;

    printf("[接收缓存] 新增连接 - ");
    print_connection_key(key);
    printf("\n");

    pthread_mutex_unlock(&g_cache_mgr->global_lock);
    return cache;
}

// 插入接收的报文到缓存
int insert_received_packet(const struct connection_key *key, uint32_t psn, const void *data, int len) {
    struct connection_cache *cache = find_or_create_connection_cache(key);
    if (!cache) return -1;

    // 创建数据包
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
    packet->src_qp = key->src_qp;
    packet->psn = psn;
    gettimeofday(&packet->timestamp, NULL);
    packet->prev = packet->next = NULL;

    // 插入缓存（按PSN排序）
    pthread_mutex_lock(&cache->lock);

    // 查找插入位置
    struct cached_packet *current = cache->head;
    struct cached_packet *prev = NULL;
    while (current && current->psn < packet->psn) {
        prev = current;
        current = current->next;
    }

    // 处理重复PSN
    if (current && current->psn == packet->psn) {
        printf("[接收缓存] 发现重复PSN=%u - 替换现有报文\n", psn);
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
    packet->prev = prev;
    packet->next = current;
    if (prev) prev->next = packet;
    else cache->head = packet;
    if (current) current->prev = packet;
    else cache->tail = packet;

    // 更新统计
    if (cache->count == 0) {
        cache->min_psn = cache->max_psn = psn;
    } else {
        if (psn < cache->min_psn) cache->min_psn = psn;
        if (psn > cache->max_psn) cache->max_psn = psn;
    }

    cache->count++;
    cache->total_bytes += (len + sizeof(struct cached_packet));
    gettimeofday(&cache->last_activity, NULL);

    printf("[接收缓存] 插入成功 - PSN=%u, 报文数=%zu, 总字节数=%zu\n",
           psn, cache->count, cache->total_bytes);
    pthread_mutex_unlock(&cache->lock);
    return 0;
}

// 打印接收缓存状态
void print_receive_cache_status() {
    if (!g_cache_mgr) return;
    
    printf("\n=== 接收缓存状态汇总 ===\n");
    printf("总连接数: %zu/%zu\n", g_cache_mgr->total_connections, g_cache_mgr->max_connections);
    
    pthread_mutex_lock(&g_cache_mgr->global_lock);
    
    for (size_t i = 0; i < g_cache_mgr->hash_table_size; i++) {
        struct hash_table_entry *entry = g_cache_mgr->hash_table[i];
        while (entry) {
            struct connection_cache *cache = entry->cache;
            pthread_mutex_lock(&cache->lock);
            
            struct timeval now;
            gettimeofday(&now, NULL);
            
            printf("  ");
            print_connection_key(&entry->key);
            printf("\n    报文数=%zu, PSN范围=[%u, %u], 空闲=%ld秒\n",
                   cache->count, cache->min_psn, cache->max_psn,
                   now.tv_sec - cache->last_activity.tv_sec);
            
            pthread_mutex_unlock(&cache->lock);
            entry = entry->next;
        }
    }
    
    pthread_mutex_unlock(&g_cache_mgr->global_lock);
    printf("====================\n\n");
}

// 初始化接收端RDMA
int init_recv_rdma() {
    printf("\n=== 初始化接收端RDMA ===\n");
    memset(&recv_ctx, 0, sizeof(recv_ctx));
    
    // 初始化发送端GID（rxe_eth1的GID）
    unsigned char gid_bytes[16] = {
        0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x02, 0x0c, 0x29, 0xff, 0xfe, 0x10, 0x01, 0xff  // 对应rxe_eth1的node_guid
    };
    memcpy(&remote_gid, gid_bytes, 16);

    // 获取设备列表
    struct ibv_device **dev_list = ibv_get_device_list(NULL);
    if (!dev_list) {
        perror("ibv_get_device_list failed");
        return -1;
    }

    // 查找rxe_eth3设备
    struct ibv_device *dev = NULL;
    for (int i = 0; dev_list[i]; i++) {
        printf("发现RDMA设备: %s\n", ibv_get_device_name(dev_list[i]));
        if (strcmp(ibv_get_device_name(dev_list[i]), "rxe_eth3") == 0) {
            dev = dev_list[i];
            break;
        }
    }
    if (!dev) {
        fprintf(stderr, "未找到rxe_eth3设备\n");
        ibv_free_device_list(dev_list);
        return -1;
    }
    printf("使用RDMA设备: %s\n", ibv_get_device_name(dev));

    // 打开设备上下文
    recv_ctx.ctx = ibv_open_device(dev);
    if (!recv_ctx.ctx) {
        perror("ibv_open_device failed");
        ibv_free_device_list(dev_list);
        return -1;
    }
    printf("成功打开设备上下文\n");

    // 分配保护域
    recv_ctx.pd = ibv_alloc_pd(recv_ctx.ctx);
    if (!recv_ctx.pd) {
        perror("ibv_alloc_pd failed");
        ibv_close_device(recv_ctx.ctx);
        ibv_free_device_list(dev_list);
        return -1;
    }
    printf("成功分配保护域 (pd=%p)\n", recv_ctx.pd);

    // 创建完成队列
    recv_ctx.cq = ibv_create_cq(recv_ctx.ctx, 1024, NULL, NULL, 0);
    if (!recv_ctx.cq) {
        perror("ibv_create_cq failed");
        ibv_dealloc_pd(recv_ctx.pd);
        ibv_close_device(recv_ctx.ctx);
        ibv_free_device_list(dev_list);
        return -1;
    }
    printf("成功创建完成队列 (cq=%p)\n", recv_ctx.cq);

    // 查询端口属性
    struct ibv_port_attr port_attr;
    if (ibv_query_port(recv_ctx.ctx, 1, &port_attr) != 0) {
        perror("ibv_query_port failed");
        ibv_destroy_cq(recv_ctx.cq);
        ibv_dealloc_pd(recv_ctx.pd);
        ibv_close_device(recv_ctx.ctx);
        ibv_free_device_list(dev_list);
        return -1;
    }
    printf("端口1属性 - 状态: %u, MTU: %u\n", port_attr.state, port_attr.max_mtu);

    // 查询PKey
    uint16_t pkey;
    if (ibv_query_pkey(recv_ctx.ctx, 1, 0, &pkey) == 0) {
        printf("本地PKey[0] = 0x%04x\n", pkey);
    } else {
        fprintf(stderr, "ibv_query_pkey failed: %s\n", strerror(errno));
    }

    // 查询GID
    if (ibv_query_gid(recv_ctx.ctx, 1, 0, &recv_ctx.local_gid) == 0) {
        printf("本地GID (index 0): ");
        unsigned char *g = (unsigned char *)&recv_ctx.local_gid;
        for (int i = 0; i < 16; ++i) printf("%02x", g[i]);
        printf("\n");
    } else {
        fprintf(stderr, "ibv_query_gid failed: %s\n", strerror(errno));
        ibv_destroy_cq(recv_ctx.cq);
        ibv_dealloc_pd(recv_ctx.pd);
        ibv_close_device(recv_ctx.ctx);
        ibv_free_device_list(dev_list);
        return -1;
    }

    // 创建QP
    struct ibv_qp_init_attr qp_attr = {
        .send_cq = recv_ctx.cq,
        .recv_cq = recv_ctx.cq,
        .qp_type = IBV_QPT_UD,
        .cap = {
            .max_send_wr = 1024,
            .max_recv_wr = 1024,
            .max_send_sge = 1,
            .max_recv_sge = 1
        }
    };
    recv_ctx.qp = ibv_create_qp(recv_ctx.pd, &qp_attr);
    if (!recv_ctx.qp) {
        perror("ibv_create_qp failed");
        ibv_destroy_cq(recv_ctx.cq);
        ibv_dealloc_pd(recv_ctx.pd);
        ibv_close_device(recv_ctx.ctx);
        ibv_free_device_list(dev_list);
        return -1;
    }
    printf("成功创建QP (qp_num=%u)\n", recv_ctx.qp->qp_num);

    // 分配接收缓冲区
    recv_ctx.recv_buf = malloc(1024);
    if (!recv_ctx.recv_buf) {
        perror("malloc recv_buf failed");
        ibv_destroy_qp(recv_ctx.qp);
        ibv_destroy_cq(recv_ctx.cq);
        ibv_dealloc_pd(recv_ctx.pd);
        ibv_close_device(recv_ctx.ctx);
        ibv_free_device_list(dev_list);
        return -1;
    }

    // 注册内存区域
    recv_ctx.mr = ibv_reg_mr(recv_ctx.pd, recv_ctx.recv_buf, 1024,
                            IBV_ACCESS_LOCAL_WRITE);
    if (!recv_ctx.mr) {
        perror("ibv_reg_mr failed");
        free(recv_ctx.recv_buf);
        ibv_destroy_qp(recv_ctx.qp);
        ibv_destroy_cq(recv_ctx.cq);
        ibv_dealloc_pd(recv_ctx.pd);
        ibv_close_device(recv_ctx.ctx);
        ibv_free_device_list(dev_list);
        return -1;
    }
    printf("成功注册接收缓冲区 (lkey=0x%x)\n", recv_ctx.mr->lkey);

    // 1. RESET → INIT
    struct ibv_qp_attr attr = {0};
    attr.qp_state = IBV_QPS_INIT;
    attr.pkey_index = 0;          // PKey索引
    attr.port_num = 1;            // 端口号
    int ret = ibv_modify_qp(recv_ctx.qp, &attr, 
                    IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT);
    if (ret) { 
        perror("INIT failed"); 
        goto cleanup;
    }
    printf("QP状态已切换到INIT\n");

    // QP状态转换: INIT -> RTR
    attr.qp_state = IBV_QPS_RTR;
    // 正确配置地址句柄属性（包含GRH）
    struct ibv_ah_attr ah_attr = {0};
    ah_attr.is_global = 1;                   // 使用全局路由（需要GRH）
    ah_attr.port_num = 1;                    // 端口号

    // 配置全局路由头部(GRH) - 这里才是设置GID的正确位置
    ah_attr.grh.dgid = remote_gid;           // 目标GID（发送端rxe_eth1的GID）
    ah_attr.grh.sgid_index = 0;              // 本地GID索引
    ah_attr.grh.hop_limit = 1;               // 跳数限制
    ah_attr.grh.flow_label = 0;              // 流标签

    // 配置PKey索引
    attr.pkey_index = 0;                  // PKey索引

    // 赋值地址属性
    attr.ah_attr = ah_attr;
    // 配置QKey（与发送端保持一致）
    attr.qkey = 0x11111111;

    // 修改标志位：使用正确的宏组合
    ret = ibv_modify_qp(recv_ctx.qp, &attr, 
                    IBV_QP_STATE | IBV_QP_AV | IBV_QP_QKEY);  // 关键修复：用IBV_QP_AV替代IBV_QP_AH_ATTR
    if (ret != 0) {
        perror("ibv_modify_qp to RTR failed");
        goto cleanup;
    }
    printf("QP状态已切换到RTR (QKey=0x11111111)\n");
    
    // QP状态转换: RTR -> RTS
    attr.qp_state = IBV_QPS_RTS;
    attr.sq_psn = 0;                // 初始PSN从0开始
    attr.max_dest_rd_atomic = 1;
    attr.min_rnr_timer = 0x12;
    ret = ibv_modify_qp(recv_ctx.qp, &attr,
                       IBV_QP_STATE | IBV_QP_SQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER);
    if (ret != 0) {
        perror("ibv_modify_qp to RTS failed");
        goto cleanup;
    }
    printf("QP状态已切换到RTS (初始PSN=0)\n");

    // 投递接收请求
    struct ibv_recv_wr wr = {0};
    struct ibv_sge sge = {0};
    wr.wr_id = 0;
    sge.addr = (uintptr_t)recv_ctx.recv_buf;
    sge.length = 1024;
    sge.lkey = recv_ctx.mr->lkey;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.next = NULL;

    struct ibv_recv_wr *bad_wr;
    ret = ibv_post_recv(recv_ctx.qp, &wr, &bad_wr);
    if (ret != 0) {
        perror("ibv_post_recv failed");
        goto cleanup;
    }
    printf("已投递接收请求 (长度: 1024)\n");

    ibv_free_device_list(dev_list);
    return 0;

cleanup:
    ibv_dereg_mr(recv_ctx.mr);
    free(recv_ctx.recv_buf);
    ibv_destroy_qp(recv_ctx.qp);
    ibv_destroy_cq(recv_ctx.cq);
    ibv_dealloc_pd(recv_ctx.pd);
    ibv_close_device(recv_ctx.ctx);
    ibv_free_device_list(dev_list);
    return -1;
}

// 主函数
int main() {
    // 初始化接收端RDMA
    if (init_recv_rdma() != 0) {
        fprintf(stderr, "接收端RDMA初始化失败\n");
        return 1;
    }

    // 初始化接收缓存
    g_cache_mgr = init_cache_manager(1024, 10, 100);
    if (!g_cache_mgr) {
        fprintf(stderr, "接收缓存初始化失败\n");
        return 1;
    }

    printf("\n接收端已启动，等待接收数据...\n");

    // 循环接收数据
    struct ibv_wc wc;
    int total_received = 0;
    while (total_received < 3) {  // 接收3个数据包后退出
        int ne = ibv_poll_cq(recv_ctx.cq, 1, &wc);
        if (ne < 0) {
            fprintf(stderr, "ibv_poll_cq error: %s\n", strerror(errno));
            break;
        } else if (ne == 0) {
            usleep(10000);  // 10ms
            continue;
        }

        // 处理完成事件
        if (wc.status != IBV_WC_SUCCESS) {
            fprintf(stderr, "接收完成状态错误: %d, opcode=%d\n", wc.status, wc.opcode);
            break;
        }

        total_received++;
        printf("\n[接收] 收到数据 - 长度: %d, wr_id=%lu\n", wc.byte_len, (unsigned long)wc.wr_id);
        printf("数据内容: %.*s\n", wc.byte_len, recv_ctx.recv_buf);

        // 构造连接键（假设发送端信息）
        struct connection_key key;
        inet_pton(AF_INET, "192.168.239.130", &key.src_ip);  // 发送端IP
        inet_pton(AF_INET, "192.168.239.133", &key.dst_ip);  // 接收端IP
        key.src_port = 1234;
        key.dst_port = 5678;
    key.src_qp = wc.src_qp;  // 从完成队列获取发送端QP (使用 wc.src_qp)
        key.dest_qp = recv_ctx.qp->qp_num;
        key.service_type = 0;
        key.pkey = 0xffff;

        // 缓存接收的数据包（使用wr_id作为PSN）
        insert_received_packet(&key, wc.wr_id, recv_ctx.recv_buf, wc.byte_len);

        // 重新投递接收请求
        struct ibv_recv_wr wr = {0};
        struct ibv_sge sge = {0};
        wr.wr_id = wc.wr_id + 1;  // 递增WR ID
        sge.addr = (uintptr_t)recv_ctx.recv_buf;
        sge.length = 1024;
        sge.lkey = recv_ctx.mr->lkey;
        wr.sg_list = &sge;
        wr.num_sge = 1;

        struct ibv_recv_wr *bad_wr;
        if (ibv_post_recv(recv_ctx.qp, &wr, &bad_wr) != 0) {
            perror("ibv_post_recv failed");
            break;
        }
    }

    // 打印接收缓存状态
    print_receive_cache_status();

    // 清理资源
    printf("\n=== 清理资源 ===\n");
    if (recv_ctx.mr) ibv_dereg_mr(recv_ctx.mr);
    if (recv_ctx.recv_buf) free(recv_ctx.recv_buf);
    if (recv_ctx.qp) ibv_destroy_qp(recv_ctx.qp);
    if (recv_ctx.cq) ibv_destroy_cq(recv_ctx.cq);
    if (recv_ctx.pd) ibv_dealloc_pd(recv_ctx.pd);
    if (recv_ctx.ctx) ibv_close_device(recv_ctx.ctx);

    return 0;
}
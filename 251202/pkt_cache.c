// RDMA报文缓存管理系统（环形缓存+多连接批量处理+每个连接）的的对应1个进程
/*
这个是我复现刘彩霞老师专利的缓存模块，主要处理逻辑如下：

1、控制平面：建立交换机与服务器连接，初始化配置与内存注册；
2、数据平面：流表匹配筛选 RDMA 报文，基于 IP+QP 计算一级哈希流特征；
3、Egress 管道：结合一级哈希 + PSN 生成两级哈希，异或后计算存储地址；
4、存储机制：固定内存块存储数据包，哈希冲突通过链表法处理；
5、连接隔离：每个连接独立内存空间，动态分配缓存资源。


编译命令：
gcc pkt_cache.c -o pkt_cache -lpthread -lrdmacm -libverbs

运行命令：
sudo ./pkt_cache
*/



// RDMA报文缓存管理系统（专利CN120034507A复现版）
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
#include <stdint.h>

// 配置参数（适配虚拟机资源）
#define MAX_CONNECTIONS 5            // 最大连接数（专利架构，动态分配）
#define RING_BUFFER_SIZE 500         // 每个连接环形缓冲区（固定内存块）
#define MAX_PACKET_SIZE 4096         // 固定数据包内存块大小
#define BATCH_TIMEOUT_MS 10          // 批量处理超时（专利批量机制）
#define BATCH_THRESHOLD 200          // 批量阈值（适配虚拟机）
#define CRC32_POLYNOMIAL 0xEDB88320L  // 专利指定CRC32多项式
#define CUSTOM_CRC_POLYNOMIAL 0x04C11DB7L  // 专利自定义CRC多项式

// 连接标识键（专利：IP+QP唯一标识）
struct connection_key {
    uint32_t src_ip;
    uint32_t dst_ip;
    uint16_t src_port;  // 新增：源端口
    uint16_t dst_port;  // 新增：目的端口（4791）
    uint32_t src_qp;
    uint32_t dest_qp;
};

// 缓存的数据包（专利：固定内存块存储）
struct cached_packet {
    unsigned char data[MAX_PACKET_SIZE];
    int data_len;
    uint32_t psn;
    int valid;
    struct timeval timestamp;
    struct cached_packet *next;  // 链表指针
};

// 单个连接的缓存（专利：连接级动态分配）
struct connection_cache {
    struct cached_packet ring[RING_BUFFER_SIZE];  // 固定内存块数组
    uint32_t base_hash;                           // 一级哈希流特征
    uint32_t min_psn;
    uint32_t max_psn;
    size_t total_bytes;
    size_t packet_count;
    struct timeval last_activity;
    pthread_mutex_t lock;
};

// 哈希表节点（专利：多级哈希架构）
struct hash_entry {
    struct connection_key key;
    pid_t process_id;                // 每个连接独立进程（专利隔离机制）
    int shm_id;                      // 共享内存ID
    int msg_queue_id;                // 消息队列ID
    struct connection_cache *cache;  // 缓存地址
    struct hash_entry *next;         // 链表法处理哈希冲突（专利要求）
};

// 缓存管理器（专利控制平面核心）
struct cache_manager {
    struct hash_entry **hash_table;
    size_t hash_table_size;
    size_t total_connections;
    pthread_mutex_t global_lock;
    // 专利元数据（内存注册信息）
    uint64_t reg_mem_addr;
    uint32_t rkey;
};

// 全局缓存管理器（专利单例模式）
struct cache_manager *g_cache_mgr = NULL;

// 消息队列结构（专利批量传输机制）
struct packet_msg {
    struct connection_key key;
    uint32_t psn;
    unsigned char data[MAX_PACKET_SIZE];
    int data_len;
    int processed;  // 0=未处理, 1=已处理
};

// 专利要求：CRC32算法（第一级哈希）
static uint32_t crc32_calculate(const unsigned char *data, size_t len) {
    uint32_t crc = 0xFFFFFFFFL;
    while (len--) {
        crc ^= (uint32_t)*data++;
        for (int i = 0; i < 8; i++) {
            crc = (crc >> 1) ^ ((crc & 1) ? CRC32_POLYNOMIAL : 0);
        }
    }
    return ~crc;
}

// 专利要求：自定义CRC多项式（第二级哈希）
static uint32_t custom_crc_calculate(const unsigned char *data, size_t len) {
    uint32_t crc = 0xFFFFFFFFL;
    while (len--) {
        crc ^= (uint32_t)*data++;
        for (int i = 0; i < 8; i++) {
            crc = (crc >> 1) ^ ((crc & 1) ? CUSTOM_CRC_POLYNOMIAL : 0);
        }
    }
    return ~crc;
}

// 一级哈希：基于IP+QP计算流特征（专利S202）
static uint32_t calculate_flow_hash(const struct connection_key *key) {
    unsigned char hash_data[20];  // 5*4字节=20字节（五元组）
    memcpy(hash_data, &key->src_ip, 4);
    memcpy(hash_data+4, &key->dst_ip, 4);
    memcpy(hash_data+8, &key->src_port, 2);  // 加入源端口
    memcpy(hash_data+10, &key->dst_port, 2); // 加入目的端口
    memcpy(hash_data+12, &key->src_qp, 4);
    memcpy(hash_data+16, &key->dest_qp, 4);
    return custom_crc_calculate(hash_data, 20);  // 专利自定义CRC
}

// 二级哈希：结合流特征+PSN生成存储地址（专利S301-S302）
static size_t calculate_storage_addr(struct connection_cache *cache, uint32_t psn) {
    // 第一级哈希：流特征+PSN的CRC32
    unsigned char hash_data[8];
    memcpy(hash_data, &cache->base_hash, 4);
    memcpy(hash_data+4, &psn, 4);
    uint32_t hash1 = crc32_calculate(hash_data, 8);
    
    // 第二级哈希：流特征+PSN的自定义CRC
    uint32_t hash2 = custom_crc_calculate(hash_data, 8);
    
    // 专利：异或合成+高位截取（低12位清零，保留高20位）
    uint32_t combined_hash = hash1 ^ hash2;
    combined_hash &= 0xFFFFF000;  // 低12位清零
    combined_hash >>= 12;          // 保留高20位
    
    // 映射到环形缓冲区索引
    return combined_hash % RING_BUFFER_SIZE;
}

// 连接键比较（专利连接唯一标识）
int connection_keys_equal(const struct connection_key *a, const struct connection_key *b) {
    return (a->src_ip == b->src_ip &&
            a->dst_ip == b->dst_ip &&
            a->src_qp == b->src_qp &&
            a->dest_qp == b->dest_qp);
}

// 创建连接键（专利S202）
struct connection_key create_connection_key(const char *src_ip, const char *dst_ip,
                                          uint16_t src_port, uint16_t dst_port,  
                                          uint32_t src_qp, uint32_t dest_qp) {
    struct connection_key key;
    memset(&key, 0, sizeof(key));
    inet_pton(AF_INET, src_ip, &key.src_ip);
    inet_pton(AF_INET, dst_ip, &key.dst_ip);
    key.src_port = src_port;  // 赋值端口
    key.dst_port = dst_port;
    key.src_qp = src_qp;
    key.dest_qp = dest_qp;
    return key;
}

// 创建连接缓存（专利：连接级动态分配）
struct connection_cache* create_connection_cache(const struct connection_key *key) {
    struct connection_cache *cache = malloc(sizeof(struct connection_cache));
    if (!cache) return NULL;
    
    memset(cache, 0, sizeof(struct connection_cache));
    cache->base_hash = calculate_flow_hash(key);  // 初始化一级哈希
    cache->min_psn = UINT32_MAX;
    cache->max_psn = 0;
    cache->total_bytes = 0;
    cache->packet_count = 0;
    gettimeofday(&cache->last_activity, NULL);
    
    if (pthread_mutex_init(&cache->lock, NULL) != 0) {
        free(cache);
        return NULL;
    }
    
    // 初始化固定内存块（专利：预分配固定大小）
    for (int i = 0; i < RING_BUFFER_SIZE; i++) {
        cache->ring[i].valid = 0;
        cache->ring[i].data_len = 0;
    }
    
    return cache;
}

// 插入数据包到缓存（专利S304存储流程）
// 修改insert_packet函数，处理哈希冲突
int insert_packet(struct connection_cache *cache, uint32_t psn, 
                 const unsigned char *data, int data_len) {
    if (!cache || !data || data_len <= 0 || data_len > MAX_PACKET_SIZE) {
        return -1;
    }
    
    pthread_mutex_lock(&cache->lock);
    
    // 计算存储地址
    size_t index = calculate_storage_addr(cache, psn);
    struct cached_packet *packet = &cache->ring[index];
    
    // 如果当前位置已有数据且PSN不同，使用链表法
    if (packet->valid && packet->psn != psn) {
        // 创建新节点
        struct cached_packet *new_node = malloc(sizeof(struct cached_packet));
        if (!new_node) {
            pthread_mutex_unlock(&cache->lock);
            return -1;
        }
        
        // 将新节点插入链表头部
        memcpy(new_node->data, data, data_len);
        new_node->data_len = data_len;
        new_node->psn = psn;
        new_node->valid = 1;
        gettimeofday(&new_node->timestamp, NULL);
        new_node->next = packet->next;
        packet->next = new_node;
        
        printf("哈希冲突：索引%zu已有PSN=%u，新PSN=%u使用链表存储\n", 
               index, packet->psn, psn);
    } else {
        // 直接存储或覆盖
        if (packet->valid) {
            cache->total_bytes -= packet->data_len;
            cache->packet_count--;
        }
        
        memcpy(packet->data, data, data_len);
        packet->data_len = data_len;
        packet->psn = psn;
        packet->valid = 1;
        gettimeofday(&packet->timestamp, NULL);
    }
    
    cache->total_bytes += data_len;
    cache->packet_count++;
    if (psn < cache->min_psn) cache->min_psn = psn;
    if (psn > cache->max_psn) cache->max_psn = psn;
    gettimeofday(&cache->last_activity, NULL);
    
    pthread_mutex_unlock(&cache->lock);
    return 0;
}

// 哈希函数（专利哈希表索引）
static uint32_t hash_table_calculate(const struct connection_key *key, size_t table_size) {
    return (key->src_ip + key->dst_ip + key->src_qp + key->dest_qp) % table_size;
}

// 添加一个辅助函数来查找哈希表项
struct hash_entry* find_hash_entry(struct cache_manager *mgr, const struct connection_key *key) {
    if (!mgr || !key) return NULL;
    
    uint32_t hash_index = hash_table_calculate(key, mgr->hash_table_size);
    
    // 遍历哈希链查找匹配的连接
    struct hash_entry *entry = mgr->hash_table[hash_index];
    while (entry) {
        if (connection_keys_equal(&entry->key, key)) {
            return entry;
        }
        entry = entry->next;
    }
    
    return NULL;
}

// 查找数据包（专利重传快速查找）
struct cached_packet* find_packet(struct connection_cache *cache, uint32_t psn) {
    if (!cache) {
        printf("查找失败: cache为空\n");
        return NULL;
    }
    
    pthread_mutex_lock(&cache->lock);
    
    // 计算存储地址
    size_t index = calculate_storage_addr(cache, psn);
    printf("查找PSN=%u, 计算地址索引=%zu\n", psn, index);
    
    struct cached_packet *packet = &cache->ring[index];
    
    if (!packet->valid) {
        printf("查找失败: 索引%zu无效\n", index);
        pthread_mutex_unlock(&cache->lock);
        return NULL;
    }
    
    if (packet->psn != psn) {
        printf("查找失败: 索引%zu存储的是PSN=%u，不是目标PSN=%u\n", 
               index, packet->psn, psn);
        pthread_mutex_unlock(&cache->lock);
        return NULL;
    }
    
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

// 连接处理进程（专利：每个连接独立进程）
static void connection_process(struct connection_cache *cache, int msg_queue_id) {
    struct packet_msg *msg_queue = (struct packet_msg*)shmat(msg_queue_id, NULL, 0);
    if (msg_queue == (void*)-1) {
        perror("shmat failed");
        exit(EXIT_FAILURE);
    }

    struct timeval last_process_time;
    gettimeofday(&last_process_time, NULL);

    while (1) {
        int processed_count = 0;
        
        // 批量处理（专利批量机制：阈值+超时）
        for (int i = 0; i < BATCH_THRESHOLD; i++) {
            if (msg_queue[i].processed == 0 && msg_queue[i].data_len > 0) {
                insert_packet(cache, msg_queue[i].psn, 
                             msg_queue[i].data, msg_queue[i].data_len);
                msg_queue[i].processed = 1;
                processed_count++;
            } else if (msg_queue[i].processed == 1) {
                memset(&msg_queue[i], 0, sizeof(struct packet_msg));  // 清理复用
            }
        }
        
        // 超时处理（专利S303：定时批量）
        struct timeval now;
        gettimeofday(&now, NULL);
        long elapsed_ms = (now.tv_sec - last_process_time.tv_sec) * 1000 +
                          (now.tv_usec - last_process_time.tv_usec) / 1000;
        
        if (processed_count > 0) {
            last_process_time = now;
        } else if (elapsed_ms >= BATCH_TIMEOUT_MS) {
            // 超时强制处理（专利避免慢连接阻塞）
            last_process_time = now;
        }
        
        // 适配虚拟机：控制CPU占用
        usleep(processed_count > 0 ? 100 : 1000);
    }

    shmdt(msg_queue);
    exit(EXIT_SUCCESS);
}

// 查找空闲消息队列位置
static int get_free_msg_slot(struct packet_msg *msg_queue) {
    for (int i = 0; i < BATCH_THRESHOLD; i++) {
        if (msg_queue[i].processed == 0) {
            return i;
        }
    }
    return -1;
}



// 控制平面：获取或创建连接（专利S10-S102）
struct hash_entry* get_or_create_hash_entry(struct cache_manager *mgr, 
                                           const struct connection_key *key) {
    uint32_t hash_index = hash_table_calculate(key, mgr->hash_table_size);
    
    // 查找现有连接（哈希冲突用链表遍历）
    struct hash_entry *entry = mgr->hash_table[hash_index];
    while (entry) {
        if (connection_keys_equal(&entry->key, key)) {
            gettimeofday(&entry->cache->last_activity, NULL);
            return entry;
        }
        entry = entry->next;
    }
    
    // 连接数限制（适配虚拟机）
    if (mgr->total_connections >= MAX_CONNECTIONS) {
        printf("专利架构：达到最大连接数限制 (%zu/%d)\n", mgr->total_connections, MAX_CONNECTIONS);
        return NULL;
    }
    
    // 消息队列共享内存（专利批量传输）
    int msg_queue_id = shmget(IPC_PRIVATE, sizeof(struct packet_msg) * BATCH_THRESHOLD, 0666 | IPC_CREAT);
    if (msg_queue_id == -1) {
        perror("shmget failed for msg queue");
        return NULL;
    }
    
    // 初始化消息队列
    struct packet_msg *msg_queue = (struct packet_msg*)shmat(msg_queue_id, NULL, 0);
    if (msg_queue == (void*)-1) {
        perror("shmat failed for msg queue");
        shmctl(msg_queue_id, IPC_RMID, NULL);
        return NULL;
    }
    memset(msg_queue, 0, sizeof(struct packet_msg) * BATCH_THRESHOLD);
    shmdt(msg_queue);
    
    // 缓存共享内存（专利动态分配）
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
    
    // 初始化缓存（专利连接级配置）
    new_cache = create_connection_cache(key);
    if (!new_cache) {
        shmdt(new_cache);
        shmctl(shm_id, IPC_RMID, NULL);
        shmctl(msg_queue_id, IPC_RMID, NULL);
        return NULL;
    }
    
    // 创建独立进程（专利连接隔离）
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork failed");
        pthread_mutex_destroy(&new_cache->lock);
        free(new_cache);
        shmctl(shm_id, IPC_RMID, NULL);
        shmctl(msg_queue_id, IPC_RMID, NULL);
        return NULL;
    } else if (pid == 0) {
        connection_process(new_cache, msg_queue_id);
        exit(EXIT_SUCCESS);
    }
    
    // 创建哈希表项（专利哈希表管理）
    struct hash_entry *new_entry = malloc(sizeof(struct hash_entry));
    if (!new_entry) {
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        pthread_mutex_destroy(&new_cache->lock);
        free(new_cache);
        shmctl(shm_id, IPC_RMID, NULL);
        shmctl(msg_queue_id, IPC_RMID, NULL);
        return NULL;
    }
    
    new_entry->key = *key;
    new_entry->process_id = pid;
    new_entry->shm_id = shm_id;
    new_entry->cache = new_cache;
    new_entry->msg_queue_id = msg_queue_id;
    new_entry->next = mgr->hash_table[hash_index];  // 链表法处理冲突
    mgr->hash_table[hash_index] = new_entry;
    mgr->total_connections++;
    
    // 专利要求：打印连接元数据
    struct in_addr src_in_addr, dst_in_addr;
    src_in_addr.s_addr = key->src_ip;
    dst_in_addr.s_addr = key->dst_ip;
    
    printf("专利架构：创建新连接(进程ID: %d): %s QP%u -> %s QP%u\n",
           pid,
           inet_ntoa(src_in_addr), key->src_qp,
           inet_ntoa(dst_in_addr), key->dest_qp);
    
    return new_entry;
}

// 批量插入数据包（专利S304）
// 修改add_to_batch_queue函数，使用正确的消息队列ID
int add_to_batch_queue(const char *src_ip, const char *dst_ip,
                      uint32_t src_qp, uint32_t dest_qp,
                      uint32_t psn, const unsigned char *app_data, int data_len) {
    if (!g_cache_mgr || !app_data || data_len <= 0 || data_len > MAX_PACKET_SIZE) {
        return -1;
    }
    
    struct connection_key key = create_connection_key(src_ip, dst_ip, src_qp, dest_qp);
    
    pthread_mutex_lock(&g_cache_mgr->global_lock);
    
    // 查找现有连接的消息队列ID
    struct hash_entry *entry = find_hash_entry(g_cache_mgr, &key);
    if (!entry) {
        // 创建新连接
        entry = get_or_create_hash_entry(g_cache_mgr, &key);
        if (!entry) {
            pthread_mutex_unlock(&g_cache_mgr->global_lock);
            return -1;
        }
    }
    
    // 使用已存在的消息队列ID
    struct packet_msg *msg_queue = (struct packet_msg*)shmat(entry->msg_queue_id, NULL, 0);
    if (msg_queue == (void*)-1) {
        perror("shmat failed");
        pthread_mutex_unlock(&g_cache_mgr->global_lock);
        return -1;
    }
    
    // 查找空闲位置
    int slot = get_free_msg_slot(msg_queue);
    if (slot == -1) {
        printf("消息队列满，稍后重试 PSN=%u\n", psn);
        shmdt(msg_queue);
        pthread_mutex_unlock(&g_cache_mgr->global_lock);
        usleep(100);
        return add_to_batch_queue(src_ip, dst_ip, src_qp, dest_qp, psn, app_data, data_len);
    }
    
    // 填充消息
    memcpy(msg_queue[slot].data, app_data, data_len);
    msg_queue[slot].data_len = data_len;
    msg_queue[slot].psn = psn;
    msg_queue[slot].processed = 0;
    
    shmdt(msg_queue);
    pthread_mutex_unlock(&g_cache_mgr->global_lock);
    return 0;
}

// 重传查找接口（专利快速查找）
struct cached_packet* find_packet_by_psn(const char *src_ip, const char *dst_ip,
                                        uint32_t src_qp, uint32_t dest_qp,
                                        uint32_t psn) {
    if (!g_cache_mgr) return NULL;
    
    struct connection_key key = create_connection_key(src_ip, dst_ip, src_qp, dest_qp);
    uint32_t hash_index = hash_table_calculate(&key, g_cache_mgr->hash_table_size);
    
    pthread_mutex_lock(&g_cache_mgr->global_lock);
    
    // 哈希表查找连接
    struct hash_entry *entry = g_cache_mgr->hash_table[hash_index];
    while (entry) {
        if (connection_keys_equal(&entry->key, &key)) {
            pthread_mutex_unlock(&g_cache_mgr->global_lock);
            return find_packet(entry->cache, psn);
        }
        entry = entry->next;  // 链表遍历处理冲突
    }
    
    pthread_mutex_unlock(&g_cache_mgr->global_lock);
    return NULL;
}

// 控制平面：初始化缓存管理器（专利S10）
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
    mgr->reg_mem_addr = 0;
    mgr->rkey = 0;
    
    if (pthread_mutex_init(&mgr->global_lock, NULL) != 0) {
        free(mgr->hash_table);
        free(mgr);
        return NULL;
    }
    
    printf("专利架构：初始化缓存管理器 - 哈希表大小=%zu, 最大连接数=%d\n",
           hash_size, MAX_CONNECTIONS);
    return mgr;
}

// 销毁缓存管理器（专利资源释放）
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
            
            // 释放资源
            if (entry->cache) {
                pthread_mutex_destroy(&entry->cache->lock);
                free(entry->cache);
            }
            if (entry->shm_id != 0) {
                shmctl(entry->shm_id, IPC_RMID, NULL);
            }
            
            free(entry);
            entry = next;
        }
    }
    
    free(mgr->hash_table);
    pthread_mutex_destroy(&mgr->global_lock);
    free(mgr);
}

// 打印连接状态（专利元数据监控）
void print_all_connections_status() {
    if (!g_cache_mgr) {
        printf("专利架构：缓存管理器未初始化\n");
        return;
    }
    
    printf("\n===== 专利架构 - 连接状态 =====\n");
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
            
            printf("连接(进程ID: %d): %s QP%u -> %s QP%u\n",
                   entry->process_id,
                   inet_ntoa(src_in_addr), key->src_qp,
                   inet_ntoa(dst_in_addr), key->dest_qp);
            
            pthread_mutex_lock(&cache->lock);
            printf("  PSN范围: %u-%u, 缓存数据包数: %zu, 缓存大小: %zu bytes\n",
                   cache->min_psn == UINT32_MAX ? 0 : cache->min_psn,
                   cache->max_psn,
                   cache->packet_count,
                   cache->total_bytes);
            pthread_mutex_unlock(&cache->lock);
            
            entry = entry->next;
        }
    }
    
    pthread_mutex_unlock(&g_cache_mgr->global_lock);
    printf("==============================\n");
}


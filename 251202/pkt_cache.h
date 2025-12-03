#ifndef PKT_CACHE_H
#define PKT_CACHE_H

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
extern struct cache_manager *g_cache_mgr;

// 消息队列结构（专利批量传输机制）
struct packet_msg {
    struct connection_key key;
    uint32_t psn;
    unsigned char data[MAX_PACKET_SIZE];
    int data_len;
    int processed;  // 0=未处理, 1=已处理
};

struct hash_entry* find_hash_entry(struct cache_manager *mgr, const struct connection_key *key);

// 专利要求：CRC32算法（第一级哈希）
static uint32_t crc32_calculate(const unsigned char *data, size_t len);

// 专利要求：自定义CRC多项式（第二级哈希）
static uint32_t custom_crc_calculate(const unsigned char *data, size_t len);

// 一级哈希：基于IP+QP计算流特征（专利S202）
static uint32_t calculate_flow_hash(const struct connection_key *key);

// 二级哈希：结合流特征+PSN生成存储地址（专利S301-S302）
static size_t calculate_storage_addr(struct connection_cache *cache, uint32_t psn);

// 连接键比较（专利连接唯一标识）
int connection_keys_equal(const struct connection_key *a, const struct connection_key *b);

// 创建连接键（专利S202）
struct connection_key create_connection_key(const char *src_ip, const char *dst_ip, uint32_t src_qp, uint32_t dest_qp);

// 创建连接缓存（专利：连接级动态分配）
struct connection_cache* create_connection_cache(const struct connection_key *key);

// 插入数据包到缓存（专利S304存储流程）
int insert_packet(struct connection_cache *cache, uint32_t psn, const unsigned char *data, int data_len);

// 查找数据包（专利重传快速查找）
struct cached_packet* find_packet(struct connection_cache *cache, uint32_t psn);

// 连接处理进程（专利：每个连接独立进程）
static void connection_process(struct connection_cache *cache, int msg_queue_id);

// 哈希函数（专利哈希表索引）
static uint32_t hash_table_calculate(const struct connection_key *key, size_t table_size);

// 批量插入数据包（专利S304）
int add_to_batch_queue(const char *src_ip, const char *dst_ip,
                      uint32_t src_qp, uint32_t dest_qp,
                      uint32_t psn, const unsigned char *app_data, int data_len);

// 重传查找接口（专利快速查找）
struct cached_packet* find_packet_by_psn(const char *src_ip, const char *dst_ip, uint32_t src_qp, uint32_t dest_qp, uint32_t psn);

// 控制平面：初始化缓存管理器（专利S10）
struct cache_manager* init_cache_manager(size_t hash_size);

// 销毁缓存管理器（专利资源释放）
void destroy_cache_manager(struct cache_manager *mgr);

// 打印连接状态（专利元数据监控）
void print_all_connections_status();



#endif // PKT_CACHE_H
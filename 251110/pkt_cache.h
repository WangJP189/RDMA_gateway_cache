#ifndef PKT_CACHE_H
#define PKT_CACHE_H

#include <pthread.h>
#include <sys/time.h>
#include <stdint.h>
#include <arpa/inet.h>

// 缓存报文结构（声明）
struct cached_packet {
    unsigned char *app_data;          // 应用数据载荷
    int data_len;                     // 数据长度
    uint32_t dest_qp;                 // 目标QP号
    uint32_t psn;                     // 包序列号
    struct timeval timestamp;         // 缓存时间戳
    struct cached_packet *next;       // 下一个节点（按PSN排序）
    struct cached_packet *prev;       // 前一个节点（双向链表）
};

// 每个连接的缓存队列（声明）
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

// IPv4连接标识键（声明）
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

// 哈希表节点（声明）
struct hash_table_entry {
    struct connection_key key;        // 连接标识
    struct connection_cache *cache;   // 对应的缓存队列
    struct hash_table_entry *next;    // 哈希冲突链表
};

// 全局缓存管理器（声明）
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

// 全局缓存管理器实例（外部可见）
extern struct cache_manager *g_cache_mgr;

// 哈希计算函数
uint32_t calculate_hash(const struct connection_key *key, size_t table_size);

// 连接键比较
int connection_keys_equal(const struct connection_key *a, const struct connection_key *b);

// 创建连接键
struct connection_key create_connection_key(const char *src_ip, const char *dst_ip,
                                            uint16_t src_port, uint16_t dst_port,
                                            uint32_t src_qp, uint32_t dest_qp,
                                            uint8_t service_type, uint16_t pkey);

// 打印连接键信息
void print_connection_key(const struct connection_key *key);

// 初始化缓存管理器
struct cache_manager* init_cache_manager(size_t hash_size, 
                                        size_t max_conns,
                                        size_t max_packets_per_conn,
                                        size_t max_bytes_per_conn_mb,
                                        int conn_timeout_seconds);

// 创建连接缓存
struct connection_cache* create_connection_cache(uint32_t window_size);

// 销毁连接缓存
void destroy_connection_cache(struct connection_cache *cache);

// 获取或创建连接缓存
struct connection_cache* get_or_create_connection_cache(
    struct cache_manager *mgr, const struct connection_key *key,
    uint32_t window_size);

// 按PSN有序插入报文
int insert_packet_sorted(struct connection_cache *cache, 
                         struct cached_packet *new_packet);

// 添加报文到连接缓存
int add_to_connection_cache(const char *src_ip, const char *dst_ip,
                           uint16_t src_port, uint16_t dst_port,
                           uint32_t src_qp, uint32_t dest_qp,
                           uint8_t service_type, uint16_t pkey,
                           uint32_t psn, const unsigned char *app_data, int data_len);

// 按PSN范围查找报文
struct cached_packet* find_packets_by_psn_range(const struct connection_key *key,
                                               uint32_t start_psn, uint32_t end_psn,
                                               int *found_count);

// 更新滑动窗口起始位置
int update_window_start(const struct connection_key *key, uint32_t new_start);

// 处理NACK批量重传
void handle_nack_batch_retransmit(const struct connection_key *key,
                                  uint32_t nack_epsn);

// 清理过期连接
void cleanup_expired_connections();

// 打印所有连接状态
void print_all_connections_status();

// 重传RDMA报文（占位函数）
void retransmit_rdma_packet(const struct connection_key *key, 
                           const unsigned char *data, int len, 
                           uint32_t dest_qp, uint32_t psn);

#endif  // PKT_CACHE_H
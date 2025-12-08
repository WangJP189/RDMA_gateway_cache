#ifndef PKT_CACHE_H
#define PKT_CACHE_H

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

// 批量处理队列
struct batch_queue {
    struct batch_node *head;      // 队列头
    struct batch_node *tail;      // 队列尾
    size_t count;                 // 队列大小
    pthread_mutex_t lock;         // 队列锁
    pthread_cond_t cond;          // 条件变量
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

// 缓存管理器
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
    pthread_t *batch_threads;     // 批量处理线程数组
    int batch_running;            // 批量线程运行标志
    size_t batch_threshold;       // 批量处理阈值
    int batch_timeout;            // 批量处理超时时间（毫秒）
    size_t num_batch_threads;     // 批量线程数量

    // 滑动窗口配置
    uint32_t default_window_size; // 默认窗口大小
};

// 全局缓存管理器
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

// 创建连接的环形缓存
struct connection_ring_cache* create_ring_cache(size_t ring_size, uint32_t first_psn, uint32_t window_size);

// 销毁连接缓存
void destroy_ring_cache(struct connection_ring_cache *cache);

// 将数据包插入环形缓存
int insert_packet_to_ring(struct connection_ring_cache *cache, 
                         uint32_t psn, const unsigned char *app_data, 
                         int data_len, uint32_t dest_qp);

// 获取或创建连接缓存
struct connection_ring_cache* get_or_create_ring_cache(
    struct cache_manager *mgr, const struct connection_key *key,
    uint32_t first_psn, pid_t *pid);

// 根据PSN查找数据包
struct cached_packet* find_packet_by_psn(const struct connection_key *key, uint32_t psn);

// 确认PSN（触发窗口滑动）
int acknowledge_psn(const struct connection_key *key, uint32_t ack_psn);

// 处理NACK重传
void handle_nack_retransmit(const struct connection_key *key, uint32_t nack_psn);

// 添加数据包到批量处理队列
int add_to_batch_queue(const char *src_ip, const char *dst_ip,
                      uint16_t src_port, uint16_t dst_port,
                      uint32_t src_qp, uint32_t dest_qp,
                      uint8_t service_type, uint16_t pkey,
                      uint32_t psn, const unsigned char *app_data, int data_len);

// 启动批量处理线程
int start_batch_processor(struct cache_manager *mgr);

// 停止批量处理线程
void stop_batch_processor(struct cache_manager *mgr);

// 清理过期连接
void cleanup_expired_connections();

// 初始化缓存管理器
struct cache_manager* init_cache_manager(size_t hash_size, 
                                        size_t max_conns,
                                        size_t ring_size,
                                        size_t max_bytes_per_conn_mb,
                                        int conn_timeout_seconds,
                                        size_t batch_threshold,
                                        int batch_timeout_ms,
                                        uint32_t window_size,
                                        size_t num_batch_threads);

// 销毁缓存管理器
void destroy_cache_manager(struct cache_manager *mgr);

// 打印所有连接状态
void print_all_connections_status();

#endif // PKT_CACHE_H
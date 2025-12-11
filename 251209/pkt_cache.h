#ifndef PKT_CACHE_H
#define PKT_CACHE_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/time.h>
#include <arpa/inet.h>

// 配置参数
#define MTU_SIZE 4096              // MTU大小
#define MEM_BLOCK_SIZE (MTU_SIZE)  // 内存块大小
#define BUFFER_CAPACITY 100        // buffer容量(阈值的2倍)
#define BATCH_THRESHOLD 50         // 批量处理阈值
#define BATCH_TIMEOUT_MS 10        // 批量处理超时(ms)
#define INIT_MEM_SIZE 1024         // 初始内存块数量
#define EXTEND_MEM_SIZE 512        // 内存扩展块数量


// 缓存报文结构
struct cached_packet {
    unsigned char data[MEM_BLOCK_SIZE];  // 数据块
    int data_len;                        // 实际数据长度
    uint32_t psn;                        // 包序列号
    int valid;                           // 有效性标记
};

// 连接标识键（五元组）
struct connection_key {
    uint32_t src_ip;      // 源IP
    uint32_t dst_ip;      // 目的IP
    uint16_t src_port;    // 源端口
    uint16_t dst_port;    // 目的端口
    uint32_t qp;          // QP信息
};

// 批量处理缓冲区
struct batch_buffer {
    struct cached_packet packets[BUFFER_CAPACITY];  // 数据包缓冲区
    int head;                     // 头指针
    int tail;                     // 尾指针
    int count;                    // 当前数量
    struct timeval last_flush;    // 上次刷新时间
    uint32_t base_psn;            // 基准PSN
    pthread_mutex_t lock;         // 缓冲区锁
};

// 线性内存区
struct linear_memory {
    struct cached_packet *blocks;  // 内存块数组
    size_t capacity;               // 总容量
    size_t used;                   // 已使用数量
    struct linear_memory *next;    // 下一段内存(用于扩展)
    pthread_mutex_t lock;          // 内存锁
};

// 单个连接的缓存管理结构
struct connection_cache {
    struct connection_key key;     // 连接键
    struct batch_buffer buffer;    // 批量缓冲区
    struct linear_memory *memory;  // 线性内存区
    uint32_t min_psn;              // 最小PSN
    uint32_t max_psn;              // 最大PSN
    pthread_t flush_thread;        // 定时刷新线程
    int running;                   // 运行标志
};

// 哈希表条目结构
struct hash_table_entry {
    struct connection_key key;
    struct connection_cache *cache;
    struct hash_table_entry *next;  // 链表解决哈希冲突
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
    struct connection_cache **conn_caches;  // 连接缓存数组(用于测试)
};

// 全局缓存管理器
extern struct cache_manager *g_cache_mgr;

// 计算初始内存地址（基于五元组）
uint32_t calculate_initial_addr(const struct connection_key *key);

// 初始化批量缓冲区
int init_batch_buffer(struct batch_buffer *buffer);

// 初始化线性内存区
struct linear_memory* init_linear_memory(size_t capacity);

// 扩展线性内存区
int extend_linear_memory(struct linear_memory *mem);

// 创建连接缓存
struct connection_cache* create_connection_cache(const struct connection_key *key);

// 销毁连接缓存
void destroy_connection_cache(struct connection_cache *cache);

// 插入数据包到缓冲区
int insert_to_buffer(struct connection_cache *cache, uint32_t psn, 
                    const unsigned char *data, int data_len);

// 刷新缓冲区到内存
int flush_buffer_to_memory(struct connection_cache *cache);

// 定时刷新线程函数
void* batch_flush_thread(void *arg);

// 根据ePSN处理重传请求（清空旧包并提取需重传包）
int process_retransmit_request(struct connection_cache *cache, uint32_t ePSN,
                              struct cached_packet **retrans_pkts, size_t *count);


// 补充的函数声明
struct cache_manager* init_cache_manager(size_t max_conns);
void destroy_cache_manager(struct cache_manager *mgr);
int add_packet_to_cache(const char *src_ip, const char *dst_ip,
                       uint16_t src_port, uint16_t dst_port,
                       uint32_t src_qp, uint32_t dest_qp,
                       uint32_t psn, const unsigned char *data, int data_len);
void print_all_connections_status();
void print_connection_status(struct connection_cache *cache);


uint32_t ip_str_to_uint(const char *ip);

#endif // PKT_CACHE_H
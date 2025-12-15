#ifndef PKT_CACHE_H
#define PKT_CACHE_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/time.h>
#include <arpa/inet.h>

// 配置参数
#define MEM_BLOCK_SIZE 5120        // 内存块大小固定为5KB
#define RING_BUFFER_SIZE 1000      // 环形数组大小
#define INIT_MEM_POOL_SIZE 100     // 初始内存池大小


// 缓存报文结构
struct cached_packet {
    unsigned char data[MEM_BLOCK_SIZE];  // 数据块(5KB)
    int data_len;                        // 实际数据长度
    uint32_t psn;                        // 包序列号
};

// 连接标识键（五元组）
struct connection_key {
    uint32_t src_ip;      // 源IP(网络字节序)
    uint32_t dst_ip;      // 目的IP(网络字节序)
    uint16_t src_port;    // 源端口(网络字节序)
    uint16_t dst_port;    // 目的端口(网络字节序)
    uint32_t qp;          // QP号
};

// 单个连接的缓存管理结构
struct connection_cache {
    struct connection_key key;               // 连接键
    struct cached_packet* packet_ptr_ring[RING_BUFFER_SIZE];  // 记录内存指针的环形数组
    uint32_t start_psn;                      // 当前环形数组中有效的最小PSN
    uint32_t end_psn;                        // 当前环形数组中有效的最大PSN
    size_t ring_used_count;                  // 环形数组中已使用的元素数量
    pthread_mutex_t ring_lock;               // 环形数组操作锁
    pthread_mutex_t mem_lock;                // 内存操作锁
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
    size_t max_connections;                 // 最大连接数
    size_t total_connections;               // 总连接数
    struct connection_cache **conn_caches;  // 连接缓存数组(用于测试)
    pthread_mutex_t global_lock;            // 全局锁
};

// 全局缓存管理器
extern struct cache_manager *g_cache_mgr;

// 初始化缓存管理器
struct cache_manager* init_cache_manager(size_t max_conns);

// 销毁缓存管理器
void destroy_cache_manager(struct cache_manager *mgr);

// 添加数据包到缓存
int add_packet_to_cache(const char *src_ip, const char *dst_ip,
                       uint16_t src_port, uint16_t dst_port,
                       uint32_t src_qp, uint32_t dest_qp,
                       uint32_t psn, const unsigned char *data, int data_len);

// 处理重传请求（返回PSN >= ePSN的数据包）
int process_retransmit_request(struct connection_cache *cache, uint32_t ePSN,
                              struct cached_packet **retrans_pkts, size_t *count);

// 打印单个连接状态
void print_connection_status(struct connection_cache *cache);

// 打印所有连接状态
void print_all_connections_status();

// IP字符串转网络字节序
uint32_t ip_str_to_uint(const char *ip);

#endif // PKT_CACHE_H
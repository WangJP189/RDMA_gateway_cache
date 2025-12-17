#ifndef CACHE_MANAGER_H
#define CACHE_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <sys/time.h>

// ==================== 连接表相关定义 ==================== WTC

// 连接表键结构
struct connection_table_key {
    uint32_t Src_IP;
    uint32_t Dst_IP; 
    uint32_t Dst_QP;
};

// 每个连接的缓存指针数组
struct MetaArray 
{
    uintptr_t*  MetaDataArry;   // 缓存指针数组
    uint32_t    start_psn;
    uint32_t    end_psn;
    int         arraylength;
    
    volatile int ref_count;     // 引用计数，正向/反向连接指向同一缓存指针数组
};

// 连接表条目
struct connection_table_entry {
    struct connection_table_key     connection_table_key;   // 键
    uint32_t                        Src_QP;                 // 值（对应的QP号）
    struct MetaArray                *cache_array;           // 缓存指针数组
    struct connection_table_entry   *next;                  // 哈希冲突链表
};

// 哈希表定义
#define CONNECTION_TABLE_SIZE 2048
extern struct connection_table_entry* g_connection_table_forward[CONNECTION_TABLE_SIZE];
extern struct connection_table_entry* g_connection_table_reverse[CONNECTION_TABLE_SIZE];

// 创建连接表键
struct connection_table_key create_connection_table_key(
                        const char *src_ip, const char *dst_ip, uint32_t qp);

// 打印连接表键信息
void print_connection_table_key(const struct connection_table_key *key);

// 计算连接表的哈希值
unsigned int calculate_connection_table_hash(const struct connection_table_key *key);

// 连接表键比较函数
bool connection_table_key_equal(const struct connection_table_key *key1, 
                                const struct connection_table_key *key2);

// 添加连接表条目到指定哈希表
int add_to_connection_table(struct connection_table_entry** table, 
                            const struct connection_table_key *key, uint32_t value, struct MetaArray *shared_meta);

// 添加连接表条目（双向）
int add_connection_table_entry(const char *src_ip, const char *dst_ip, 
                        uint32_t src_qp, uint32_t dst_qp);

// 打印指定哈希表的所有条目
void print_connection_table(struct connection_table_entry** table, const char *table_name);

// 打印所有连接表信息
void print_all_connection_table();

// 释放所有连接表
void free_connection_table(struct connection_table_entry** table);

// 释放连接表
void free_all_connection_tables();

// 查找QP映射关系（根据源IP、目的IP和目的QP查找对应的源QP）
uint32_t lookup_qp_mapping(const char *src_ip, const char *dst_ip, uint32_t dest_qp);

struct MetaArray* create_meta_array(int length);

void release_meta_array(struct MetaArray *meta);

// ==================== 连接缓存相关定义 ====================

// 缓存报文结构
struct cached_packet {
    // unsigned char *app_data;          // 应用数据载荷
    // int data_len;                     // 数据长度
    // uint32_t dest_qp;                 // 目标QP号
    // uint32_t psn;                     // 包序列号
    // struct timeval timestamp;         // 缓存时间戳
    // struct cached_packet *next;       // 下一个节点（按PSN排序）
    // struct cached_packet *prev;       // 前一个节点（双向链表）
};

// 每个连接的缓存队列
//struct connection_cache {
    // struct cached_packet *head;       // 队列头（最小PSN）
    // struct cached_packet *tail;       // 队列尾（最大PSN）
    // size_t count;                     // 当前缓存数量
    // size_t total_bytes;               // 总字节数
    // uint32_t min_psn;                 // 最小PSN
    // uint32_t max_psn;                 // 最大PSN
    // struct timeval last_activity;     // 最后活动时间
    // pthread_mutex_t lock;             // 连接级锁
    
//};

// IPv4连接标识键（用于哈希表）
// struct connection_key {
//     uint32_t src_ip;                  // 源IP
//     uint32_t dst_ip;                  // 目的IP
//     uint16_t src_port;                // 源端口
//     uint16_t dst_port;                // 目的端口
//     uint32_t src_qp;                  // 源主机QP号     // TODO: 新增字段, 后面代码需要适配
//     uint32_t dest_qp;                 // 目标主机QP号   // TODO: 新增字段, 后面代码需要适配
//     uint8_t service_type;             // RDMA服务类型   // TODO: 待分析, 可能不需要此字段
//     uint16_t pkey;                    // 分区键
// };

// 哈希表节点
// struct hash_table_entry {
//     struct connection_key key;        // 连接标识
//     struct connection_cache *cache;   // 对应的缓存队列
//     struct hash_table_entry *next;    // 哈希冲突链表
// };

// 全局缓存管理器 NEED_WTC
// struct cache_manager {
//     struct hash_table_entry **hash_table;   // 哈希表
//     size_t hash_table_size;                 // 哈希表大小
//     size_t max_connections;           // 最大连接数
//     size_t max_packets_per_conn;      // 每连接最大报文数
//     size_t max_bytes_per_conn;        // 每连接最大字节数
//     int connection_timeout;           // 连接超时时间（秒）
//     pthread_mutex_t global_lock;      // 全局锁
//     size_t total_connections;         // 总连接数
// };

// 缓存管理器初始化
// 全局缓存管理器实例
// static struct cache_manager *g_cache_mgr = NULL;

// 初始化缓存管理器
// struct cache_manager* init_cache_manager(size_t hash_size, 
//                                         size_t max_conns,
//                                         size_t max_packets_per_conn,
//                                         size_t max_bytes_per_conn_mb,
//                                         int conn_timeout_seconds);

// 添加报文到对应的连接缓存
// int add_to_connection_cache(const char *src_ip, const char *dst_ip,
//                            uint16_t src_port, uint16_t dst_port,
//                            uint8_t service_type, uint16_t pkey,
//                            uint32_t dest_qp, uint32_t psn,
//                            const unsigned char *app_data, int data_len);
int add_to_connection_cache(const char *src_ip, const char *dst_ip,
                           uint32_t dest_qp, uint32_t psn,
                           const unsigned char *packet, int packet_len);

// 创建连接键
// struct connection_key create_connection_key(const char *src_ip, const char *dst_ip,
//                                             uint16_t src_port, uint16_t dst_port,
//                                             uint8_t service_type, uint16_t pkey, uint32_t dest_qp);

// 查找或创建连接缓存
// struct connection_cache* get_or_create_connection_cache(
//         struct cache_manager *mgr, const struct connection_key *key);
// get_or_create


// 计算连接键的哈希值
//uint32_t calculate_hash(const struct connection_key *key, size_t table_size);

// 比较两个连接键是否相等
//int connection_keys_equal(const struct connection_key *a,
//                          const struct connection_key *b);

// 连接缓存管理
// 创建新的连接缓存
struct connection_cache* create_connection_cache();

// 销毁连接缓存
void destroy_connection_cache(struct connection_cache *cache);

// 打印连接键信息
//void print_connection_key(const struct connection_key *key);

// 有序插入缓存报文（按PSN排序）
// 在连接缓存中按PSN顺序插入报文
int insert_packet_sorted(struct connection_cache *cache, 
                         struct cached_packet *new_packet);

#endif
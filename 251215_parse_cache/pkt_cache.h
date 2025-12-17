#ifndef CACHE_MANAGER_H
#define CACHE_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <sys/time.h>


// 配置参数
#define MEM_BLOCK_SIZE 5120        // 固定5KB内存块大小
#define RING_BUFFER_SIZE 1000      // 环形数组大小（存储内存首地址）
#define MAX_AGE_MILLISECONDS 60    // 数据包最大老化时间（毫秒），可按需调整
#define CONNECTION_TABLE_SIZE 2048 // 哈希表定义

// ==================== 连接表相关定义 ==================== WTC

// 连接表键结构
struct connection_table_key {
    uint32_t Src_IP;
    uint32_t Dst_IP; 
    uint32_t Dst_QP;
};


// 连接表条目
struct connection_table_entry {
    struct connection_table_key     connection_table_key;   // 键
    uint32_t                        Src_QP;                 // 值（对应的QP号）
    struct ConnectionCache          *cache_array;           // 缓存指针数组
    struct connection_table_entry   *next;                  // 哈希冲突链表
};



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
                            const struct connection_table_key *key, uint32_t value, struct ConnectionCache *shared_meta);

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

struct ConnectionCache* create_meta_array(int length);

void release_meta_array(struct ConnectionCache *meta);




// ==================== 连接缓存相关定义 ====================

// 每个连接的缓存指针数组
struct ConnectionCache 
{
    uintptr_t*  MemArray[RING_BUFFER_SIZE];   // 记录内存块地址针的环形数组
    uint32_t    start_psn;
    uint32_t    end_psn;
    uint32_t    current_psn;
    int         arraylength;
    
    volatile int ref_count;     // 引用计数，正向/反向连接指向同一缓存指针数组
};

// 内存块头部控制信息（嵌入在5KB内存块的开头）
// 整体内存块布局：[MemBlockHeader][RDMA数据包数据]，总大小≤5KB
typedef struct {
    int data_len;                  // 有效RDMA数据包长度
    uint64_t timestamp_ms;         // 毫秒级时间戳，记录数据包缓存时间（自解释命名）
} MemBlockHeader;


// 缓存RDMA数据包到内存，并将地址存入环形数组
int cache_rdma_packet(struct ConnectionCache* conn, uint32_t psn, const unsigned char* data, int data_len);

// 在连接的有效PSN范围（start_psn~end_psn）内查找丢包的数据包
int find_lost_packets(struct ConnectionCache* conn, uint32_t* lost_psns, int max_lost);

// 根据ePSN处理重传：删除psn<ePSN的包，收集psn≥ePSN的包地址用于重传
int process_retransmit_by_epsn(struct ConnectionCache* conn, uint32_t epsn, uint64_t** retrans_addrs, int* retrans_count);

// 释放指定PSN对应的内存块，并清空环形数组对应位置
int free_packet_by_psn(struct ConnectionCache* conn, uint32_t psn);

// IP字符串转网络字节序的uint32_t
uint32_t ip_str_to_uint(const char* ip);

// 老化处理函数：根据毫秒级时间戳清理过期的数据包（核心修改）
int age_out_expired_packets(struct ConnectionCache* conn, uint64_t current_timestamp_ms);

// 辅助函数：获取当前系统的毫秒级时间戳
uint64_t get_current_timestamp_ms(void);

// 添加连接缓存条目
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



#endif
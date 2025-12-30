#define _XOPEN_SOURCE 500

#ifndef PKT_CACHE_H
#define PKT_CACHE_H

#include <stdint.h>
#include <pthread.h>

//大小定义
#define     RING_BUFFER_SIZE    120000      // 环形数组大小（存储内存首地址）
#define     TABLE_SIZE          2048        // 表大小
#define MEM_BLOCK_SIZE 5120        // 固定5KB内存块大小

// PSN定义，解决end_psn<start_psn的问题
#define PSN_MASK          0xFFFFFF  // 24位PSN掩码（0~16777215）
#define PSN_MAX_VALUE     PSN_MASK        // PSN最大值（2^24-1）
#define BUFFER_MASK       RING_BUFFER_SIZE - 1  // 环形数组掩码（环形数组大小-1）

// 超时定义
#define CONN_IDLE_EXPIRE_THRESHOLD_MS  30000   // 连接老化阈值（30秒）
#define MAX_AGE_MILLISECONDS 60    // 数据包最大老化时间（毫秒），可按需调整
#define CONN_AGE_CHECK_INTERVAL_MS    5       // 连接级检查间隔（5ms）
#define AGE_THREAD_SLEEP_SEC          10       // 全局老化线程休眠间隔（10秒）

//全局哈希桶定义
#define CONN_BUCKET_COUNT 1024  // 哈希桶总数
extern struct connection_bucket *g_conn_buckets;

// ==================== DataStruct定义 ====================


/* ============ FLOWTABLE ============ */

// IPv4_流_键（用于流表）
struct flow_key {
    uint32_t    src_ip;         // 源IP
    uint32_t    dst_ip;         // 目的IP
    uint16_t    src_port;       // 源端口
    uint16_t    dst_port;       // 目的端口
    uint32_t    dst_qp;         // 目的QP
    uint16_t    pkey;           // 分区键
    uint16_t    resv;           // 字节对齐保留
};// UINT32/UINT16/UINT64等网络通讯时要用网络序，本机存储的时候要转换成本机序

enum gateway_role {

    src_gateway = 0,
    dst_gateway = 1
};

// 流条目
struct flow_entry {
    struct flow_key         flow_key;     // 流表-键
    uint32_t                src_qp;       // 流表-值
    enum gateway_role       role;         // 网关角色 
    struct flow_entry       *next;        // 哈希冲突链表
};


/* ============ CONNECTIONTABLE ============ */

// IPv4_连接_键（用于连接表）
struct connection_key {
    uint32_t    src_ip;         // 源IP
    uint32_t    dst_ip;         // 目的IP
    uint16_t    src_port;       // 源端口
    uint16_t    dst_port;       // 目的端口
    uint32_t    src_qp;         // 源QP
    uint32_t    dst_qp;         // 目的QP
    uint16_t    pkey;           // 分区键
    uint16_t    resv;           // 字节对齐保留
};


// 每个连接的缓存指针数组
struct connection_cache_array {
    uintptr_t*          ring_buf;           // 记录内存块地址的环形数组
    uint32_t            array_length;
    uint32_t            start_psn;
    uint32_t            end_psn;
    uint32_t            cur_psn;
    uint64_t            last_age_stamp;     // 上次老化时间记录
    uint64_t            last_active_stamp;  // 连接最后活动时间（收/发包）
    // pthread_rwlock_t    rwlock;             // 连接级读写锁
};// ring_buf需动态malloc，防止stack溢出

// 连接条目
struct connection_entry {
    struct connection_key           connection_key;
    struct connection_cache_array   *cache_array; 
    struct connection_entry         *next;              // 哈希冲突链表
    uint32_t    last_active_stamp; // 最后活动时间戳（秒级）
    int         valid;          // 连接有效性（1=有效，0=无效）
};

// 内存块头部控制信息（嵌入在5KB内存块的开头）
// 整体内存块布局：[mem_block_header][RDMA数据包数据]，总大小≤5KB
struct mem_block_header{
    int data_len;                  // 有效RDMA数据包长度
    uint64_t timestamp_ms;         // 毫秒级时间戳，记录数据包缓存时间
    uint32_t psn;                  // 新增：当前内存块对应的PSN
};

struct connection_bucket {
    pthread_rwlock_t                rwlock;     // 哈希桶级读写锁
    struct connection_entry         *head;
}__attribute__((aligned(64)));

// 定义重传处理结果的枚举类型
typedef enum {
    RETRANS_SUCCESS = 0,                  // 处理成功
    RETRANS_INVALID_PARAM = -1,           // 参数无效
    RETRANS_NO_VALID_PSN_RANGE = -2,      // 无有效PSN范围
    RETRANS_NO_CACHED_PACKETS = -3,       // 未缓存任何数据包
    RETRANS_NO_NEED = -4                  // 无需处理重传（start_psn >= epsn）
} retransmit_process_result;

// ==================== FlowTable接口声明 ====================

extern struct flow_entry*       g_flow_table_forward[TABLE_SIZE];
extern struct flow_entry*       g_flow_table_reverse[TABLE_SIZE];

// 创建流键
struct flow_key create_flow_key(const char *src_ip, const char *dst_ip, 
                                uint16_t src_port, uint16_t dst_port,
                                uint32_t dst_qp, uint16_t pkey);

// 添加条目到双向流表                             
int add_to_flow_table(const char *src_ip_str, const char *dst_ip_str, 
                      uint16_t src_port, uint16_t dst_port,
                      uint32_t src_qp, uint32_t dst_qp, 
                      uint16_t pkey);
                 
// 查找流表
struct flow_entry* lookup_flow(const char *pkt_src_ip, const char *pkt_dst_ip,
                                     uint16_t pkt_src_port, uint16_t pkt_dst_port,
                                     uint32_t pkt_dst_qp, uint16_t pkt_pkey);

// 销毁双向流表
void destroy_flow_tables();

// 根据连接键删除对应的双向流条目
void remove_flow_entry(struct connection_key key);

// 打印流条
void print_flow_entry(struct flow_entry *entry, const char* type);

// ==================== ConnectionTable接口声明 ====================

extern struct connection_bucket connection_table[TABLE_SIZE];

// [新增] 初始化连接表
int init_connection_table(void);

// [新增] 获取 Bucket 指针
struct connection_bucket* get_connection_bucket(struct connection_key key);

// 创建连接键
struct connection_key create_connection_key(const char *src_ip, const char *dst_ip, 
                                            uint16_t src_port, uint16_t dst_port, 
                                            uint32_t src_qp, uint32_t dst_qp, uint16_t pkey);
/**
 * @brief 查找或创建连接表条目
 * @return 连接缓存数组指针
 */
struct connection_cache_array* get_or_create_connection_cache_array(struct connection_bucket *bucket, struct connection_key key);

/**
 * @brief [纯查找]仅查找连接缓存
 * @return 找到返回 cache 指针，未找到返回 NULL
 */
struct connection_cache_array* get_connection_cache(struct connection_entry *entry, struct connection_key key);

/**
 * @brief [纯创建] 创建新连接并分配缓存
 * @note 如果连接已存在，会打印警告并返回现有缓存
 */
struct connection_cache_array* create_connection_cache(struct connection_bucket *bucket, struct connection_key key);

/**
 * @brief 销毁特定连接条目
 * @note 外部调用仅供持有锁时
 */
void remove_connection_entry(struct connection_key key);

// 销毁连接表
void destroy_connection_table();


// ==================== CACHEOPERATION接口声明 ====================

// 将数据包存入连接缓存结构
int add_to_connection_cache(struct connection_cache_array* conn_cache, uint32_t psn,
                            const unsigned char *packet_data, int packet_len);


// 缓存RDMA数据包到内存，并将地址存入环形数组
int cache_rdma_packet(struct connection_cache_array* conn, uint32_t psn, const unsigned char* data, int data_len);

// 在连接的有效PSN范围（start_psn~end_psn）内查找丢包的数据包
int find_lost_packets(struct connection_cache_array* conn, uint32_t* lost_psns, int max_lost);

// 根据ePSN处理重传：删除psn<ePSN的包，收集psn≥ePSN的包地址用于重传
retransmit_process_result process_retransmit_by_epsn(struct connection_cache_array* conn, uint32_t epsn, uint64_t** retrans_addrs, int* retrans_count);

// 收到ACK后清理已被确认的报文（PSN ≤ ack_msn），释放对应内存块
int clean_acked_packets(struct connection_cache_array* conn, uint32_t ack_msn);

// 释放指定PSN对应的内存块，并清空环形数组对应位置
retransmit_process_result free_packet_by_psn(struct connection_cache_array* conn, uint32_t psn);

// 老化处理函数：根据毫秒级时间戳清理过期的数据包
int age_out_expired_packets(struct connection_cache_array* conn, uint64_t current_timestamp_ms);

// 辅助函数：获取当前系统的毫秒级时间戳
uint64_t get_current_timestamp_ms(void);
#endif
#define _XOPEN_SOURCE 500

#ifndef PKT_CACHE_H
#define PKT_CACHE_H

#include <stdint.h>
#include <pthread.h>
#include <time.h>
#include <signal.h>
// #include <bits/pthreadtypes.h>

//大小定义
#define     RING_BUFFER_SIZE    102400      // 环形数组大小（存储内存首地址）
#define     TABLE_SIZE          2048        // 表大小
#define MEM_BLOCK_SIZE 5120        // 固定5KB内存块大小

// PSN定义，解决end_psn<start_psn的问题
#define PSN_MASK          0xFFFFFF  // 24位PSN掩码（0~16777215）
#define PSN_HALF_CYCLE 0x800000  // 24位PSN的半周期（判断回绕的阈值）
#define PSN_MAX_VALUE     PSN_MASK        // PSN最大值（2^24-1）
#define PSN_INVALID       0x1000000  // 无效PSN（大于最大值，表示未初始化）

// 超时定义
#define TIME_STAMP_UNIT_MS     1          // 时间戳单位：毫秒
#define CONN_IDLE_EXPIRE_THRESHOLD  30000   // 连接老化阈值（30000ms）
#define PACKET_AGE_THRESHOLD 10    // 数据包最大老化时间（毫秒）
#define PACKET_AGE_CHECK_INTERVAL    5       // 报文老化检查间隔
#define AGE_THREAD_SLEEP_INTERVAL          10       // 全局老化线程休眠间隔（10秒）
#define GLOBAL_AGE_BATCH_SIZE  100        // 全局老化分批次遍历大小
#define CONN_AGE_PER_BUCKET_DELAY 500 // 每个桶老化后延时（微秒）

//全局哈希桶定义
#define CONN_BUCKET_COUNT 1024  // 哈希桶总数
extern struct connection_bucket *g_conn_buckets;
extern volatile sig_atomic_t g_running; // 全局线程控制标记
extern uint32_t g_total_cleaned_conn; // 全局累计清理连接数
extern pthread_t g_aging_tid; // 全局老化线程ID


// ==================== DataStruct定义 ====================


/* ============ FLOWTABLE ============ */

// IPv4_流_键（用于流表）
struct flow_key {
    uint32_t    src_ip;         // 源IP
    uint32_t    dst_ip;         // 目的IP
    // uint16_t    src_port;       // 源端口
    // uint16_t    dst_port;       // 目的端口
    uint32_t    dst_qp;         // 目的QP
    uint16_t    pkey;           // 分区键
    uint16_t    resv1;           // 字节对齐保留
    uint16_t    resv2;           // 字节对齐保留
    uint16_t    resv3;           // 字节对齐保留
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
    // uint16_t    src_port;       // 源端口
    // uint16_t    dst_port;       // 目的端口
    uint32_t    src_qp;         // 源QP
    uint32_t    dst_qp;         // 目的QP
    uint16_t    pkey;           // 分区键
    uint16_t    resv1;           // 字节对齐保留
    uint16_t    resv2;           // 字节对齐保留
    uint16_t    resv3;           // 字节对齐保留
};


// 每个连接的缓存指针数组
struct connection_cache_array {
    uintptr_t*          ring_buf;           // 记录内存块地址的环形数组
    uint32_t            array_length;
    uint32_t            start_psn;
    uint32_t            end_psn;
    uint32_t            cur_psn;
    // uint64_t            last_age_stamp;     // 上次老化时间记录
    uint64_t            last_active_stamp;  // 连接最后活动时间（收/发包）
    // pthread_rwlock_t    rwlock;             // 连接级读写锁
};// ring_buf需动态malloc，防止stack溢出

// 连接条目
struct connection_entry {
    struct connection_key           connection_key;
    struct connection_cache_array   *cache_array; 
    struct connection_entry         *next;              // 哈希冲突链表
    // uint32_t    last_active_stamp; // 最后活动时间戳（秒级）
    int         valid;          // 连接有效性（1=有效，0=无效）
};

// 内存块头部控制信息（嵌入在5KB内存块的开头）
// 整体内存块布局：[mem_block_header][RDMA数据包数据]，总大小≤5KB
struct mem_block_header{
    int data_len;                  // 有效RDMA数据包长度
    uint64_t recv_stamp;         // 毫秒级时间戳，记录数据包缓存时间
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
    RETRANS_NO_NEED = -4,                 // 无需处理重传（start_psn >= epsn）
    RETRANS_DATA_ALLOC_FAIL = -5,         // connection_cache_array数据分配失败
} retransmit_process_result;

// ==================== FlowTable接口声明 ====================

extern struct flow_entry*       g_flow_table_forward[TABLE_SIZE];
extern struct flow_entry*       g_flow_table_reverse[TABLE_SIZE];

// 创建流键
struct flow_key create_flow_key(const char *src_ip, const char *dst_ip, 
                                uint32_t dst_qp, uint16_t pkey);

// 添加条目到双向流表                             
int add_to_flow_table(const char *src_ip_str, const char *dst_ip_str, 
                      uint32_t src_qp, uint32_t dst_qp, 
                      uint16_t pkey);
                 
// 查找流表
struct flow_entry* lookup_flow(const char *pkt_src_ip, const char *pkt_dst_ip,
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
                                            uint32_t src_qp, uint32_t dst_qp, uint16_t pkey);
/**
 * @brief 查找或创建连接表条目
 * @return 连接缓存数组指针
 */
struct connection_cache_array* get_connection_cache_array(struct connection_bucket *bucket, struct connection_key key);

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

// 释放连接缓存数组及其内存块
void free_cache_array(struct connection_cache_array *cache);


// ==================== CACHEOPERATION接口声明 ====================

// 将数据包存入连接缓存结构
int add_to_connection_cache(struct connection_cache_array* conn_cache, uint32_t psn,
                            const unsigned char *packet_data, int packet_len);


// 缓存RDMA数据包到内存，并将地址存入环形数组
int cache_rdma_packet(struct connection_cache_array* conn, uint32_t psn, const unsigned char* data, int data_len);

// 收到ACK后清理已被确认的报文（PSN ≤ ack_msn），释放对应内存块
int clean_acked_packets(struct connection_cache_array* conn, uint32_t ack_msn);

// 定时老化处理函数：根据毫秒级时间戳清理过期的数据包
void age_expired_packets(struct connection_cache_array* conn);




// 释放单个连接条目及其资源（全局资源老化功能调用）
static void free_connection_entry(struct connection_entry *entry);

//清理单个哈希桶中的空闲连接条目（全局资源老化功能调用）
int clean_idle_entry(uint32_t bucket_idx);

// 清理全局空闲连接条目（全局资源老化线程调用）
int clean_global_idle_entry();

// 标记空闲连接为无效（全局资源老化线程调用）
int connection_bucket_mark_idle_as_invalid(uint32_t bucket_idx);
// 清理单个哈希桶valid=0的连接条目（全局资源老化线程调用）
static int connection_bucket_clean_invalid(uint32_t bucket_idx);
// 清理全局valid=0的连接条目（全局资源老化线程调用）
int connection_global_clean_invalid();

// 全局资源老化线程入口函数
void *connection_aging_thread();

// 启动全局资源老化线程
int connection_aging_thread_start();

// 停止全局资源老化线程
int connection_aging_thread_stop(void);





//======辅助函数=====
// 获取当前系统的毫秒级时间戳
uint64_t get_current_timestamp_ms(void);

// PSN比较函数，处理24位PSN的回绕问题,主要判断start_psn是否需要更新，以及是否发生回绕
int psn_less_than(uint32_t a, uint32_t b);

// PSN比较函数，处理24位PSN的回绕问题,主要判断end_psn是否需要更新
int psn_greater_equal(uint32_t a, uint32_t b);

// 批量清理指定PSN范围内的数据包
int batch_clean_psn_range(struct connection_cache_array* conn, uint32_t start, uint32_t end);

//二分法处理PSN老化（回绕+查找+判断+批量清理全流程）
void binary_age_psn(struct connection_cache_array* conn, uint32_t start_psn, uint32_t end_psn, uint64_t current_timestamp_ms);

// 查找连接缓存中的最小有效PSN
uint32_t find_valid_min_psn(struct connection_cache_array *conn);

#endif
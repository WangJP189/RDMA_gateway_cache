#define _XOPEN_SOURCE 500

#ifndef PKT_CACHE_H
#define PKT_CACHE_H

#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <time.h>

#include "simulate.h"

// #include <bits/pthreadtypes.h>

// 大小定义
#define RING_BUFFER_SIZE 10240 // 环形数组大小（存储内存首地址）
#define TABLE_SIZE 2048        // 表大小
#define MEM_BLOCK_SIZE 5120    // 固定5KB内存块大小（空间实验对比用，网关已不用）

// ==================== 档位（size-class）动态内存块机制（论文核心机制） ====================
//
// 取代「简单 malloc」：把固定 5KB 改为「档位标记数组 + 分档环形数组」。
// 关键点：查找仍是 O(1)，因为
//   * 档位标记数组 tier_mark 是一维字节数组，索引 = psn % TIER_MARK_SIZE，
//     值是「这个 PSN 的包被存进了哪个档位」。它是确定性数组，不是哈希表——
//     索引由 PSN 模运算直接算出，无需遍历/搜索。
//   * 分档环形数组 tier_ring[tier] 是每个档位一根的环形缓冲，
//     存该档位内存块地址；同档内块大小一致（可池化、无外部碎片）。
//
// 一次查找 = 读标记数组 1 次 + 读分档环 1 次，共 2 次数组访问，O(1) 且无遍历。
//
// 档位边界由建联协商的 MTU 动态生成（仿真中用全局 g_mtu 模拟协商结果，默认 4096）。
// 覆盖 MTU 档位：{g_mtu/16, g_mtu/8, g_mtu/4, g_mtu/2, g_mtu}（幂次 2，便于分类）。

#define TIER_COUNT 5                         // 档位数
#define TIER_MARK_SIZE RING_BUFFER_SIZE      // 档位标记数组长度（字节）
#define TIER_NONE 0xFF                       // 标记数组的空档位值（该 PSN 无缓存）
#define TIER_ALIGN 16                        // 内存块对齐粒度（与 glibc malloc 16B 一致）

// 全局变量：仿真 MTU 与档位边界（由 main.c/simulate.c 初始化，模拟建联协商结果）
extern uint32_t g_mtu;                       // 协商得到的 MTU（字节），默认 4096
extern uint32_t g_tier_boundary[TIER_COUNT]; // 档位边界（字节），幂次 2

// 根据协商 MTU 重新生成档位边界（幂次 2），供 main.c 初始化调用
void init_tier_boundaries(uint32_t mtu);

// PSN定义，解决end_psn<start_psn的问题
#define PSN_MASK 0xFFFFFF       // 24位PSN掩码（0~16777215）
#define PSN_HALF_CYCLE 0x800000 // 24位PSN的半周期（判断回绕的阈值）
#define PSN_MAX_VALUE PSN_MASK  // PSN最大值（2^24-1）
#define PSN_INVALID 0x1000000   // 无效PSN（大于最大值，表示未初始化）

// 超时定义
#define TIME_STAMP_UNIT_MS 1             // 时间戳单位：毫秒
#define CONN_IDLE_EXPIRE_THRESHOLD 30000 // 连接老化阈值（毫秒）
#define PACKET_AGE_THRESHOLD 100         // 数据包最大老化时间（毫秒）
#define PACKET_AGE_CHECK_INTERVAL 5      // 报文老化检查间隔（毫秒）
#define AGE_THREAD_SLEEP_INTERVAL 10     // 全局老化线程休眠间隔（秒）
#define CONN_AGE_PER_BUCKET_DELAY 50     // 每个桶老化后延时（微秒）

// 全局哈希桶定义
#define CONN_BUCKET_COUNT 1024 // 哈希桶总数

// 全局变量声明
extern struct connection_bucket *g_conn_buckets;
extern volatile sig_atomic_t g_running; // 全局线程控制标记
extern uint32_t g_total_cleaned_conn;   // 全局累计清理连接数
extern pthread_t g_aging_tid;           // 全局老化线程ID

// ==================== DataStruct定义 ====================

/* ============ FLOWTABLE ============ */

// IPv4_流_键（用于流表）
struct flow_key {
    uint32_t src_ip; // 源IP
    uint32_t dst_ip; // 目的IP
    uint32_t dst_qp; // 目的QP
    uint16_t pkey;   // 分区键
    uint16_t resv1;  // 字节对齐保留
    uint16_t resv2;  // 字节对齐保留
    uint16_t resv3;  // 字节对齐保留
};

enum gateway_role {

    src_gateway = 0,
    dst_gateway = 1
};

// 流条目
struct flow_entry {
    struct flow_key flow_key; // 流表-键
    uint32_t src_qp;          // 流表-值
    enum gateway_role role;   // 网关角色
    struct flow_entry *next;  // 哈希冲突链表
};

/* ============ CONNECTIONTABLE ============ */

// IPv4_连接_键（用于连接表）
struct connection_key {
    uint32_t src_ip; // 源IP
    uint32_t dst_ip; // 目的IP
    uint32_t src_qp; // 源QP
    uint32_t dst_qp; // 目的QP
    uint16_t pkey;   // 分区键
    uint16_t resv1;  // 字节对齐保留
    uint16_t resv2;  // 字节对齐保留
    uint16_t resv3;  // 字节对齐保留
};

// 每档一个空闲块池（free list）：复用释放的内存块，避免每包 malloc/free
// 空闲块首 8 字节存 next 指针，构成单向链表。
struct mem_block_pool {
    void *free_head;       // 空闲块链表头（NULL 表示空）
    size_t block_size;     // 该档块大小 = align(header + tier_boundary)
    uint64_t alloc_count;  // 累计 malloc 次数（统计复用率用）
    uint64_t reuse_count;  // 累计复用次数
};

// 每个连接的缓存指针数组（分档环形数组 + 档位标记数组）
//
// 与「单根 ring_buf」的差异：多了一根档位标记数组 tier_mark，
// 以及把一根环形数组拆成 TIER_COUNT 根（每档一根）。
// 查找路径：tier = tier_mark[psn % TIER_MARK_SIZE] --> tier_ring[tier][psn % len]。
struct connection_cache_array {
    uint8_t *tier_mark;                // 档位标记数组：psn%TIER_MARK_SIZE -> 档位号
    uintptr_t *tier_ring[TIER_COUNT];  // 分档环形数组：每档一根，存内存块地址
    uint32_t tier_ring_len[TIER_COUNT];// 每档环长（= RING_BUFFER_SIZE，幂次 2）
    struct mem_block_pool pool[TIER_COUNT]; // 每档空闲块池
    uint32_t array_length;             // 兼容字段（= RING_BUFFER_SIZE）
    uint32_t start_psn;
    uint32_t end_psn;
    uint32_t cur_psn;
    uint64_t last_active_stamp; // 连接最后活动时间（收/发包）
};

// 连接条目
struct connection_entry {
    struct connection_key connection_key;
    struct connection_cache_array *cache_array;
    struct connection_entry *next; // 哈希冲突链表
    int valid;                     // 连接有效性（1=有效，0=无效）
};

// 内存块头部控制信息（嵌入在内存块的开头）
// 整体内存块布局：[mem_block_header][RDMA数据包数据]，总大小≤该档边界
struct mem_block_header {
    int data_len;        // 有效RDMA数据包长度
    uint64_t recv_stamp; // 毫秒级时间戳，记录数据包缓存时间
    uint32_t psn;        // 当前内存块对应的PSN
};

// ==================== 档位机制 O(1) 原语（内联，供 pkt_cache.c / pkt_recv.c 共用） ====================

// 档位判断：根据数据包长度用分支判断确定档位（O(1)，无遍历）
// 边界是幂次 2，也可用前导零（clz）实现，这里用分支保证可读性。
static inline uint8_t classify_tier(uint32_t data_len) {
    if (data_len <= g_tier_boundary[0]) return 0;
    if (data_len <= g_tier_boundary[1]) return 1;
    if (data_len <= g_tier_boundary[2]) return 2;
    if (data_len <= g_tier_boundary[3]) return 3;
    return 4; // 最大档
}

// 计算某档内存块大小（header + 档位边界，对齐到 TIER_ALIGN）
static inline size_t tier_block_size(uint8_t tier) {
    size_t need = sizeof(struct mem_block_header) + (size_t)g_tier_boundary[tier];
    return (need + (TIER_ALIGN - 1)) & ~((size_t)TIER_ALIGN - 1);
}

// 取块：psn -> 档位号 -> 槽位 -> 内存块地址（O(1)，2 次数组访问）
// 返回 NULL 表示该 PSN 无缓存块（标记数组为空档）。
static inline unsigned char *get_cached_block(struct connection_cache_array *conn,
                                              uint32_t psn) {
    uint8_t tier = conn->tier_mark[psn % TIER_MARK_SIZE];
    if (tier >= TIER_COUNT)
        return NULL;
    uint32_t slot = psn % conn->tier_ring_len[tier];
    return (unsigned char *)(uintptr_t)conn->tier_ring[tier][slot];
}

// 存块：写入档位环形数组，并在标记数组记录档位号（O(1)）
static inline void set_cached_block(struct connection_cache_array *conn,
                                    uint32_t psn, uint8_t tier,
                                    unsigned char *block) {
    uint32_t slot = psn % conn->tier_ring_len[tier];
    conn->tier_ring[tier][slot] = (uintptr_t)block;
    conn->tier_mark[psn % TIER_MARK_SIZE] = tier;
}

// 取并清块：读取该 PSN 的档位号与槽位，清空标记与槽位，返回块地址（O(1)）
// 注意：真正的内存释放（回收到档位池）由调用方决定，这里只做「摘除」。
static inline unsigned char *
take_cached_block(struct connection_cache_array *conn, uint32_t psn,
                  uint8_t *out_tier) {
    uint8_t tier = conn->tier_mark[psn % TIER_MARK_SIZE];
    if (tier >= TIER_COUNT)
        return NULL;
    uint32_t slot = psn % conn->tier_ring_len[tier];
    unsigned char *block = (unsigned char *)(uintptr_t)conn->tier_ring[tier][slot];
    conn->tier_ring[tier][slot] = 0;
    conn->tier_mark[psn % TIER_MARK_SIZE] = TIER_NONE;
    if (out_tier)
        *out_tier = tier;
    return block;
}

// 释放缓存块：O(1) 摘除该 PSN 的块并回收到对应档位空闲池（供 pkt_recv.c 调用）
void release_cached_block(struct connection_cache_array *conn, uint32_t psn);

struct connection_bucket {
    pthread_rwlock_t rwlock; // 哈希桶级读写锁
    struct connection_entry *head;
} __attribute__((aligned(64)));

// 定义重传处理结果的枚举类型
typedef enum {
    RETRANS_SUCCESS = 0,             // 处理成功
    RETRANS_INVALID_PARAM = -1,      // 参数无效
    RETRANS_NO_VALID_PSN_RANGE = -2, // 无有效PSN范围
    RETRANS_NO_CACHED_PACKETS = -3,  // 未缓存任何数据包
    RETRANS_NO_NEED = -4,            // 无需处理重传（start_psn >= epsn）
    RETRANS_DATA_ALLOC_FAIL = -5,    // connection_cache_array数据分配失败
    RETRANS_NO_PACKET = -6,          // 指定ring_buffer位置无数据包
    RETRANS_PACkET_PSN_MISMATCH = -7 // 指定ring_buffer位置数据包PSN不匹配
} retransmit_process_result;

// ==================== FlowTable接口声明 ====================

extern struct flow_entry *g_flow_table_forward[TABLE_SIZE];
extern struct flow_entry *g_flow_table_reverse[TABLE_SIZE];

// 创建流键
struct flow_key create_flow_key(const char *src_ip, const char *dst_ip,
                                uint32_t dst_qp, uint16_t pkey);

// 添加条目到双向流表
int add_to_flow_table(const char *src_ip_str, const char *dst_ip_str,
                      uint32_t src_qp, uint32_t dst_qp, uint16_t pkey);

// 查找流表
struct flow_entry *lookup_flow(const char *pkt_src_ip, const char *pkt_dst_ip,
                               uint32_t pkt_dst_qp, uint16_t pkt_pkey);

// 销毁双向流表
void destroy_flow_tables(void);

// 根据连接键删除对应的双向流条目
void remove_flow_entry(struct connection_key key);

// 打印流条
void print_flow_entry(struct flow_entry *entry, const char *type);

// ==================== ConnectionTable接口声明 ====================

extern struct connection_bucket connection_table[TABLE_SIZE];

// [新增] 初始化连接表
int init_connection_table(void);

// [新增] 获取 Bucket 指针
struct connection_bucket *get_connection_bucket(struct connection_key key);

// 创建连接键
struct connection_key create_connection_key(const char *src_ip,
                                            const char *dst_ip, uint32_t src_qp,
                                            uint32_t dst_qp, uint16_t pkey);

struct connection_key create_connection_key_u32(uint32_t src_ip,
                                                uint32_t dst_ip,
                                                uint32_t src_qp,
                                                uint32_t dst_qp, uint16_t pkey);
/**
 * @brief
 * @return 连接缓存数组指针
 */
struct connection_cache_array *
get_connection_cache_array(struct connection_bucket *bucket,
                           struct connection_key key);

/**
 * @brief [纯查找]仅查找连接缓存
 * @return 找到返回 cache 指针，未找到返回 NULL
 */
struct connection_cache_array *
get_connection_cache(struct connection_entry *entry, struct connection_key key);

/**
 * @brief [纯创建] 创建新连接并分配缓存
 * @note 如果连接已存在，会打印警告并返回现有缓存
 */
struct connection_cache_array *
create_connection_cache(struct connection_bucket *bucket,
                        struct connection_key key);

/**
 * @brief 销毁特定连接条目
 * @note 外部调用仅供持有锁时
 */
void remove_connection_entry(struct connection_key key);

// 销毁连接表
void destroy_connection_table(void);

// 释放连接缓存数组及其内存块
void free_cache_array(struct connection_cache_array *cache);

// ==================== CACHEOPERATION接口声明 ====================

// 将数据包存入连接缓存结构
int add_to_connection_cache(struct connection_cache_array *conn_cache,
                            uint32_t psn, const unsigned char *packet_data,
                            int packet_len);

// 缓存RDMA数据包到内存，并将地址存入环形数组
int cache_rdma_packet(struct connection_cache_array *conn, uint32_t psn,
                      const unsigned char *data, int data_len);

// 判断psn是否在范围内，考虑回绕情况
int psn_in_ring_range(uint32_t psn, uint32_t start, uint32_t end);

// 收到ACK后清理已被确认的报文（PSN ≤ ack_msn），释放对应内存块
int clean_acked_packets(struct connection_cache_array *conn, uint32_t ack_msn);

// 定时老化处理函数：根据毫秒级时间戳清理过期的数据包
void age_expired_packets(struct connection_cache_array *conn);

// 释放单个连接条目及其资源（全局资源老化功能调用）
void free_connection_entry(struct connection_entry *entry);

// 清理单个哈希桶中的空闲连接条目（全局资源老化功能调用）
int clean_idle_entry(uint32_t bucket_idx);

// 清理全局空闲连接条目（全局资源老化线程调用）
int clean_global_idle_entry(void);

// 打印当前所有连接的状态信息
void print_all_connections_status();

// 全局资源老化线程
void *age_thread_proc(void *arg);
void start_age_thread(void);
void stop_age_thread(void);
void cleanup_age_resources(void);

//======辅助函数=====
// 获取当前系统的毫秒级时间戳
uint64_t get_current_timestamp_ms(void);

// PSN比较函数，处理24位PSN的回绕问题,主要判断start_psn是否需要更新，以及是否发生回绕
int psn_less_than(uint32_t a, uint32_t b);

// PSN比较函数，处理24位PSN的回绕问题,主要判断end_psn是否需要更新
int psn_greater_than(uint32_t a, uint32_t b);

// 批量清理指定PSN范围内的数据包
int batch_clean_psn_range(struct connection_cache_array *conn, uint32_t start,
                          uint32_t end);

// 二分法处理PSN老化（回绕+查找+判断+批量清理全流程）
void binary_age_psn(struct connection_cache_array *conn, uint32_t start_psn,
                    uint32_t end_psn, uint64_t current_timestamp_ms);

// 查找连接缓存中的最小有效PSN
uint32_t find_valid_min_psn(struct connection_cache_array *conn);

// 判断连接是否空闲超时
int is_conn_idle_expired(struct connection_cache_array *conn);

#endif
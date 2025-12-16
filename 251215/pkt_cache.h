#ifndef PKT_CACHE_H
#define PKT_CACHE_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <arpa/inet.h>
#include <time.h>

// 配置参数
#define MEM_BLOCK_SIZE 5120        // 固定5KB内存块大小
#define RING_BUFFER_SIZE 1000      // 环形数组大小（存储内存首地址）
#define MAX_CONNECTIONS 10         // 最大连接数（连接表大小）
#define MAX_AGE_MILLISECONDS 60    // 数据包最大老化时间（毫秒），可按需调整

// 内存块头部控制信息（嵌入在5KB内存块的开头）
// 整体内存块布局：[MemBlockHeader][RDMA数据包数据]，总大小≤5KB
typedef struct {
    int data_len;                  // 有效RDMA数据包长度
    uint64_t timestamp_ms;         // 毫秒级时间戳，记录数据包缓存时间（自解释命名）
} MemBlockHeader;

// 连接标识键（三元组：src_ip + dst_ip + dst_qp 唯一标识一个连接）
typedef struct {
    uint32_t src_ip;               // 源IP（网络字节序）
    uint32_t dst_ip;               // 目的IP（网络字节序）
    uint32_t dst_qp;               // 目的QP号（唯一标识）
} ConnectionKey;

// 单个连接的缓存结构（核心是环形数组，存储内存块首地址）
typedef struct {
    ConnectionKey key;             // 连接三元组
    uint64_t ring_buffer[RING_BUFFER_SIZE]; // 环形数组：存储5KB内存块的首地址（64位地址）
    uint32_t start_psn;            // 环形数组中有效PSN的最小值
    uint32_t end_psn;              // 环形数组中有效PSN的最大值
    uint32_t current_psn;          // 当前最新的PSN
} ConnectionCache;

// 全局连接表：存储所有连接的缓存结构地址（用于查表找到对应连接的环形数组）
extern ConnectionCache* g_connection_table[MAX_CONNECTIONS];
// 全局连接计数
extern int g_connection_count;

// 函数声明（命名均自解释）
// 1. 根据连接三元组查找对应的连接缓存
ConnectionCache* find_connection_by_key(const ConnectionKey* key);
// 2. 添加新连接到连接表
int add_connection_to_table(const ConnectionKey* key);
// 3. 缓存RDMA数据包到内存，并将地址存入环形数组
int cache_rdma_packet(ConnectionCache* conn, uint32_t psn, const unsigned char* data, int data_len);
// 4. 在连接的有效PSN范围（start_psn~end_psn）内查找丢包的数据包
int find_lost_packets(ConnectionCache* conn, uint32_t* lost_psns, int max_lost);
// 5. 根据ePSN处理重传：删除psn<ePSN的包，收集psn≥ePSN的包地址用于重传
int process_retransmit_by_epsn(ConnectionCache* conn, uint32_t epsn, uint64_t** retrans_addrs, int* retrans_count);
// 6. 释放指定PSN对应的内存块，并清空环形数组对应位置
int free_packet_by_psn(ConnectionCache* conn, uint32_t psn);
// 7. IP字符串转网络字节序的uint32_t
uint32_t ip_str_to_uint(const char* ip);
// 8. 老化处理函数：根据毫秒级时间戳清理过期的数据包（核心修改）
int age_out_expired_packets(ConnectionCache* conn, uint64_t current_timestamp_ms);
// 辅助函数：获取当前系统的毫秒级时间戳（自解释）
uint64_t get_current_timestamp_ms(void);

#endif // PKT_CACHE_H
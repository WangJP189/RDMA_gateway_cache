#ifndef PKT_CACHE_H
#define PKT_CACHE_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <arpa/inet.h>

// 配置参数
#define MEM_BLOCK_SIZE 5120        // 固定5KB内存块大小
#define RING_BUFFER_SIZE 1000      // 环形数组大小（存储内存首地址）
#define MAX_CONNECTIONS 10         // 最大连接数（连接表大小）

// 内存块头部控制信息（嵌入在5KB内存块的开头）
// 整体内存块布局：[memblock_header][RDMA数据包数据]，总大小≤5KB
typedef struct {
    int data_len;                  // 有效RDMA数据包长度
} memblock_header;

// 连接标识键（三元组：src_ip + dst_ip + dst_qp 唯一标识一个连接）
typedef struct {
    uint32_t src_ip;               // 源IP（网络字节序）
    uint32_t dst_ip;               // 目的IP（网络字节序）
    uint32_t dst_qp;               // 目的QP号（唯一标识）
} connection_key;

// 单个连接的缓存结构（核心是环形数组，存储内存块首地址）
typedef struct {
    connection_key key;             // 连接三元组
    uint64_t ring_buffer[RING_BUFFER_SIZE]; // 环形数组：存储5KB内存块的首地址（64位地址）
    uint32_t start_psn;            // 环形数组中有效PSN的最小值
    uint32_t end_psn;              // 环形数组中有效PSN的最大值
    uint32_t current_psn;          // 当前最新的PSN
} connection_cache;

// 全局连接表：存储所有连接的缓存结构地址（用于查表找到对应连接的环形数组）
extern connection_cache* g_connection_table[MAX_CONNECTIONS];
// 全局连接计数
extern int g_connection_count;

// 函数声明
// 1. 根据连接三元组查找对应的连接缓存（返回环形数组地址）
uint64_t* find_connection_ring_buffer(const connection_key* key);
// 2. 添加新连接到连接表
int add_connection_to_table(const connection_key* key);
// 3. 缓存RDMA数据包到内存，并将地址存入环形数组
int cache_rdma_packet(const connection_key* key, uint32_t psn, const unsigned char* data, int data_len);
// 4. 根据ePSN查找环形数组中丢失的数据包（返回丢包的PSN列表）
int find_lost_packets(uint64_t* ring_buffer, uint32_t epsn_start, uint32_t epsn_end, uint32_t* lost_psns, int max_lost);
// 5. 释放指定PSN对应的内存块，并清空环形数组对应位置
int free_packet_by_psn(uint64_t* ring_buffer, uint32_t psn);
// 6. IP字符串转网络字节序的uint32_t
uint32_t ip_str_to_uint(const char* ip);

#endif // PKT_CACHE_H
/*
该版本的人的吗网关缓存模块特点：
1. 支持多连接缓存，每个连接维护独立的环形数组存储RDMA数据包的内存首地址
2. 数据包缓存到动态分配的5KB内存块中，头部包含控制信息，这个内存位置是随机的
3. 环形数组的主要作用是存储这些5KB内存块的首地址，便于快速查找和管理，期间使用地址映射机制，实现存储首地址的有序性
4. 支持根据ePSN范围查找丢包，可以直接到环形数组中定位丢失的数据包地址
*/

//注：缓存模块的for循环月月应该只有ePSN查找有，其余都不需要

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "pkt_cache.h"

// 全局连接表初始化
connection_cache* g_connection_table[MAX_CONNECTIONS] = {NULL};
int g_connection_count = 0;

//以下几个函数需要滕超学长实现connection_key_equal、find_connection_ring_buffer、add_connection_to_table

// 辅助函数：比较两个连接键是否相等（三元组匹配）
static int connection_key_equal(const connection_key* a, const connection_key* b) {
    if (!a || !b) return 0;
    return (a->src_ip == b->src_ip &&
            a->dst_ip == b->dst_ip &&
            a->dst_qp == b->dst_qp);
}

// 1. 根据连接三元组查找对应的连接缓存（返回环形数组地址）
uint64_t* find_connection_ring_buffer(const connection_key* key) {
    if (!key) {
        printf("[ERROR] 查找连接失败：连接键为空\n");
        return NULL;
    }

    // 遍历连接表，查找匹配的连接
    for (int i = 0; i < g_connection_count; i++) {
        if (connection_key_equal(&g_connection_table[i]->key, key)) {
            printf("[INFO] 找到连接：src_ip=0x%x, dst_ip=0x%x, dst_qp=%u | 环形数组地址=%p\n",
                   key->src_ip, key->dst_ip, key->dst_qp, g_connection_table[i]->ring_buffer);
            return g_connection_table[i]->ring_buffer;
        }
    }

    printf("[WARN] 未找到连接：src_ip=0x%x, dst_ip=0x%x, dst_qp=%u\n",
           key->src_ip, key->dst_ip, key->dst_qp);
    return NULL;
}

// 2. 添加新连接到连接表
int add_connection_to_table(const connection_key* key) {
    if (!key || g_connection_count >= MAX_CONNECTIONS) {
        printf("[ERROR] 添加连接失败：连接键为空或连接表已满（当前=%d/最大=%d）\n",
               g_connection_count, MAX_CONNECTIONS);
        return -1;
    }

    // 检查连接是否已存在
    for (int i = 0; i < g_connection_count; i++) {
        if (connection_key_equal(&g_connection_table[i]->key, key)) {
            printf("[WARN] 连接已存在：src_ip=0x%x, dst_ip=0x%x, dst_qp=%u\n",
                   key->src_ip, key->dst_ip, key->dst_qp);
            return 0;
        }
    }

    // 分配连接缓存内存
    connection_cache* new_conn = (connection_cache*)malloc(sizeof(connection_cache));
    if (!new_conn) {
        printf("[ERROR] 添加连接失败：内存分配失败\n");
        return -1;
    }

    // 初始化连接缓存
    new_conn->key = *key;
    // 初始化环形数组：所有位置设为0（表示空地址）
    memset(new_conn->ring_buffer, 0, sizeof(uint64_t) * RING_BUFFER_SIZE);
    new_conn->start_psn = 0;
    new_conn->end_psn = 0;
    new_conn->current_psn = 0;

    // 添加到连接表
    g_connection_table[g_connection_count++] = new_conn;

    printf("[INFO] 新增连接成功：src_ip=0x%x, dst_ip=0x%x, dst_qp=%u | 连接表位置=%d | 环形数组地址=%p\n",
           key->src_ip, key->dst_ip, key->dst_qp, g_connection_count - 1, new_conn->ring_buffer);
    return 0;
}



//以下函数需要我的缓存模块具体实现的

// 3. 缓存RDMA数据包到内存，并将地址存入环形数组
// 这个函数有问题，用for循环查找哪个个连接对应ring_buffer，部分应该是直接在传参部分传入的对应连接，不需要查找

int cache_rdma_packet(const connection_key* key, uint32_t psn, const unsigned char* data, int data_len) {
    if (!key || !data || data_len <= 0) {
        printf("[ERROR] 缓存数据包失败：参数无效（psn=%u, data_len=%d）\n", psn, data_len);
        return -1;
    }

    // 检查数据长度是否超过内存块可用空间（5KB - 头部控制信息大小）
    int max_data_len = MEM_BLOCK_SIZE - sizeof(memblock_header);
    if (data_len > max_data_len) {
        printf("[ERROR] 缓存数据包失败：数据长度超过上限（请求=%d, 上限=%d）\n", data_len, max_data_len);
        return -1;
    }

    // 步骤1：查找/添加连接，获取环形数组地址
    uint64_t* ring_buffer = find_connection_ring_buffer(key);
    if (!ring_buffer) {
        // 连接不存在，添加新连接
        if (add_connection_to_table(key) != 0) {
            return -1;
        }
        // 重新查找环形数组
        ring_buffer = find_connection_ring_buffer(key);
        if (!ring_buffer) {
            return -1;
        }
    }

    // 步骤2：malloc固定5KB内存块（随机分配，动态内存）
    unsigned char* mem_block = (unsigned char*)malloc(MEM_BLOCK_SIZE);
    if (!mem_block) {
        printf("[ERROR] 缓存数据包失败：内存块分配失败（5KB）\n");
        return -1;
    }

    // 步骤3：写入内存块头部控制信息 + RDMA数据包
    memblock_header* header = (memblock_header*)mem_block;
    header->data_len = data_len; // 头部存储有效数据长度

    // 拷贝RDMA数据包到头部之后的位置
    memcpy(mem_block + sizeof(memblock_header), data, data_len);

    // 步骤4：计算PSN对应的环形数组索引（psn=3 → 索引3)
    // 索引公式：psn % RING_BUFFER_SIZE，根据需求调整
    int ring_index = psn % RING_BUFFER_SIZE;
    if (ring_index < 0 || ring_index >= RING_BUFFER_SIZE) {
        printf("[ERROR] 缓存数据包失败：PSN对应的索引超出范围（psn=%u, 索引=%d, 数组大小=%d）\n",
               psn, ring_index, RING_BUFFER_SIZE);
        free(mem_block);
        return -1;
    }

    // 步骤5：将内存块首地址存入环形数组对应位置（转换为uint64_t存储）
    ring_buffer[ring_index] = (uint64_t)(uintptr_t)mem_block;
    printf("[CACHE] 数据包PSN=%u → 环形数组索引=%d | 内存块首地址=0x%lx | 数据长度=%d\n",
           psn, ring_index, (uintptr_t)mem_block, data_len);

    // 步骤6：更新连接的PSN参数（找到对应连接缓存，更新start/end/current_psn）
    connection_cache* conn = NULL;
    for (int i = 0; i < g_connection_count; i++) {
        if (g_connection_table[i]->ring_buffer == ring_buffer) {
            conn = g_connection_table[i];
            break;
        }
    }
    if (conn) {
        // 更新start_psn（最小PSN）
        if (conn->start_psn == 0 || psn < conn->start_psn) {
            conn->start_psn = psn;
        }
        // 更新end_psn（最大PSN）
        if (psn > conn->end_psn) {
            conn->end_psn = psn;
        }
        // 更新current_psn（当前最新PSN）
        conn->current_psn = psn;

        printf("[UPDATE] 连接PSN参数：start_psn=%u, end_psn=%u, current_psn=%u\n",
               conn->start_psn, conn->end_psn, conn->current_psn);
    }

    return 0;
}

// 4. 根据ePSN范围查找环形数组中丢失的数据包（返回丢包的PSN列表）
int find_lost_packets(uint64_t* ring_buffer, uint32_t epsn_start, uint32_t epsn_end, uint32_t* lost_psns, int max_lost) {
    if (!ring_buffer || epsn_start > epsn_end || !lost_psns || max_lost <= 0) {
        printf("[ERROR] 查找丢包失败：参数无效（ePSN范围=%u~%u）\n", epsn_start, epsn_end);
        return -1;
    }

    int lost_count = 0;
    printf("\n[FIND] 开始查找ePSN范围[%u~%u]的丢包情况：\n", epsn_start, epsn_end);

    // 遍历ePSN范围，检查环形数组对应位置是否为空（地址为0表示丢包）
    for (uint32_t psn = epsn_start; psn <= epsn_end && lost_count < max_lost; psn++) {
        int ring_index = psn - 1;
        if (ring_index < 0 || ring_index >= RING_BUFFER_SIZE) {
            printf("[WARN] PSN=%u 对应的索引超出范围（索引=%d），跳过\n", psn, ring_index);
            continue;
        }

        if (ring_buffer[ring_index] == 0) {
            // 地址为空，说明丢包
            lost_psns[lost_count++] = psn;
            printf("[LOST] PSN=%u → 环形数组索引=%d | 地址为空（丢包）\n", psn, ring_index);
        } else {
            // 地址不为空，验证内存块数据
            unsigned char* mem_block = (unsigned char*)(uintptr_t)ring_buffer[ring_index];
            memblock_header* header = (memblock_header*)mem_block;
            printf("[FOUND] PSN=%u → 环形数组索引=%d | 地址=0x%lx | 数据长度=%d（正常）\n",
                   psn, ring_index, (uintptr_t)mem_block, header->data_len);
        }
    }

    printf("[FIND] 查找完成：ePSN范围[%u~%u] | 总检查PSN数=%u | 丢包数=%d\n",
           epsn_start, epsn_end, epsn_end - epsn_start + 1, lost_count);
    return lost_count;
}

// 5. 释放指定PSN对应的内存块，并清空环形数组对应位置
int free_packet_by_psn(uint64_t* ring_buffer, uint32_t psn) {
    if (!ring_buffer) {
        printf("[ERROR] 释放数据包失败：环形数组地址为空\n");
        return -1;
    }

    int ring_index = psn - 1;
    if (ring_index < 0 || ring_index >= RING_BUFFER_SIZE) {
        printf("[ERROR] 释放数据包失败：PSN=%u 对应的索引超出范围（索引=%d）\n", psn, ring_index);
        return -1;
    }

    // 释放内存块
    if (ring_buffer[ring_index] != 0) {
        unsigned char* mem_block = (unsigned char*)(uintptr_t)ring_buffer[ring_index];
        free(mem_block);
        ring_buffer[ring_index] = 0; // 清空环形数组位置
        printf("[FREE] PSN=%u → 环形数组索引=%d | 内存块已释放\n", psn, ring_index);
    } else {
        printf("[WARN] PSN=%u → 环形数组索引=%d | 地址为空，无需释放\n", psn, ring_index);
    }

    return 0;
}

// 6. IP字符串转网络字节序的uint32_t
uint32_t ip_str_to_uint(const char* ip) {
    struct in_addr addr;
    if (inet_pton(AF_INET, ip, &addr) != 1) {
        printf("[ERROR] 无效的IP地址：%s\n", ip);
        return 0;
    }
    return addr.s_addr; // 网络字节序
}
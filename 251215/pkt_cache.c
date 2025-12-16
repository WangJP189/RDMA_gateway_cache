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
ConnectionCache* g_connection_table[MAX_CONNECTIONS] = {NULL};
int g_connection_count = 0;

// 辅助函数：比较两个连接键是否相等（三元组匹配）
static int connection_key_equal(const ConnectionKey* a, const ConnectionKey* b) {
    if (!a || !b) return 0;
    return (a->src_ip == b->src_ip &&
            a->dst_ip == b->dst_ip &&
            a->dst_qp == b->dst_qp);
}

// 1. 根据连接三元组查找对应的连接缓存（返回ConnectionCache*，便于获取start/end_psn）
ConnectionCache* find_connection_by_key(const ConnectionKey* key) {
    if (!key) {
        printf("[ERROR] 查找连接失败：连接键为空\n");
        return NULL;
    }

    // 遍历连接表，查找匹配的连接
    for (int i = 0; i < g_connection_count; i++) {
        if (connection_key_equal(&g_connection_table[i]->key, key)) {
            printf("[INFO] 找到连接：src_ip=0x%x, dst_ip=0x%x, dst_qp=%u | 连接地址=%p\n",
                   key->src_ip, key->dst_ip, key->dst_qp, g_connection_table[i]);
            return g_connection_table[i];
        }
    }

    printf("[WARN] 未找到连接：src_ip=0x%x, dst_ip=0x%x, dst_qp=%u\n",
           key->src_ip, key->dst_ip, key->dst_qp);
    return NULL;
}

// 2. 添加新连接到连接表
int add_connection_to_table(const ConnectionKey* key) {
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
    ConnectionCache* new_conn = (ConnectionCache*)malloc(sizeof(ConnectionCache));
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

    printf("[INFO] 新增连接成功：src_ip=0x%x, dst_ip=0x%x, dst_qp=%u | 连接表位置=%d | 连接地址=%p\n",
           key->src_ip, key->dst_ip, key->dst_qp, g_connection_count - 1, new_conn);
    return 0;
}

// 3. 缓存RDMA数据包到内存，并将地址存入环形数组（直接传入连接缓存，移除内部查表）
int cache_rdma_packet(ConnectionCache* conn, uint32_t psn, const unsigned char* data, int data_len) {
    if (!conn || !data || data_len <= 0) {
        printf("[ERROR] 缓存数据包失败：参数无效（conn=%p, psn=%u, data_len=%d）\n",
               conn, psn, data_len);
        return -1;
    }

    // 检查数据长度是否超过内存块可用空间（5KB - 头部控制信息大小）
    int max_data_len = MEM_BLOCK_SIZE - sizeof(MemBlockHeader);
    if (data_len > max_data_len) {
        printf("[ERROR] 缓存数据包失败：数据长度超过上限（请求=%d, 上限=%d）\n", data_len, max_data_len);
        return -1;
    }

    // 步骤1：malloc固定5KB内存块（随机分配，动态内存）
    unsigned char* mem_block = (unsigned char*)malloc(MEM_BLOCK_SIZE);
    if (!mem_block) {
        printf("[ERROR] 缓存数据包失败：内存块分配失败（5KB）\n");
        return -1;
    }

    // 步骤2：写入内存块头部控制信息 + RDMA数据包
    MemBlockHeader* header = (MemBlockHeader*)mem_block;
    header->data_len = data_len; // 头部存储有效数据长度
    // 拷贝RDMA数据包到头部之后的位置
    memcpy(mem_block + sizeof(MemBlockHeader), data, data_len);

    // 步骤3：计算PSN对应的环形数组索引（psn=3 → 索引2，psn从1开始；psn=0 → 索引0）
    int ring_index = psn % RING_BUFFER_SIZE;
    if (ring_index < 0 || ring_index >= RING_BUFFER_SIZE) {
        printf("[ERROR] 缓存数据包失败：PSN对应的索引超出范围（psn=%u, 索引=%d, 数组大小=%d）\n",
               psn, ring_index, RING_BUFFER_SIZE);
        free(mem_block);
        return -1;
    }

        // 步骤4：处理环形数组冲突（覆盖旧数据包）
    if (conn->ring_buffer[ring_index] != 0) {
        // 存在旧数据包，先释放其内存块
        unsigned char* old_mem_block = (unsigned char*)(uintptr_t)conn->ring_buffer[ring_index];
        free(old_mem_block);
        printf("[OVERWRITE] 环形数组索引=%d 存在旧数据包（PSN可能为%u），已释放旧内存块（地址=0x%lx）\n",
               ring_index, psn - (psn % RING_BUFFER_SIZE) + ring_index, (uintptr_t)old_mem_block);
    }

    // 步骤5：将内存块首地址存入环形数组对应位置（转换为uint64_t存储）
    conn->ring_buffer[ring_index] = (uint64_t)(uintptr_t)mem_block;
    printf("[CACHE] 数据包PSN=%u → 环形数组索引=%d | 内存块首地址=0x%lx | 数据长度=%d\n",
           psn, ring_index, (uintptr_t)mem_block, data_len);

    // 步骤6：更新连接的PSN参数（直接操作传入的conn，无需循环查找）
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

    return 0;
}

// 4. 在连接的有效PSN范围（start_psn~end_psn）内查找丢包的数据包
int find_lost_packets(ConnectionCache* conn, uint32_t* lost_psns, int max_lost) {
    if (!conn || !lost_psns || max_lost <= 0) {
        printf("[ERROR] 查找丢包失败：参数无效（conn=%p, max_lost=%d）\n", conn, max_lost);
        return -1;
    }

    // 有效PSN范围：conn->start_psn ~ conn->end_psn
    uint32_t psn_start = conn->start_psn;
    uint32_t psn_end = conn->end_psn;

    if (psn_start == 0 || psn_start > psn_end) {
        printf("[WARN] 连接无有效PSN范围（start_psn=%u, end_psn=%u），无丢包可查\n",
               psn_start, psn_end);
        return 0;
    }

    int lost_count = 0;
    printf("\n[FIND] 开始查找连接有效PSN范围[%u~%u]的丢包情况：\n", psn_start, psn_end);

    // 遍历有效PSN范围，检查环形数组对应位置是否为空（地址为0表示丢包）
    for (uint32_t psn = psn_start; psn <= psn_end && lost_count < max_lost; psn++) {
        int ring_index = psn % RING_BUFFER_SIZE;
        if (ring_index < 0 || ring_index >= RING_BUFFER_SIZE) {
            printf("[WARN] PSN=%u 对应的索引超出范围（索引=%d），跳过\n", psn, ring_index);
            continue;
        }

        if (conn->ring_buffer[ring_index] == 0) {
            // 地址为空，说明丢包
            lost_psns[lost_count++] = psn;
            printf("[LOST] PSN=%u → 环形数组索引=%d | 地址为空（丢包）\n", psn, ring_index);
        } else {
            // 地址不为空，验证内存块数据
            unsigned char* mem_block = (unsigned char*)(uintptr_t)conn->ring_buffer[ring_index];
            MemBlockHeader* header = (MemBlockHeader*)mem_block;
            printf("[FOUND] PSN=%u → 环形数组索引=%d | 地址=0x%lx | 数据长度=%d（正常）\n",
                   psn, ring_index, (uintptr_t)mem_block, header->data_len);
        }
    }

    printf("[FIND] 查找完成：有效PSN范围[%u~%u] | 总检查PSN数=%u | 丢包数=%d\n",
           psn_start, psn_end, psn_end - psn_start + 1, lost_count);
    return lost_count;
}

// 5. 根据ePSN处理重传：删除psn<ePSN的包，收集psn≥ePSN的包地址用于重传
int process_retransmit_by_epsn(ConnectionCache* conn, uint32_t epsn, uint64_t** retrans_addrs, int* retrans_count) {
    if (!conn || !retrans_addrs || !retrans_count || epsn == 0) {
        printf("[ERROR] 处理重传失败：参数无效（conn=%p, epsn=%u）\n", conn, epsn);
        return -1;
    }

    *retrans_count = 0;
    *retrans_addrs = NULL;

    uint32_t psn_start = conn->start_psn;
    uint32_t psn_end = conn->end_psn;

    if (psn_start == 0 || psn_start > psn_end) {
        printf("[WARN] 连接无有效PSN范围，无需处理重传\n");
        return 0;
    }

    printf("\n[RETRANS] 开始处理ePSN=%u的重传请求：\n", epsn);
    printf("原有效PSN范围：%u~%u\n", psn_start, psn_end);

    // 步骤1：删除psn < epsn的数据包（释放内存，环形数组置0）
    int delete_count = 0;
    for (uint32_t psn = psn_start; psn < epsn && psn <= psn_end; psn++) {
        int ring_index = psn % RING_BUFFER_SIZE;
        if (ring_index < 0 || ring_index >= RING_BUFFER_SIZE) {
            continue;
        }

        if (conn->ring_buffer[ring_index] != 0) {
            // 释放内存块
            unsigned char* mem_block = (unsigned char*)(uintptr_t)conn->ring_buffer[ring_index];
            free(mem_block);
            conn->ring_buffer[ring_index] = 0;
            delete_count++;
            printf("[DELETE] PSN=%u → 环形数组索引=%d | 内存块已释放（psn < ePSN）\n",
                   psn, ring_index);
        }
    }

    // 步骤2：收集psn ≥ epsn的数据包地址（用于重传）
    int retrans_temp_count = 0;
    // 先统计数量
    for (uint32_t psn = epsn; psn <= psn_end; psn++) {
        int ring_index = psn % RING_BUFFER_SIZE;
        if (ring_index < 0 || ring_index >= RING_BUFFER_SIZE) {
            continue;
        }

        if (conn->ring_buffer[ring_index] != 0) {
            retrans_temp_count++;
        }
    }

    if (retrans_temp_count > 0) {
        // 分配内存存储重传地址
        *retrans_addrs = (uint64_t*)malloc(sizeof(uint64_t) * retrans_temp_count);
        if (!*retrans_addrs) {
            printf("[ERROR] 分配重传地址数组失败\n");
            return -1;
        }

        // 收集地址
        int idx = 0;
        for (uint32_t psn = epsn; psn <= psn_end; psn++) {
            int ring_index = psn % RING_BUFFER_SIZE;
            if (ring_index < 0 || ring_index >= RING_BUFFER_SIZE) {
                continue;
            }

            if (conn->ring_buffer[ring_index] != 0) {
                (*retrans_addrs)[idx++] = conn->ring_buffer[ring_index];
                printf("[COLLECT] PSN=%u → 环形数组索引=%d | 地址=0x%lx（用于重传）\n",
                       psn, ring_index, (uintptr_t)conn->ring_buffer[ring_index]);
            }
        }
        *retrans_count = idx;
    }

    // 步骤3：更新连接的start_psn为epsn（因为psn < epsn的都删了）
    conn->start_psn = epsn;
    if (conn->start_psn > conn->end_psn) {
        // 所有包都被删除，重置PSN参数
        conn->start_psn = 0;
        conn->end_psn = 0;
        conn->current_psn = 0;
    }

    printf("[RETRANS] 重传处理完成：删除psn<ePSN的包数量=%d | 收集重传包数量=%d\n",
           delete_count, *retrans_count);
    printf("[UPDATE] 连接新的有效PSN范围：%u~%u\n", conn->start_psn, conn->end_psn);

    return 0;
}

// 6. 释放指定PSN对应的内存块，并清空环形数组对应位置
int free_packet_by_psn(ConnectionCache* conn, uint32_t psn) {
    if (!conn) {
        printf("[ERROR] 释放数据包失败：连接缓存为空\n");
        return -1;
    }

    int ring_index = psn % RING_BUFFER_SIZE;
    if (ring_index < 0 || ring_index >= RING_BUFFER_SIZE) {
        printf("[ERROR] 释放数据包失败：PSN=%u 对应的索引超出范围（索引=%d）\n", psn, ring_index);
        return -1;
    }

    // 释放内存块
    if (conn->ring_buffer[ring_index] != 0) {
        unsigned char* mem_block = (unsigned char*)(uintptr_t)conn->ring_buffer[ring_index];
        free(mem_block);
        conn->ring_buffer[ring_index] = 0; // 清空环形数组位置
        printf("[FREE] PSN=%u → 环形数组索引=%d | 内存块已释放\n", psn, ring_index);
    } else {
        printf("[WARN] PSN=%u → 环形数组索引=%d | 地址为空，无需释放\n", psn, ring_index);
    }

    return 0;
}

// 7. IP字符串转网络字节序的uint32_t
uint32_t ip_str_to_uint(const char* ip) {
    struct in_addr addr;
    if (inet_pton(AF_INET, ip, &addr) != 1) {
        printf("[ERROR] 无效的IP地址：%s\n", ip);
        return 0;
    }
    return addr.s_addr; // 网络字节序
}
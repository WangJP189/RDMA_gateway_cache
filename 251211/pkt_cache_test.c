/*
编译命令：
gcc pkt_cache_test.c pkt_cache.c -o pkt_cache_test -lpthread -lrdmacm -libverbs

运行命令：
sudo ./pkt_cache_test
*/


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include "pkt_cache.h"

// 生成随机RoCEv2数据包（模拟真实格式）
void generate_roce_packet(unsigned char *buf, int *len, uint32_t psn) {
    // 真实RoCEv2包头结构（基于WireShark捕获）
    struct roce_header {
        // Base Transport Header (8字节)
        uint8_t opcode;          // 操作码 (SEND First=0x00)
        uint8_t flags;           // 标志位 (0x00)
        uint16_t pkey;           // 分区键 (0xffff)
        uint32_t dst_qp;         // 目的QP号
        // Extended Transport Header (可选，此处简化)
        uint32_t psn;            // 包序列号
        uint32_t invariant_crc;  // CRC校验 (占位)
        uint8_t payload[0]; // 柔性数组成员（修正点）
    } __attribute__((packed));

    // 固定MTU=1024字节（数据部分）
    int payload_len = 1024;  // 真实MTU数据长度
    *len = sizeof(struct roce_header) + payload_len;

    struct roce_header *hdr = (struct roce_header*)buf;
    hdr->opcode = 0x00;               // SEND First
    hdr->flags = 0x00;                // 无特殊标志
    hdr->pkey = htons(0xffff);        // 分区键
    hdr->dst_qp = htonl(0x000011);    // 目的QP（示例值）
    hdr->psn = htonl(psn);            // 网络字节序PSN
    hdr->invariant_crc = 0x00000000;  // 临时占位

    // 填充载荷（固定长度）
    memset(hdr->payload, 0x00, payload_len);
    // 可添加简单模式填充（如递增字节）
    for (int i = 0; i < payload_len; i++) {
        hdr->payload[i] = (psn + i) % 256;
    }

    printf("[PKT] 生成RoCEv2包: 总长度=%d, 包头=%zu, 载荷=%d, PSN=%u\n",
           *len, sizeof(struct roce_header), payload_len, psn);
}

// 转换IP字符串到网络字节序
// uint32_t ip_str_to_uint(const char *ip) {
//     struct in_addr addr;
//     inet_pton(AF_INET, ip, &addr);
//     return addr.s_addr;
// }

// 模拟300个乱序RoCEv2数据包并缓存
void simulate_roce_traffic() {
    // 单连接测试（聚焦缓存逻辑）
    const char *src_ip = "192.168.239.132";
    const char *dst_ip = "192.168.239.134";
    const uint16_t src_port = 4791;
    const uint16_t dst_port = 4791;
    const uint32_t src_qp = 1001;
    const uint32_t dest_qp = 2001;

    int packet_count = 200;

    unsigned char pkt_buf[MTU_SIZE];
    int pkt_len;
    int total_pkts = packet_count;
    int success_count = 0;
    int fail_count = 0;

    // 生成乱序PSN数组（1~300乱序）
    uint32_t psn_list[packet_count];
    for (int i = 0; i < packet_count; i++) {
        psn_list[i] = i + 1;
    }
    // // 打乱PSN顺序（模拟网络乱序）
    // for (int i = 299; i > 0; i--) {
    //     int j = rand() % (i + 1);
    //     uint32_t temp = psn_list[i];
    //     psn_list[i] = psn_list[j];
    //     psn_list[j] = temp;
    // }

    printf("===== 开始模拟RoCEv2数据包 =====\n");
    printf("连接: %s:%u (QP%u) -> %s:%u (QP%u)\n",
           src_ip, src_port, src_qp,
           dst_ip, dst_port, dest_qp);
    printf("=========================================\n");

    // 逐个发送数据包
    for (int i = 0; i < total_pkts; i++) {
        uint32_t psn = psn_list[i];
        // 生成RoCEv2包
        generate_roce_packet(pkt_buf, &pkt_len, psn);

        // 添加到缓存
        int ret = add_packet_to_cache(
            src_ip, dst_ip,
            src_port, dst_port,
            src_qp, dest_qp,
            psn, pkt_buf, pkt_len
        );

        // 统计结果
        if (ret == 0) {
            success_count++;
        } else {
            fail_count++;
        }

        // 每50个包输出进度
        if ((i + 1) % 50 == 0) {
            printf("已发送 %d/%d 包 | 成功: %d | 失败: %d | 当前PSN: %u \n",
                   i + 1, total_pkts, success_count, fail_count, psn);
        }

        // 模拟网络延迟（触发超时刷新）
        usleep(1000);
    }

    printf("\n===== 数据包发送完成 =====\n");
    printf("总计发送: %d | 成功缓存: %d | 缓存失败: %d\n",
           total_pkts, success_count, fail_count);
}

// 验证缓存结果（检查内存中的数据包）
void verify_cache_result() {
    printf("\n===== 开始验证缓存结果 =====\n");

    // 等待缓冲区全部刷入内存
    printf("等待缓冲区批量刷入内存（%dms超时）...\n", BATCH_TIMEOUT_MS * 2);
    usleep(BATCH_TIMEOUT_MS * 2 * 1000);

    // 打印所有连接状态
    print_all_connections_status();

    // 模拟重传请求（ePSN=100）
    printf("\n===== 模拟重传请求（ePSN=100）=====\n");
    // 找到第一个连接缓存
    struct connection_cache *cache = NULL;
    if (g_cache_mgr && g_cache_mgr->conn_caches && g_cache_mgr->total_connections > 0) {
        for (size_t i = 0; i < g_cache_mgr->max_connections; i++) {
            if (g_cache_mgr->conn_caches[i]) {
                cache = g_cache_mgr->conn_caches[i];
                break;
            }
        }
    }

    if (cache) {
        struct cached_packet *retrans_pkts = NULL;
        size_t retrans_count = 0;

        // 处理重传请求
        int ret = process_retransmit_request(cache, 100, &retrans_pkts, &retrans_count);

        if (ret == 0) {
            printf("重传包数量: %zu (PSN ≥ 100)\n", retrans_count);
            if (retrans_pkts) {
                // 打印前5个重传包信息
                int show_count = retrans_count > 5 ? 5 : retrans_count;
                for (int i = 0; i < show_count; i++) {
                    printf("  重传包 %d: PSN=%u, 长度=%d\n",
                           i + 1, retrans_pkts[i].psn, retrans_pkts[i].data_len);
                }
                free(retrans_pkts);
            }
        } else {
            printf("重传请求处理失败！\n");
        }
    } else {
        printf("未找到连接缓存，无法验证重传逻辑！\n");
    }
}

int main() {
    // 初始化随机数
    srand(time(NULL));

    // 1. 初始化缓存管理器
    printf("===== 初始化缓存管理器 =====\n");
    g_cache_mgr = init_cache_manager(10);  // 最大10个连接
    if (!g_cache_mgr) {
        fprintf(stderr, "缓存管理器初始化失败！\n");
        return -1;
    }
    printf("缓存管理器初始化成功（最大连接数: %zu）\n", g_cache_mgr->max_connections);

    // 2. 模拟RoCEv2流量
    simulate_roce_traffic();

    // 3. 验证缓存结果
    verify_cache_result();

    // 4. 清理资源
    printf("\n===== 清理缓存资源 =====\n");
    destroy_cache_manager(g_cache_mgr);
    printf("缓存资源清理完成！\n");

    return 0;
}
/*
编译命令：
gcc pkt_cache_test.c pkt_cache.c -o pkt_cache_test -lpthread -lrdmacm -libverbs

运行命令：
./pkt_cache_test
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include "pkt_cache.h"

// 生成模拟RDMA数据包（RoCEv2格式，简化版）
void generate_rdma_packet(unsigned char* buf, int* len, uint32_t psn) {
    // 简化的RoCEv2数据包结构（实际可替换为真实RDMA数据包）
    const int header_len = 16; // 模拟RoCEv2包头长度
    // 数据包总长度：包头+随机载荷（范围：128~4096字节，不超过内存块可用空间）
    int payload_len = 128 + (rand() % (4096 - 128));
    *len = header_len + payload_len;

    // 填充包头（简单标识）
    memset(buf, 0, header_len);
    memcpy(buf, "RDMA_PKT", 8); // 包头标识
    memcpy(buf + 8, &psn, 4);   // 包头存储PSN
    // 填充载荷（随机数据，与PSN关联）
    for (int i = 0; i < payload_len; i++) {
        buf[header_len + i] = (psn + i) % 256;
    }

    printf("[PKT] 生成RDMA数据包：PSN=%u | 总长度=%d（包头=%d + 载荷=%d）\n",
           psn, *len, header_len, payload_len);
}

// 模拟300个乱序RDMA数据包生成、缓存，以及丢包模拟
void simulate_rdma_traffic(uint32_t* sent_psns, int total_pkts, int* lost_psns, int lost_count) {
    // 固定连接三元组（测试用）
    const char* src_ip_str = "192.168.239.132";
    const char* dst_ip_str = "192.168.239.134";
    const uint32_t dst_qp = 2001;

    // 构建连接键
    connection_key key;
    key.src_ip = ip_str_to_uint(src_ip_str);
    key.dst_ip = ip_str_to_uint(dst_ip_str);
    key.dst_qp = dst_qp;

    // 初始化发送的PSN列表（1~300）
    for (int i = 0; i < total_pkts; i++) {
        sent_psns[i] = i + 1;
    }

    // 打乱PSN顺序（模拟网络乱序）
    for (int i = total_pkts - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        uint32_t temp = sent_psns[i];
        sent_psns[i] = sent_psns[j];
        sent_psns[j] = temp;
    }

    // 随机选择丢包的PSN（从sent_psns中选lost_count个）
    printf("\n[SIM] 开始模拟丢包：共选择%d个PSN丢弃\n", lost_count);
    for (int i = 0; i < lost_count; i++) {
        int random_idx = rand() % total_pkts;
        lost_psns[i] = sent_psns[random_idx];
        // 标记为已丢弃（设为0）
        sent_psns[random_idx] = 0;
        printf("[LOST] 选择丢弃PSN=%u\n", lost_psns[i]);
    }

    printf("\n===== 开始模拟RDMA数据包发送（乱序，含丢包）=====\n");
    printf("连接三元组：src_ip=%s, dst_ip=%s, dst_qp=%u\n",
           src_ip_str, dst_ip_str, dst_qp);
    printf("总数据包数量：%d | 丢包数量：%d\n", total_pkts, lost_count);
    printf("=============================================\n");

    unsigned char pkt_buf[MEM_BLOCK_SIZE - sizeof(memblock_header)]; // 数据包缓冲区（不超过内存块可用空间）
    int pkt_len;
    int success_count = 0;
    int fail_count = 0;

    // 逐个发送/缓存数据包（跳过丢包的PSN）
    for (int i = 0; i < total_pkts; i++) {
        uint32_t psn = sent_psns[i];
        if (psn == 0) {
            // 丢包的PSN，跳过缓存
            printf("[SKIP] PSN=%u 被丢弃，不缓存\n", lost_psns[fail_count]);
            fail_count++;
            continue;
        }

        // 生成RDMA数据包
        generate_rdma_packet(pkt_buf, &pkt_len, psn);

        // 缓存数据包
        int ret = cache_rdma_packet(&key, psn, pkt_buf, pkt_len);
        if (ret == 0) {
            success_count++;
        } else {
            fail_count++;
        }

        // 每50个包输出进度
        if ((i + 1) % 50 == 0) {
            printf("\n[PROGRESS] 已处理 %d/%d 包 | 成功缓存：%d | 失败/丢包：%d\n",
                   i + 1, total_pkts, success_count, fail_count);
        }

        // 模拟网络延迟
        usleep(1000);
    }

    printf("\n===== 数据包发送完成 =====\n");
    printf("总计发送：%d | 成功缓存：%d | 丢包/失败：%d\n",
           total_pkts, success_count, fail_count);
}

int main() {
    // 初始化随机数
    srand(time(NULL));

    // 配置参数
    const int total_pkts = 300;    // 总数据包数量
    const int lost_count = 5;      // 丢包数量
    uint32_t sent_psns[total_pkts];// 发送的PSN列表
    int lost_psns[lost_count];     // 丢包的PSN列表
    uint32_t found_lost_psns[total_pkts]; // 查找出的丢包PSN列表

    // 步骤1：模拟RDMA流量（含丢包）
    simulate_rdma_traffic(sent_psns, total_pkts, lost_psns, lost_count);

    // 步骤2：获取连接的环形数组（测试用连接三元组）
    connection_key key;
    key.src_ip = ip_str_to_uint("192.168.239.132");
    key.dst_ip = ip_str_to_uint("192.168.239.134");
    key.dst_qp = 2001;
    uint64_t* ring_buffer = find_connection_ring_buffer(&key);
    if (!ring_buffer) {
        fprintf(stderr, "未找到连接的环形数组，程序退出\n");
        return -1;
    }

    // 步骤3：ePSN查找丢包（ePSN范围：1~300）
    uint32_t epsn_start = 1;
    uint32_t epsn_end = total_pkts;
    int found_lost_count = find_lost_packets(ring_buffer, epsn_start, epsn_end, found_lost_psns, total_pkts);

    // 步骤4：验证丢包结果
    printf("\n===== 丢包结果验证 =====\n");
    printf("预期丢包PSN：");
    for (int i = 0; i < lost_count; i++) {
        printf("%u ", lost_psns[i]);
    }
    printf("\n查找出的丢包PSN：");
    for (int i = 0; i < found_lost_count; i++) {
        printf("%u ", found_lost_psns[i]);
    }
    printf("\n");

    // 步骤5：释放所有数据包内存（清理资源）
    printf("\n===== 开始释放所有数据包内存 =====\n");
    for (uint32_t psn = epsn_start; psn <= epsn_end; psn++) {
        free_packet_by_psn(ring_buffer, psn);
    }

    // 步骤6：释放连接缓存（清理资源）
    for (int i = 0; i < g_connection_count; i++) {
        free(g_connection_table[i]);
        g_connection_table[i] = NULL;
    }
    g_connection_count = 0;
    printf("\n[CLEAN] 所有资源已清理完成\n");

    return 0;
}
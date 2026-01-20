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
    // 简化的RoCEv2数据包结构
    const int header_len = 16; // 模拟RoCEv2包头长度
    // 数据包总长度：包头+随机载荷（128~4096字节，不超过内存块可用空间）
    int payload_len = 128 + (rand() % (4096 - 128));
    *len = header_len + payload_len;

    // 填充包头
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

// 模拟乱序RDMA数据包生成、缓存，以及丢包模拟
void simulate_rdma_traffic(ConnectionCache* conn, uint32_t* sent_psns, int total_pkts, int* lost_psns, int lost_count) {
    // 初始化发送的PSN列表（1~total_pkts）
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

    // 随机选择丢包的PSN
    printf("\n[SIM] 开始模拟丢包：共选择%d个PSN丢弃\n", lost_count);
    for (int i = 0; i < lost_count; i++) {
        int random_idx = rand() % total_pkts;
        lost_psns[i] = sent_psns[random_idx];
        sent_psns[random_idx] = 0; // 标记为丢弃
        printf("[LOST] 选择丢弃PSN=%u\n", lost_psns[i]);
    }

    printf("\n===== 开始模拟RDMA数据包发送（乱序，含丢包）=====\n");
    printf("总数据包数量：%d | 丢包数量：%d\n", total_pkts, lost_count);
    printf("=============================================\n");

    unsigned char pkt_buf[MEM_BLOCK_SIZE - sizeof(MemBlockHeader)]; // 数据包缓冲区
    int pkt_len;
    int success_count = 0;
    int fail_count = 0;

    // 逐个发送/缓存数据包
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
        int ret = cache_rdma_packet(conn, psn, pkt_buf, pkt_len);
        if (ret == 0) {
            success_count++;
        } else {
            fail_count++;
        }

        // 模拟网络延迟（微秒级，不影响老化时间）
        usleep(1000);
    }

    printf("\n===== 数据包发送完成 =====\n");
    printf("总计发送：%d | 成功缓存：%d | 丢包/失败：%d\n",
           total_pkts, success_count, fail_count);
}

int main() {
    // 初始化随机数
    srand((unsigned int)time(NULL));

    // 配置参数
    const int total_pkts = 100;    // 总数据包数量
    const int lost_count = 2;      // 丢包数量
    uint32_t sent_psns[total_pkts];// 发送的PSN列表
    int lost_psns[lost_count];     // 丢包的PSN列表
    uint32_t found_lost_psns[total_pkts]; // 查找出的丢包PSN列表
    uint64_t* retrans_addrs = NULL;// 重传的数据包地址列表
    int retrans_count = 0;

    // 步骤1：构建测试连接键
    ConnectionKey key;
    key.src_ip = ip_str_to_uint("192.168.239.132");
    key.dst_ip = ip_str_to_uint("192.168.239.134");
    key.dst_qp = 2001;

    // 步骤2：添加连接到表（若不存在）
    if (find_connection_by_key(&key) == NULL) {
        add_connection_to_table(&key);
    }

    // 步骤3：获取连接缓存
    ConnectionCache* conn = find_connection_by_key(&key);
    if (!conn) {
        fprintf(stderr, "未找到连接的缓存结构，程序退出\n");
        return -1;
    }

    // 步骤4：模拟RDMA流量（含丢包）
    simulate_rdma_traffic(conn, sent_psns, total_pkts, lost_psns, lost_count);

    // 步骤5：在连接有效PSN范围内查找丢包
    int found_lost_count = find_lost_packets(conn, found_lost_psns, total_pkts);

    // 步骤6：验证丢包结果
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

    // 步骤7：模拟老化处理（等待超过最大老化时间）
    printf("\n===== 等待数据包过期（%d ms）=====\n", MAX_AGE_MILLISECONDS);
    usleep(MAX_AGE_MILLISECONDS * 2); // 等待120ms，确保数据包过期
    // 获取当前毫秒级时间戳，传入老化函数
    uint64_t current_ts_ms = get_current_timestamp_ms();
    int expired_count = age_out_expired_packets(conn, current_ts_ms);
    printf("[TEST] 老化处理完成，共清理过期数据包：%d个\n", expired_count);

    // 步骤8：模拟ePSN重传（选择ePSN=5）
    uint32_t epsn = 5;
    int ret = process_retransmit_by_epsn(conn, epsn, &retrans_addrs, &retrans_count);
    if (ret == 0 && retrans_count > 0) {
        printf("\n===== 重传包信息 =====\n");
        printf("ePSN=%u 对应的重传包数量：%d\n", epsn, retrans_count);
        printf("重传包地址列表：");
        for (int i = 0; i < retrans_count; i++) {
            printf("0x%lx ", (uintptr_t)retrans_addrs[i]);
        }
        printf("\n");
        free(retrans_addrs); // 释放重传地址数组
    }

    // 步骤9：释放所有剩余数据包内存
    printf("\n===== 开始释放所有剩余数据包内存 =====\n");
    uint32_t psn_start = conn->start_psn;
    uint32_t psn_end = conn->end_psn;
    if (psn_start != 0 && psn_start <= psn_end) {
        for (uint32_t psn = psn_start; psn <= psn_end; psn++) {
            free_packet_by_psn(conn, psn);
        }
    }

    // 步骤10：释放连接缓存（清理资源）
    for (int i = 0; i < g_connection_count; i++) {
        free(g_connection_table[i]);
        g_connection_table[i] = NULL;
    }
    g_connection_count = 0;
    printf("\n[CLEAN] 所有资源已清理完成\n");

    return 0;
}
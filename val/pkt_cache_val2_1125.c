/*
编译命令：
gcc pkt_cache_val2_1125.c ../251125/pkt_cache.c -o pkt_cache_val2_1125 -lpthread -lpcap

运行命令：
sudo ./pkt_cache_val2_1125

加入gdb调试后的命令：
编译命令：
gcc -g pkt_cache_val2_1125.c ../251125/pkt_cache.c -o pkt_cache_val2_1125 -lpthread -lpcap

运行命令：
sudo gdb ./pkt_cache_val2_1125

更新说明：
1. 修复内存泄漏问题
2. 增加线程同步超时机制
3. 优化批量处理逻辑，减少线程切换
4. 限制PCAP捕获速率，避免缓冲区溢出
5. 增加内存使用监控


gcc pkt_cache_val2_1125.c -o pkt_cache_val2_1125 -lpcap
*/

// RDMA缓存验证程序 - 简化稳定版本
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pcap.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "../251125/pkt_cache.c"  // 直接包含缓存模块

#define RDMA_PORT 4791
#define ETH_HDR_LEN 14
#define MAX_PACKETS 10000  // 最大处理包数，防止无限运行

// 全局变量
pcap_t *handle;
int packet_count = 0;

// 数据包处理回调
void packet_handler(u_char *user, const struct pcap_pkthdr *hdr, const u_char *packet) {
    // 限制处理包数，防止无限运行
    if (packet_count++ >= MAX_PACKETS) {
        pcap_breakloop(handle);
        return;
    }
    
    // 解析以太网头部
    const struct ip *ip_hdr = (struct ip*)(packet + ETH_HDR_LEN);
    if (ip_hdr->ip_v != 4) return;

    // 解析UDP头部
    int ip_header_len = ip_hdr->ip_hl * 4;
    const struct udphdr *udp_hdr = (struct udphdr*)((u_char*)ip_hdr + ip_header_len);
    
    // 只处理目标端口4791的包
    if (ntohs(udp_hdr->dest) != RDMA_PORT) return;

    // 提取负载
    int udp_total_len = ntohs(udp_hdr->len);
    int payload_len = udp_total_len - 8; // UDP头长度
    const unsigned char *payload = (u_char*)udp_hdr + 8;

    // 模拟提取RDMA信息（实际应从IB头部解析）
    static uint32_t psn_counter = 1;
    uint32_t src_qp = 0x1234;
    uint32_t dest_qp = 0x5678;
    uint32_t psn = psn_counter++;

    // 调用缓存函数
    int ret = add_to_batch_queue(
        inet_ntoa(ip_hdr->ip_src),
        inet_ntoa(ip_hdr->ip_dst),
        ntohs(udp_hdr->source),
        ntohs(udp_hdr->dest),
        src_qp, dest_qp,
        psn, payload, payload_len
    );

    // 打印处理结果
    if (ret == 0) {
        if (psn % 100 == 0) { // 每100个包打印一次
            printf("成功缓存数据包 #%d: %s:%d -> %s:%d PSN=%u 长度=%d\n",
                   packet_count,
                   inet_ntoa(ip_hdr->ip_src), ntohs(udp_hdr->source),
                   inet_ntoa(ip_hdr->ip_dst), ntohs(udp_hdr->dest),
                   psn, payload_len);
        }
    } else {
        printf("缓存失败: PSN=%u\n", psn);
    }

    // 定期打印状态
    if (packet_count % 500 == 0) {
        print_all_connections_status();
    }
}

int main() {
    char errbuf[PCAP_ERRBUF_SIZE];
    struct bpf_program fp;
    char filter_exp[100];

    printf("=== RDMA缓存验证程序启动 ===\n");

    // 初始化缓存管理器
    g_cache_mgr = init_cache_manager(16); // 小哈希表
    if (!g_cache_mgr) {
        fprintf(stderr, "缓存管理器初始化失败\n");
        return 1;
    }

    // 打开网卡
    handle = pcap_open_live("eth0", BUFSIZ, 1, 1000, errbuf);
    if (!handle) {
        fprintf(stderr, "无法打开网卡eth0: %s\n", errbuf);
        destroy_cache_manager(g_cache_mgr);
        return 1;
    }

    // 设置过滤器
    snprintf(filter_exp, sizeof(filter_exp), "udp dst port %d", RDMA_PORT);
    if (pcap_compile(handle, &fp, filter_exp, 0, PCAP_NETMASK_UNKNOWN) == -1) {
        fprintf(stderr, "过滤器编译失败: %s\n", pcap_geterr(handle));
        pcap_close(handle);
        destroy_cache_manager(g_cache_mgr);
        return 1;
    }
    
    if (pcap_setfilter(handle, &fp) == -1) {
        fprintf(stderr, "设置过滤器失败: %s\n", pcap_geterr(handle));
        pcap_close(handle);
        destroy_cache_manager(g_cache_mgr);
        return 1;
    }

    printf("开始监听eth0网卡，目标端口: %d\n", RDMA_PORT);
    printf("最大处理包数: %d\n", MAX_PACKETS);
    printf("按Ctrl+C停止程序\n\n");

    // 开始捕获
    pcap_loop(handle, 0, packet_handler, NULL);

    // 清理资源
    pcap_close(handle);
    destroy_cache_manager(g_cache_mgr);
    
    printf("\n程序正常结束，共处理 %d 个数据包\n", packet_count);
    return 0;
}
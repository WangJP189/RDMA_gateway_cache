/*
编译命令：
gcc pkt_cache_val2_1110.c ../251110/pkt_cache.c -o pkt_cache_val2_1110 -lpthread -lpcap

运行命令：
sudo ./pkt_cache_val2_1110
*/


// pkt_cache_val2.c - 监听eth0上目标端口4791的RDMA报文并缓存
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pcap.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <time.h>
#include<unistd.h>

#include "../251110/pkt_cache.h"

// 全局PCAP句柄
pcap_t *handle;

// 协议常量定义
#define ETH_HDR_LEN 14
#define IP_HDR_LEN  20
#define UDP_HDR_LEN 8
#define RDMA_PORT   4791  // 目标端口筛选

// 打印数据包基本信息
void print_packet_info(const struct ip *ip_hdr, const struct udphdr *udp_hdr, 
                      const unsigned char *payload, int payload_len) {
    printf("\n=== 捕获新报文 ===\n");
    printf("源IP: %s:%d\n", inet_ntoa(ip_hdr->ip_src), ntohs(udp_hdr->source));
    printf("目标IP: %s:%d\n", inet_ntoa(ip_hdr->ip_dst), ntohs(udp_hdr->dest));
    printf("IP协议: %d, 总长度: %d bytes\n", ip_hdr->ip_p, ntohs(ip_hdr->ip_len));
    printf("UDP长度: %d bytes, 负载长度: %d bytes\n", 
           ntohs(udp_hdr->len), payload_len);
}

// 数据包处理回调函数
void packet_handler(u_char *user, const struct pcap_pkthdr *hdr, const u_char *packet) {
    // 解析以太网头部
    const struct ip *ip_hdr = (struct ip*)(packet + ETH_HDR_LEN);
    if (ip_hdr->ip_v != 4) {  // 只处理IPv4
        printf("忽略非IPv4报文\n");
        return;
    }

    // 计算IP头部长度（考虑选项字段）
    int ip_header_len = ip_hdr->ip_hl * 4;
    if (ip_header_len < IP_HDR_LEN) {
        printf("无效的IP头部长度: %d\n", ip_header_len);
        return;
    }

    // 解析UDP头部
    const struct udphdr *udp_hdr = (struct udphdr*)((u_char*)ip_hdr + ip_header_len);
    if (ntohs(udp_hdr->dest) != RDMA_PORT) {  // 筛选目标端口4791
        return;
    }

    // 计算UDP负载
    int udp_total_len = ntohs(udp_hdr->len);
    int payload_len = udp_total_len - UDP_HDR_LEN;
    const unsigned char *payload = (u_char*)udp_hdr + UDP_HDR_LEN;

    // 打印报文基本信息
    print_packet_info(ip_hdr, udp_hdr, payload, payload_len);

    // 提取RDMA相关字段（这里简化处理，实际应根据RDMA协议解析）
    // 注意：真实环境中需根据实际QP号解析逻辑替换以下默认值
    uint32_t src_qp = 10;    // 示例值，实际应从报文中解析
    uint32_t dest_qp = 20;   // 示例值，实际应从报文中解析
    uint8_t service_type = 0;
    uint16_t pkey = 0xffff;
    static uint32_t psn = 1; // 示例PSN自增，实际应从报文中解析

    // 将报文添加到缓存
    int ret = add_to_connection_cache(
        inet_ntoa(ip_hdr->ip_src),    // 源IP
        inet_ntoa(ip_hdr->ip_dst),    // 目标IP
        ntohs(udp_hdr->source),       // 源端口
        ntohs(udp_hdr->dest),         // 目标端口
        src_qp, dest_qp,
        service_type, pkey,
        psn++,                        // PSN
        payload, payload_len
    );

    if (ret != 0) {
        fprintf(stderr, "缓存失败！返回值: %d\n", ret);
    } else {
        printf("缓存成功，当前PSN: %u\n", psn - 1);
    }

    // 每捕获5个包打印一次缓存状态
    if (psn % 5 == 0) {
        printf("\n===== 缓存状态快照 =====\n");
        print_all_connections_status();
    }
}

// 定期清理过期连接的线程
void *cleanup_thread(void *arg) {
    while (1) {
        sleep(60);  // 每分钟清理一次
        printf("\n===== 执行过期连接清理 =====");
        cleanup_expired_connections();
        print_all_connections_status();
    }
    return NULL;
}

int main() {
    char errbuf[PCAP_ERRBUF_SIZE];
    struct bpf_program fp;
    char filter_exp[128];
    bpf_u_int32 net;

    // 初始化缓存管理器
    g_cache_mgr = init_cache_manager(
        1024,    // 哈希表大小
        100,     // 最大连接数
        1000,    // 每连接最大报文数
        10,      // 每连接最大字节数(MB)
        300      // 连接超时时间(秒)
    );
    if (!g_cache_mgr) {
        fprintf(stderr, "缓存管理器初始化失败\n");
        return 1;
    }

    // 启动清理线程
    pthread_t tid;
    if (pthread_create(&tid, NULL, cleanup_thread, NULL) != 0) {
        perror("创建清理线程失败");
        return 1;
    }

    // 打开网卡进行混杂模式监听
    handle = pcap_open_live("eth0", BUFSIZ, 1, 1000, errbuf);
    if (handle == NULL) {
        fprintf(stderr, "无法打开设备eth0: %s\n", errbuf);
        return 1;
    }

    // 设置过滤器：目标端口4791的UDP报文
    snprintf(filter_exp, sizeof(filter_exp), "udp dst port %d", RDMA_PORT);
    if (pcap_compile(handle, &fp, filter_exp, 0, net) == -1) {
        fprintf(stderr, "过滤器编译失败: %s\n", pcap_geterr(handle));
        return 1;
    }
    if (pcap_setfilter(handle, &fp) == -1) {
        fprintf(stderr, "设置过滤器失败: %s\n", pcap_geterr(handle));
        return 1;
    }
    printf("开始监听eth0上目标端口%d的UDP报文...\n", RDMA_PORT);
    printf("使用ib_send_bw进行RDMA通信以生成测试流量\n");

    // 开始捕获报文
    pcap_loop(handle, 0, packet_handler, NULL);

    // 清理资源（正常情况下不会执行到这里）
    pcap_close(handle);
    return 0;
}
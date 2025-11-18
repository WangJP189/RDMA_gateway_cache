/*
编译命令：
gcc pkt_cache_val2_1117.c ../251117/pkt_cache.c -o pkt_cache_val2_1117 -lpthread -lpcap

运行命令：
sudo ./pkt_cache_val2_1117
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pcap.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>

#include "../251117/pkt_cache.h"

// 全局PCAP句柄
pcap_t *handle;

// 协议常量定义
#define ETH_HDR_LEN 14
#define IP_HDR_LEN  20
#define UDP_HDR_LEN 8
#define RDMA_PORT   4791  // 目标端口筛选
#define DEFAULT_WINDOW_SIZE 8192  // 默认窗口大小
#define STATS_INTERVAL 5  // 统计信息输出间隔（秒）

// 性能统计结构体
typedef struct {
    uint64_t total_packets;      // 总处理报文数
    uint64_t cached_packets;     // 成功缓存报文数
    uint64_t dropped_packets;    // 丢弃报文数
    uint64_t total_bytes;        // 总处理字节数
    uint64_t cached_bytes;       // 缓存字节数
    uint64_t cache_hits;         // 缓存命中数（假设存在查询操作）
    uint64_t cache_misses;       // 缓存未命中数
    struct timeval start_time;   // 统计开始时间
    pthread_mutex_t stats_lock;  // 统计信息互斥锁
} PerformanceStats;

// 全局性能统计变量
PerformanceStats perf_stats;

// 初始化性能统计
void init_performance_stats() {
    memset(&perf_stats, 0, sizeof(PerformanceStats));
    gettimeofday(&perf_stats.start_time, NULL);
    pthread_mutex_init(&perf_stats.stats_lock, NULL);
}

// 更新性能统计（缓存成功）
void update_stats_cache_success(int payload_len) {
    pthread_mutex_lock(&perf_stats.stats_lock);
    perf_stats.total_packets++;
    perf_stats.cached_packets++;
    perf_stats.total_bytes += payload_len;
    perf_stats.cached_bytes += payload_len;
    pthread_mutex_unlock(&perf_stats.stats_lock);
}

// 更新性能统计（缓存失败）
void update_stats_cache_failed(int payload_len) {
    pthread_mutex_lock(&perf_stats.stats_lock);
    perf_stats.total_packets++;
    perf_stats.dropped_packets++;
    perf_stats.total_bytes += payload_len;
    pthread_mutex_unlock(&perf_stats.stats_lock);
}

// 更新缓存命中统计
void update_stats_cache_hit() {
    pthread_mutex_lock(&perf_stats.stats_lock);
    perf_stats.cache_hits++;
    pthread_mutex_unlock(&perf_stats.stats_lock);
}

// 更新缓存未命中统计
void update_stats_cache_miss() {
    pthread_mutex_lock(&perf_stats.stats_lock);
    perf_stats.cache_misses++;
    pthread_mutex_unlock(&perf_stats.stats_lock);
}

// 计算时间差（秒）
double time_diff(struct timeval *start, struct timeval *end) {
    return (end->tv_sec - start->tv_sec) + 
           (end->tv_usec - start->tv_usec) / 1000000.0;
}

// 打印性能统计信息
void print_performance_stats() {
    struct timeval current_time;
    gettimeofday(&current_time, NULL);
    double elapsed = time_diff(&perf_stats.start_time, &current_time);
    
    pthread_mutex_lock(&perf_stats.stats_lock);
    
    // 计算速率
    double pkt_rate = elapsed > 0 ? perf_stats.total_packets / elapsed : 0;
    double byte_rate = elapsed > 0 ? (perf_stats.total_bytes / 1024.0) / elapsed : 0;
    double cache_ratio = perf_stats.total_packets > 0 ? 
        (double)perf_stats.cached_packets / perf_stats.total_packets * 100 : 0;
    
    // 计算缓存命中率
    double hit_ratio = 0;
    if (perf_stats.cache_hits + perf_stats.cache_misses > 0) {
        hit_ratio = (double)perf_stats.cache_hits / 
                   (perf_stats.cache_hits + perf_stats.cache_misses) * 100;
    }

    printf("\n===== 性能统计信息 =====\n");
    printf("运行时间: %.2f 秒\n", elapsed);
    printf("总处理报文: %llu 个 (%.2f 个/秒)\n", 
           (unsigned long long)perf_stats.total_packets, pkt_rate);
    printf("总处理字节: %llu B (%.2f KB/秒)\n", 
           (unsigned long long)perf_stats.total_bytes, byte_rate);
    printf("缓存成功: %llu 个 (%.2f%%)\n", 
           (unsigned long long)perf_stats.cached_packets, cache_ratio);
    printf("缓存丢弃: %llu 个\n", 
           (unsigned long long)perf_stats.dropped_packets);
    printf("缓存命中: %llu 次 (%.2f%%)\n", 
           (unsigned long long)perf_stats.cache_hits, hit_ratio);
    printf("缓存未命中: %llu 次\n", 
           (unsigned long long)perf_stats.cache_misses);
    
    // 获取缓存当前状态（仅使用已定义的成员）
    struct cache_manager *mgr = get_cache_manager();
    if (mgr) {
        // 移除未定义的current_bytes/max_bytes，改用缓存的报文数统计
        printf("当前总连接数: %zu/%zu\n",
               mgr->total_connections, mgr->max_connections);
    }
    printf("=========================\n");
    
    pthread_mutex_unlock(&perf_stats.stats_lock);
}

// 定期打印性能统计的线程
void *stats_thread(void *arg) {
    while (1) {
        sleep(STATS_INTERVAL);
        print_performance_stats();
    }
    return NULL;
}

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

    // 提取RDMA相关字段（示例值）
    uint32_t src_qp = 10;
    uint32_t dest_qp = 20;
    uint8_t service_type = 0;
    uint16_t pkey = 0xffff;
    static uint32_t psn = 1;

    // 获取全局缓存管理器
    struct cache_manager *mgr = get_cache_manager();
    if (!mgr) {
        fprintf(stderr, "获取缓存管理器失败\n");
        update_stats_cache_failed(payload_len);
        return;
    }

    // 将报文添加到缓存
    int ret = add_to_connection_cache(
        inet_ntoa(ip_hdr->ip_src),
        inet_ntoa(ip_hdr->ip_dst),
        ntohs(udp_hdr->source),
        ntohs(udp_hdr->dest),
        src_qp, dest_qp,
        service_type, pkey,
        psn++,
        payload, payload_len,
        DEFAULT_WINDOW_SIZE
    );

    if (ret != 0) {
        fprintf(stderr, "缓存失败！返回值: %d\n", ret);
        update_stats_cache_failed(payload_len);
    } else {
        printf("缓存成功，当前PSN: %u\n", psn - 1);
        update_stats_cache_success(payload_len);
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
        sleep(60);
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

    // 初始化性能统计
    init_performance_stats();

    // 初始化缓存管理器
    struct cache_manager *mgr = init_cache_manager(
        1024,                // 哈希表大小
        100,                 // 最大连接数
        1000,                // 每连接最大报文数
        10,                  // 每连接最大字节数(MB)
        300,                 // 连接超时时间(秒)
        DEFAULT_WINDOW_SIZE  // 默认窗口大小
    );
    if (!mgr) {
        fprintf(stderr, "缓存管理器初始化失败\n");
        return 1;
    }
    // 赋值全局缓存管理器（如果pkt_cache.c中用g_cache_mgr）
    extern struct cache_manager *g_cache_mgr;
    g_cache_mgr = mgr;

    // 启动清理线程
    pthread_t cleanup_tid;
    if (pthread_create(&cleanup_tid, NULL, cleanup_thread, NULL) != 0) {
        perror("创建清理线程失败");
        return 1;
    }

    // 启动性能统计线程
    pthread_t stats_tid;
    if (pthread_create(&stats_tid, NULL, stats_thread, NULL) != 0) {
        perror("创建统计线程失败");
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

    // 清理资源
    pthread_mutex_destroy(&perf_stats.stats_lock);
    pcap_close(handle);
    return 0;
}
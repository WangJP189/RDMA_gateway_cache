/*
编译命令：
gcc pkt_cache_val2_1120.c ../251120/pkt_cache.c -o pkt_cache_val2_1120 -lpthread -lpcap

运行命令：
sudo ./pkt_cache_val2_1120

加入gdb调试后的命令：
编译命令：
gcc -g pkt_cache_val2_1120.c ../251120/pkt_cache.c -o pkt_cache_val2_1120 -lpthread -lpcap

运行命令：
sudo gdb ./pkt_cache_val2_1120

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

#include "../251120/pkt_cache.h"

// 配置参数
#define RDMA_PORT 4791               // RoCEv2默认端口
#define ETH_HDR_LEN 14               // 以太网头部长度
#define IP_HDR_LEN 20                // IP头部最小长度
#define UDP_HDR_LEN 8                // UDP头部长度
#define BATCH_INTERVAL 0.1           // 批量处理间隔(秒)
#define BATCH_MAX_PACKETS 100        // 批量处理最大包数
#define THREAD_COUNT 4               // 工作线程数量
#define STATS_INTERVAL 10            // 性能统计间隔(秒)
#define DEFAULT_WINDOW_SIZE 32       // 默认滑动窗口大小

// 全局批量队列
struct {
    struct packet_data **packets;
    int count;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} g_batch_queue;

// 性能统计结构
struct {
    uint64_t total_captured;  // 总捕获包数
    uint64_t total_cached;    // 总缓存包数
    uint64_t total_bytes;     // 总缓存字节数
    uint64_t total_dropped;   // 因缓存上限丢弃的包数
    struct timeval start_time; // 统计开始时间
    pthread_mutex_t lock;     // 统计锁
} g_perf_stats;

// 数据包结构定义
struct packet_data {
    char *src_ip;
    char *dst_ip;
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t src_qp;
    uint32_t dest_qp;
    uint8_t service_type;
    uint16_t pkey;
    uint32_t psn;
    unsigned char *payload;
    int payload_len;
    uint32_t window_size;
};

// 声明全局缓存管理器
extern struct cache_manager *g_cache_mgr;
pcap_t *handle;

// 计算时间差（秒）
double time_diff(struct timeval *start, struct timeval *end) {
    return (end->tv_sec - start->tv_sec) + 
           (end->tv_usec - start->tv_usec) / 1000000.0;
}

// 打印性能统计
void print_perf_stats() {
    struct timeval current_time;
    gettimeofday(&current_time, NULL);
    double elapsed = time_diff(&g_perf_stats.start_time, &current_time);
    
    pthread_mutex_lock(&g_perf_stats.lock);
    double pkt_rate = elapsed > 0 ? g_perf_stats.total_cached / elapsed : 0;
    double byte_rate = elapsed > 0 ? (g_perf_stats.total_bytes / 1024.0) / elapsed : 0;
    double cache_ratio = g_perf_stats.total_captured > 0 ? 
        (double)g_perf_stats.total_cached / g_perf_stats.total_captured * 100 : 0;

    printf("\n===== 性能统计 =====\n");
    printf("运行时间: %.2f秒\n", elapsed);
    printf("总捕获包数: %llu\n", (unsigned long long)g_perf_stats.total_captured);
    printf("总缓存包数: %llu (%.2f%%)\n", 
           (unsigned long long)g_perf_stats.total_cached, cache_ratio);
    printf("因上限丢弃包数: %llu\n", (unsigned long long)g_perf_stats.total_dropped);
    printf("缓存速率: %.2f包/秒\n", pkt_rate);
    printf("缓存字节: %llu B (%.2f KB/秒)\n", 
           (unsigned long long)g_perf_stats.total_bytes, byte_rate);
    printf("====================\n");
    pthread_mutex_unlock(&g_perf_stats.lock);
}

// 性能统计线程
void *stats_thread(void *arg) {
    while (1) {
        sleep(STATS_INTERVAL);
        printf("[统计线程] ");
        print_perf_stats();
    }
    return NULL;
}

// 打印数据包信息
void print_packet_info(const struct ip *ip_hdr, const struct udphdr *udp_hdr, 
                      const unsigned char *payload, int payload_len) {
    printf("[捕获线程] === 捕获新报文 ===\n");
    printf("[捕获线程] 源IP: %s:%d\n", inet_ntoa(ip_hdr->ip_src), ntohs(udp_hdr->source));
    printf("[捕获线程] 目标IP: %s:%d\n", inet_ntoa(ip_hdr->ip_dst), ntohs(udp_hdr->dest));
    printf("[捕获线程] IP协议: %d, 总长度: %d bytes\n", ip_hdr->ip_p, ntohs(ip_hdr->ip_len));
    printf("[捕获线程] UDP长度: %d bytes, 负载长度: %d bytes\n", 
           ntohs(udp_hdr->len), payload_len);
}

// 批量缓存处理线程
void *batch_cache_thread(void *arg) {
    while (1) {
        pthread_mutex_lock(&g_batch_queue.mutex);
        
        // 等待定时或队列满
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += (time_t)BATCH_INTERVAL;
        ts.tv_nsec += (long)((BATCH_INTERVAL - (time_t)BATCH_INTERVAL) * 1e9);
        if (ts.tv_nsec >= 1e9) {
            ts.tv_sec++;
            ts.tv_nsec -= 1e9;
        }
        
        pthread_cond_timedwait(&g_batch_queue.cond, &g_batch_queue.mutex, &ts);
        
        int count = g_batch_queue.count;
        if (count > 0) {
            printf("[批量线程] 开始处理批量数据，共 %d 个包\n", count);
            
            // 处理每个数据包
            for (int i = 0; i < count; i++) {
                struct packet_data *pkt = g_batch_queue.packets[i];
                
                // 添加到批量处理队列
                int ret = add_to_batch_queue(
                    pkt->src_ip, pkt->dst_ip,
                    pkt->src_port, pkt->dst_port,
                    pkt->src_qp, pkt->dest_qp,
                    pkt->service_type, pkt->pkey,
                    pkt->psn, pkt->payload, pkt->payload_len
                );
                
                // 更新性能统计
                pthread_mutex_lock(&g_perf_stats.lock);
                if (ret == 0) {
                    g_perf_stats.total_cached++;
                    g_perf_stats.total_bytes += pkt->payload_len;
                } else {
                    g_perf_stats.total_dropped++;
                    printf("[批量线程] 缓存达到上限，丢弃数据包 PSN=%u\n", pkt->psn);
                }
                pthread_mutex_unlock(&g_perf_stats.lock);
                
                // 释放数据包
                free(pkt->src_ip);
                free(pkt->dst_ip);
                free(pkt->payload);
                free(pkt);
            }
            
            g_batch_queue.count = 0;
            printf("[批量线程] 批量缓存完成\n");
        }
        
        pthread_mutex_unlock(&g_batch_queue.mutex);
    }
    return NULL;
}

// 定期清理线程
void *cleanup_thread(void *arg) {
    while (1) {
        sleep(60);  // 每分钟清理一次
        printf("[清理线程] ===== 清理过期连接 =====\n");
        cleanup_expired_connections();
        print_all_connections_status();
    }
    return NULL;
}

// 数据包处理回调
void packet_handler(u_char *user, const struct pcap_pkthdr *hdr, const u_char *packet) {
    // 解析以太网头部
    const struct ip *ip_hdr = (struct ip*)(packet + ETH_HDR_LEN);
    if (ip_hdr->ip_v != 4) {  // 只处理IPv4
        return;
    }

    // 计算IP头部长度
    int ip_header_len = ip_hdr->ip_hl * 4;
    if (ip_header_len < IP_HDR_LEN) {
        printf("[捕获线程] 无效IP头部长度: %d\n", ip_header_len);
        return;
    }

    // 解析UDP头部
    const struct udphdr *udp_hdr = (struct udphdr*)((u_char*)ip_hdr + ip_header_len);
    if (ntohs(udp_hdr->dest) != RDMA_PORT) {  // 筛选目标端口
        return;
    }

    // 提取负载
    int udp_total_len = ntohs(udp_hdr->len);
    int payload_len = udp_total_len - UDP_HDR_LEN;
    const unsigned char *payload = (u_char*)udp_hdr + UDP_HDR_LEN;

    // 从RoCEv2报文中提取QP和PSN（简化处理，实际应解析IB头部）
    static uint32_t psn_counter = 1;  // 实际应用中应从报文中解析
    uint32_t src_qp = 0x1234;         // 示例值，实际应从报文中解析
    uint32_t dest_qp = 0x5678;        // 示例值，实际应从报文中解析
    uint8_t service_type = 0;
    uint16_t pkey = 0xffff;
    uint32_t psn = psn_counter++;

    // 调试信息（每100个包打印一次）
    if (psn % 100 == 0) {
        print_packet_info(ip_hdr, udp_hdr, payload, payload_len);
    }

    // 创建数据包结构
    struct packet_data *pkt = malloc(sizeof(struct packet_data));
    if (!pkt) {
        fprintf(stderr, "[捕获线程] 内存分配失败\n");
        return;
    }
    pkt->src_ip = strdup(inet_ntoa(ip_hdr->ip_src));
    pkt->dst_ip = strdup(inet_ntoa(ip_hdr->ip_dst));
    pkt->src_port = ntohs(udp_hdr->source);
    pkt->dst_port = ntohs(udp_hdr->dest);
    pkt->src_qp = src_qp;
    pkt->dest_qp = dest_qp;
    pkt->service_type = service_type;
    pkt->pkey = pkey;
    pkt->psn = psn;
    pkt->payload_len = payload_len;
    pkt->payload = malloc(payload_len);
    if (!pkt->payload) {
        free(pkt->src_ip);
        free(pkt->dst_ip);
        free(pkt);
        return;
    }
    memcpy(pkt->payload, payload, payload_len);
    pkt->window_size = DEFAULT_WINDOW_SIZE;

    // 添加到批量队列
    pthread_mutex_lock(&g_batch_queue.mutex);
    if (g_batch_queue.count < BATCH_MAX_PACKETS) {
        g_batch_queue.packets[g_batch_queue.count++] = pkt;
        // 队列满时唤醒处理线程
        if (g_batch_queue.count >= BATCH_MAX_PACKETS) {
            pthread_cond_signal(&g_batch_queue.cond);
            printf("[捕获线程] 批量队列已满，唤醒处理线程\n");
        }
        // 更新捕获统计
        pthread_mutex_lock(&g_perf_stats.lock);
        g_perf_stats.total_captured++;
        pthread_mutex_unlock(&g_perf_stats.lock);
    } else {
        // 队列满，丢弃包
        free(pkt->src_ip);
        free(pkt->dst_ip);
        free(pkt->payload);
        free(pkt);
        printf("[捕获线程] 批量队列已满，丢弃数据包\n");
        pthread_mutex_lock(&g_perf_stats.lock);
        g_perf_stats.total_dropped++;
        pthread_mutex_unlock(&g_perf_stats.lock);
    }
    pthread_mutex_unlock(&g_batch_queue.mutex);

    // 定期打印缓存状态
    if (psn % 500 == 0) {
        printf("[捕获线程] ===== 缓存状态快照 =====\n");
        print_all_connections_status();
    }
}

int main() {
    char errbuf[PCAP_ERRBUF_SIZE];
    struct bpf_program fp;
    char filter_exp[128];
    bpf_u_int32 net, mask;

    // 初始化批量队列
    g_batch_queue.packets = malloc(sizeof(struct packet_data*) * BATCH_MAX_PACKETS);
    g_batch_queue.count = 0;
    pthread_mutex_init(&g_batch_queue.mutex, NULL);
    pthread_cond_init(&g_batch_queue.cond, NULL);

    // 初始化性能统计
    memset(&g_perf_stats, 0, sizeof(g_perf_stats));
    gettimeofday(&g_perf_stats.start_time, NULL);
    pthread_mutex_init(&g_perf_stats.lock, NULL);
    printf("[主线程] 性能统计已初始化\n");

    // 初始化缓存管理器
    g_cache_mgr = init_cache_manager(
        1024,                // 哈希表大小
        1000,                // 最大连接数
        4096,                // 每连接环形缓存大小
        100,                 // 每连接最大字节数(MB)
        300,                 // 连接超时时间(秒)
        100,                 // 批量处理阈值
        100,                 // 批量处理超时(ms)
        DEFAULT_WINDOW_SIZE  // 滑动窗口大小
    );
    if (!g_cache_mgr) {
        fprintf(stderr, "[主线程] 缓存管理器初始化失败\n");
        return 1;
    }

    // 启动批量处理线程
    if (start_batch_processor(g_cache_mgr) != 0) {
        fprintf(stderr, "[主线程] 启动批量处理线程失败\n");
        return 1;
    }

    // 启动辅助线程
    pthread_t cleanup_tid, batch_tid, stats_tid;
    if (pthread_create(&cleanup_tid, NULL, cleanup_thread, NULL) != 0) {
        perror("[主线程] 创建清理线程失败");
        return 1;
    }

    if (pthread_create(&batch_tid, NULL, batch_cache_thread, NULL) != 0) {
        perror("[主线程] 创建批量缓存线程失败");
        return 1;
    }

    if (pthread_create(&stats_tid, NULL, stats_thread, NULL) != 0) {
        perror("[主线程] 创建统计线程失败");
        return 1;
    }

    // 打开网卡监听
    if (pcap_lookupnet("eth0", &net, &mask, errbuf) == -1) {
        fprintf(stderr, "[主线程] 无法获取网络地址: %s\n", errbuf);
        net = 0;
        mask = 0;
    }

    handle = pcap_open_live("eth0", BUFSIZ, 1, 1000, errbuf);
    if (!handle) {
        fprintf(stderr, "[主线程] 无法打开设备eth0: %s\n", errbuf);
        return 1;
    }

    // 设置过滤器（目标端口4791的UDP包）
    snprintf(filter_exp, sizeof(filter_exp), "udp dst port %d", RDMA_PORT);
    if (pcap_compile(handle, &fp, filter_exp, 0, net) == -1) {
        fprintf(stderr, "[主线程] 过滤器编译失败: %s\n", pcap_geterr(handle));
        return 1;
    }
    if (pcap_setfilter(handle, &fp) == -1) {
        fprintf(stderr, "[主线程] 设置过滤器失败: %s\n", pcap_geterr(handle));
        return 1;
    }

    printf("[主线程] 开始监听eth0上目标端口%d的UDP报文...\n", RDMA_PORT);
    printf("[主线程] 批量缓存配置：间隔%.2f秒或满%d个包触发\n", BATCH_INTERVAL, BATCH_MAX_PACKETS);
    printf("[主线程] 滑动窗口大小：%d\n", DEFAULT_WINDOW_SIZE);

    // 开始捕获报文
    pcap_loop(handle, 0, packet_handler, NULL);

    // 清理资源（正常退出时执行）
    pcap_close(handle);
    stop_batch_processor(g_cache_mgr);
    destroy_cache_manager(g_cache_mgr);
    
    for (int i = 0; i < g_batch_queue.count; i++) {
        free(g_batch_queue.packets[i]->src_ip);
        free(g_batch_queue.packets[i]->dst_ip);
        free(g_batch_queue.packets[i]->payload);
        free(g_batch_queue.packets[i]);
    }
    free(g_batch_queue.packets);
    
    pthread_mutex_destroy(&g_batch_queue.mutex);
    pthread_cond_destroy(&g_batch_queue.cond);
    pthread_mutex_destroy(&g_perf_stats.lock);
    
    printf("[主线程] 程序正常退出\n");
    return 0;
}
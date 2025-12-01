/*
编译命令：
gcc pkt_cache_val2_1201.c ../251201/pkt_cache.c -o pkt_cache_val2_1201 -lpthread -lpcap

运行命令：
sudo ./pkt_cache_val2_1201

加入gdb调试后的命令：
编译命令：
gcc -g pkt_cache_val2_1201.c ../251201/pkt_cache.c -o pkt_cache_val2_1201 -lpthread -lpcap

运行命令：
sudo gdb ./pkt_cache_val2_1201



gcc pkt_cache_val2_1201.c -o pkt_cache_val2_1201 -lpcap
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pcap.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>

#include "../251201/pkt_cache.c"

#define RDMA_PORT 4791
#define ETH_HDR_LEN 14
#define MAX_PACKETS 10000
#define RETRANSMIT_TEST_INTERVAL 500

// 全局变量
pcap_t *handle;
int packet_count = 0;
pthread_mutex_t retransmit_lock = PTHREAD_MUTEX_INITIALIZER;
uint32_t last_test_psn = 0;
char last_src_ip[INET_ADDRSTRLEN] = {0};
char last_dst_ip[INET_ADDRSTRLEN] = {0};
uint16_t last_src_port = 0;
uint16_t last_dst_port = 0;
uint32_t last_src_qp = 0;
uint32_t last_dest_qp = 0;

// 修改重传测试线程，添加更多调试信息
void *retransmit_test_thread(void *arg) {
    srand(time(NULL));
    int test_count = 0;
    
    while (1) {
        sleep(2);
        test_count++;
        
        pthread_mutex_lock(&retransmit_lock);
        if (last_test_psn > 20 && strlen(last_src_ip) > 0) {
            printf("\n===== 重传测试 #%d =====\n", test_count);
            
            // 测试多个不同的PSN
            for (int offset = 5; offset <= 20; offset += 5) {
                uint32_t test_psn = last_test_psn - offset;
                if (test_psn < 1) continue;
                
                printf("测试查找 PSN=%u... ", test_psn);
                
                struct cached_packet *pkt = find_packet_by_psn(
                    last_src_ip, last_dst_ip,
                    last_src_port, last_dst_port,
                    last_src_qp, last_dest_qp,
                    test_psn
                );
                
                if (pkt) {
                    printf("✅ 成功找到 (长度=%d)\n", pkt->data_len);
                    free(pkt);
                } else {
                    printf("❌ 未找到\n");
                }
            }
            
            // 打印当前连接状态
            struct connection_key test_key = create_connection_key(
                last_src_ip, last_dst_ip,
                last_src_port, last_dst_port,
                last_src_qp, last_dest_qp
            );
            
            printf("当前连接状态: ");
            char src_ip_str[INET_ADDRSTRLEN], dst_ip_str[INET_ADDRSTRLEN];
            struct in_addr src_in_addr, dst_in_addr;
            src_in_addr.s_addr = test_key.src_ip;
            dst_in_addr.s_addr = test_key.dst_ip;
            inet_ntop(AF_INET, &src_in_addr, src_ip_str, sizeof(src_ip_str));
            inet_ntop(AF_INET, &dst_in_addr, dst_ip_str, sizeof(dst_ip_str));
            
            printf("%s:%u -> %s:%u, 最新PSN=%u\n",
                   src_ip_str, test_key.src_port,
                   dst_ip_str, test_key.dst_port,
                   last_test_psn);
                   
            printf("==============================\n");
        } else {
            printf("等待更多数据包进行重传测试(当前PSN=%u)...\n", last_test_psn);
        }
        pthread_mutex_unlock(&retransmit_lock);
    }
    return NULL;
}

// 数据包处理回调
void packet_handler(u_char *user, const struct pcap_pkthdr *hdr, const u_char *packet) {
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
    
    if (ntohs(udp_hdr->dest) != RDMA_PORT) return;

    // 提取负载
    int udp_total_len = ntohs(udp_hdr->len);
    int payload_len = udp_total_len - 8;
    const unsigned char *payload = (u_char*)udp_hdr + 8;

    // 从IB payload提取真实PSN（模拟真实场景）
    static uint32_t psn_counter = 1;
    uint32_t psn = psn_counter++;
    // 模拟QP值（可根据实际IB协议解析）
    uint32_t src_qp = (ntohs(udp_hdr->source) << 8) | 0x01;
    uint32_t dest_qp = (ntohs(udp_hdr->dest) << 8) | 0x02;

    // 保存重传测试用的信息
    pthread_mutex_lock(&retransmit_lock);
    strncpy(last_src_ip, inet_ntoa(ip_hdr->ip_src), INET_ADDRSTRLEN-1);
    strncpy(last_dst_ip, inet_ntoa(ip_hdr->ip_dst), INET_ADDRSTRLEN-1);
    last_src_port = ntohs(udp_hdr->source);
    last_dst_port = ntohs(udp_hdr->dest);
    last_src_qp = src_qp;
    last_dest_qp = dest_qp;
    last_test_psn = psn;
    pthread_mutex_unlock(&retransmit_lock);

    // 调用批量缓存函数
    int ret = add_to_batch_queue(
        inet_ntoa(ip_hdr->ip_src),
        inet_ntoa(ip_hdr->ip_dst),
        ntohs(udp_hdr->source),
        ntohs(udp_hdr->dest),
        src_qp, dest_qp,
        psn, payload, payload_len
    );

    // 打印结果（每50个包打印一次，减少日志刷屏）
    if (psn % 50 == 0) {
        if (ret == 0) {
            printf("✅ 成功缓存数据包 #%d: %s:%d -> %s:%d PSN=%u 长度=%d\n",
                   packet_count,
                   inet_ntoa(ip_hdr->ip_src), ntohs(udp_hdr->source),
                   inet_ntoa(ip_hdr->ip_dst), ntohs(udp_hdr->dest),
                   psn, payload_len);
        } else {
            printf("❌ 缓存失败: PSN=%u\n", psn);
        }
    }

    // 每500个包打印连接状态
    if (packet_count % 500 == 0) {
        print_all_connections_status();
    }
}

int main() {
    char errbuf[PCAP_ERRBUF_SIZE];
    struct bpf_program fp;
    char filter_exp[100];
    pthread_t retransmit_thread;

    printf("=== RDMA缓存验证程序启动 ===\n");

    // 初始化缓存管理器
    g_cache_mgr = init_cache_manager(8);
    if (!g_cache_mgr) {
        fprintf(stderr, "缓存管理器初始化失败\n");
        return 1;
    }

    // 启动重传测试线程
    if (pthread_create(&retransmit_thread, NULL, retransmit_test_thread, NULL) != 0) {
        fprintf(stderr, "创建重传测试线程失败\n");
        destroy_cache_manager(g_cache_mgr);
        return 1;
    }

    // 打开网卡（设置为非阻塞模式）
    handle = pcap_open_live("eth0", BUFSIZ, 1, 100, errbuf);
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
    pthread_cancel(retransmit_thread);
    pthread_join(retransmit_thread, NULL);
    
    printf("\n程序正常结束，共处理 %d 个数据包\n", packet_count);
    return 0;
}
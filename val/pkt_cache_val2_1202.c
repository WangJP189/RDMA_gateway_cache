/*
编译命令：
gcc pkt_cache_val2_1202.c ../251202/pkt_cache.c -o pkt_cache_val2_1202 -lpthread -lpcap

运行命令：
sudo ./pkt_cache_val2_1202

加入gdb调试后的命令：
编译命令：
gcc -g pkt_cache_val2_1202.c ../251202/pkt_cache.c -o pkt_cache_val2_1202 -lpthread -lpcap

运行命令：
sudo gdb ./pkt_cache_val2_1202



gcc pkt_cache_val2_1202.c -o pkt_cache_val2_1202 -lpcap
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

#include "../251202/pkt_cache.h"

#define RDMA_PORT 4791
#define ETH_HDR_LEN 14
#define MAX_PACKETS 10000  // 最大处理包数
#define RETRANSMIT_TEST_INTERVAL 2  // 重传测试间隔(秒)

// 全局变量
pcap_t *handle;
int packet_count = 0;
pthread_mutex_t retransmit_lock = PTHREAD_MUTEX_INITIALIZER;
uint32_t last_test_psn = 0;
char last_src_ip[INET_ADDRSTRLEN] = {0};
char last_dst_ip[INET_ADDRSTRLEN] = {0};
uint32_t last_src_qp = 0;
uint32_t last_dest_qp = 0;

// 重传测试线程（验证专利快速查找）
void *retransmit_test_thread(void *arg) {
    srand(time(NULL));
    int test_count = 0;
    
    while (1) {
        sleep(RETRANSMIT_TEST_INTERVAL);
        test_count++;
        
        pthread_mutex_lock(&retransmit_lock);
        if (last_test_psn > 20 && strlen(last_src_ip) > 0) {
            printf("\n===== 专利架构 - 重传测试 #%d =====\n", test_count);
            
            // 测试多个PSN（验证多级哈希查找）
            for (int offset = 5; offset <= 20; offset += 5) {
                uint32_t test_psn = last_test_psn - offset;
                if (test_psn < 1) continue;
                
                printf("查找 PSN=%u... ", test_psn);
                struct cached_packet *pkt = find_packet_by_psn(
                    last_src_ip, last_dst_ip,
                    last_src_qp, last_dest_qp,
                    test_psn
                );
                
                if (pkt) {
                    printf("✅ 成功（长度=%d）\n", pkt->data_len);
                    free(pkt);
                } else {
                    printf("❌ 未找到\n");
                }
            }
            
            printf("当前连接：%s QP%u -> %s QP%u，最新PSN=%u\n",
                   last_src_ip, last_src_qp,
                   last_dst_ip, last_dest_qp,
                   last_test_psn);
            printf("==============================\n");
        } else {
            printf("专利架构：等待数据包（当前PSN=%u）...\n", last_test_psn);
        }
        pthread_mutex_unlock(&retransmit_lock);
    }
    return NULL;
}

// 数据包处理回调（监听eth0:4791，验证专利缓存流程）
void packet_handler(u_char *user, const struct pcap_pkthdr *hdr, const u_char *packet) {
    if (packet_count++ >= MAX_PACKETS) {
        pcap_breakloop(handle);
        return;
    }
    
    // 解析以太网头部
    const struct ip *ip_hdr = (struct ip*)(packet + ETH_HDR_LEN);
    if (ip_hdr->ip_v != 4) return;

    // 解析UDP头部（专利：筛选RDMA端口4791）
    int ip_header_len = ip_hdr->ip_hl * 4;
    const struct udphdr *udp_hdr = (struct udphdr*)((u_char*)ip_hdr + ip_header_len);
    if (ntohs(udp_hdr->dest) != RDMA_PORT) return;

    // 提取负载（专利：RDMA报文payload）
    int udp_total_len = ntohs(udp_hdr->len);
    int payload_len = udp_total_len - 8;
    const unsigned char *payload = (u_char*)udp_hdr + 8;
    if (payload_len <= 0) return;

    // 专利：从IB payload提取真实PSN和QP（模拟）
    static uint32_t psn_counter = 1;
    uint32_t psn = psn_counter++;
    // 模拟QP值（专利：QP是连接核心标识）
    uint32_t src_qp = (ntohs(udp_hdr->source) << 16) | (ip_hdr->ip_src.s_addr & 0xFFFF);
    uint32_t dest_qp = (ntohs(udp_hdr->dest) << 16) | (ip_hdr->ip_dst.s_addr & 0xFFFF);

    // 保存重传测试信息
    pthread_mutex_lock(&retransmit_lock);
    strncpy(last_src_ip, inet_ntoa(ip_hdr->ip_src), INET_ADDRSTRLEN-1);
    strncpy(last_dst_ip, inet_ntoa(ip_hdr->ip_dst), INET_ADDRSTRLEN-1);
    last_src_qp = src_qp;
    last_dest_qp = dest_qp;
    last_test_psn = psn;
    pthread_mutex_unlock(&retransmit_lock);

    // 调用专利缓存接口
    int ret = add_to_batch_queue(
        inet_ntoa(ip_hdr->ip_src),
        inet_ntoa(ip_hdr->ip_dst),
        src_qp, dest_qp,
        psn, payload, payload_len
    );

    // 打印结果（每50个包输出一次）
    if (psn % 50 == 0) {
        if (ret == 0) {
            printf("✅ 专利缓存成功：%s QP%u -> %s QP%u，PSN=%u，长度=%d\n",
                   inet_ntoa(ip_hdr->ip_src), src_qp,
                   inet_ntoa(ip_hdr->ip_dst), dest_qp,
                   psn, payload_len);
        } else {
            printf("❌ 专利缓存失败：PSN=%u\n", psn);
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

    printf("=== 专利CN120034507A - RDMA缓存验证程序 ===\n");

    // 初始化缓存管理器（专利控制平面）
    g_cache_mgr = init_cache_manager(8);  // 哈希表大小=8
    if (!g_cache_mgr) {
        fprintf(stderr, "专利架构：缓存管理器初始化失败\n");
        return 1;
    }

    // 启动重传测试线程
    if (pthread_create(&retransmit_thread, NULL, retransmit_test_thread, NULL) != 0) {
        fprintf(stderr, "专利架构：创建重传测试线程失败\n");
        destroy_cache_manager(g_cache_mgr);
        return 1;
    }

    // 打开eth0网卡（监听专利指定端口）
    handle = pcap_open_live("eth0", BUFSIZ, 1, 100, errbuf);
    if (!handle) {
        fprintf(stderr, "专利架构：无法打开网卡eth0: %s\n", errbuf);
        destroy_cache_manager(g_cache_mgr);
        return 1;
    }

    // 设置过滤器（仅UDP 4791端口）
    snprintf(filter_exp, sizeof(filter_exp), "udp dst port %d", RDMA_PORT);
    if (pcap_compile(handle, &fp, filter_exp, 0, PCAP_NETMASK_UNKNOWN) == -1) {
        fprintf(stderr, "专利架构：过滤器编译失败: %s\n", pcap_geterr(handle));
        pcap_close(handle);
        destroy_cache_manager(g_cache_mgr);
        return 1;
    }
    
    if (pcap_setfilter(handle, &fp) == -1) {
        fprintf(stderr, "专利架构：设置过滤器失败: %s\n", pcap_geterr(handle));
        pcap_close(handle);
        destroy_cache_manager(g_cache_mgr);
        return 1;
    }

    printf("专利架构：开始监听eth0网卡，目标端口: %d\n", RDMA_PORT);
    printf("最大处理包数: %d，按Ctrl+C停止\n\n", MAX_PACKETS);

    // 开始捕获数据包
    pcap_loop(handle, 0, packet_handler, NULL);

    // 清理资源
    pcap_close(handle);
    destroy_cache_manager(g_cache_mgr);
    pthread_cancel(retransmit_thread);
    pthread_join(retransmit_thread, NULL);
    
    printf("\n专利架构：程序正常结束，共处理 %d 个数据包\n", packet_count);
    return 0;
}
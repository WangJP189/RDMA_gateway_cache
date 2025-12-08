/*
编译命令：
gcc pkt_cache_val2_1208.c ../251208/pkt_cache.c -o pkt_cache_val2_1208 -lpthread -lpcap

运行命令：
sudo ./pkt_cache_val2_1208

调试编译：
gcc -g pkt_cache_val2_1208.c ../251208/pkt_cache.c -o pkt_cache_val2_1208 -lpthread -lpcap
sudo gdb ./pkt_cache_val2_1208

gcc pkt_cache_val2_1208.c -o pkt_cache_val2_1208 -lpcap
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
#include <signal.h>

// 引入正确的缓存头文件（路径根据实际调整）
#include "../251208/pkt_cache.c"

#define RDMA_PORT 4791          // RDMA默认端口
#define ETH_HDR_LEN 14          // 以太网头部长度
#define MAX_PACKETS 10000       // 最大处理包数
#define SNAP_LEN 65535          // 抓包最大长度

// 全局变量
pcap_t *pcap_handle = NULL;
int packet_count = 0;
pthread_mutex_t global_lock = PTHREAD_MUTEX_INITIALIZER;
uint32_t last_psn = 0;          // 最后缓存的PSN
char last_src_ip[INET_ADDRSTRLEN] = {0};
char last_dst_ip[INET_ADDRSTRLEN] = {0};
uint16_t last_src_port = 0;
uint16_t last_dst_port = 0;
uint32_t last_src_qp = 0;
uint32_t last_dest_qp = 0;

// 重传测试线程（预留接口，暂时模拟）
void *retransmit_test_thread(void *arg) {
    printf("重传测试线程启动（预留接口）\n");
    
    while (1) {
        sleep(3);  // 每3秒模拟一次重传查询
        
        pthread_mutex_lock(&global_lock);
        if (last_psn > 0 && strlen(last_src_ip) > 0) {
            // 模拟查询最近的PSN（预留真实查找逻辑）
            uint32_t test_psn = last_psn - (rand() % 10);
            if (test_psn < 1) test_psn = 1;
            
            printf("\n===== 重传测试 [模拟] =====\n");
            printf("查询PSN: %u\n", test_psn);
            printf("连接: %s:%u -> %s:%u (QP%u->QP%u)\n",
                   last_src_ip, last_src_port,
                   last_dst_ip, last_dst_port,
                   last_src_qp, last_dest_qp);
            printf("提示: 真实查找逻辑待实现\n");
            printf("===========================\n");
        }
        pthread_mutex_unlock(&global_lock);
    }
    return NULL;
}

// 解析数据包并提取RDMA相关信息
static int parse_rdma_packet(const u_char *packet, int pkt_len, 
                           struct connection_key *conn_key, 
                           uint32_t *psn, unsigned char *payload, int *payload_len) {
    // 解析IP头部
    const struct ip *ip_hdr = (struct ip*)(packet + ETH_HDR_LEN);
    if (ip_hdr->ip_v != 4) {
        return -1;  // 仅处理IPv4
    }
    
    // 解析UDP头部
    int ip_hl = ip_hdr->ip_hl * 4;
    const struct udphdr *udp_hdr = (struct udphdr*)((u_char*)ip_hdr + ip_hl);
    
    // 仅处理目标端口4791的UDP包
    if (ntohs(udp_hdr->dest) != RDMA_PORT) {
        return -2;
    }
    
    // 计算负载长度和指针
    int udp_len = ntohs(udp_hdr->len);
    *payload_len = udp_len - sizeof(struct udphdr);
    if (*payload_len <= 0 || *payload_len > MEM_BLOCK_SIZE) {
        return -3;
    }
    memcpy(payload, (u_char*)udp_hdr + sizeof(struct udphdr), *payload_len);
    
    // 填充连接五元组（+QP）
    conn_key->src_ip = ip_hdr->ip_src.s_addr;
    conn_key->dst_ip = ip_hdr->ip_dst.s_addr;
    conn_key->src_port = ntohs(udp_hdr->source);
    conn_key->dst_port = ntohs(udp_hdr->dest);
    
    // 模拟QP值（实际场景需从RDMA载荷解析）
    conn_key->src_qp = (conn_key->src_port << 8) | 0x01;
    conn_key->dest_qp = (conn_key->dst_port << 8) | 0x02;
    
    // 生成递增PSN（实际场景从RDMA载荷提取）
    static uint32_t psn_counter = 1;
    *psn = psn_counter++;
    
    return 0;
}

// 数据包处理回调函数
void packet_handler(u_char *user, const struct pcap_pkthdr *hdr, const u_char *packet) {
    if (packet_count >= MAX_PACKETS) {
        pcap_breakloop(pcap_handle);
        return;
    }
    
    struct connection_key conn_key;
    uint32_t psn;
    unsigned char payload[MEM_BLOCK_SIZE];
    int payload_len;
    
    // 解析数据包
    int ret = parse_rdma_packet(packet, hdr->len, &conn_key, &psn, payload, &payload_len);
    if (ret != 0) {
        return;  // 非目标数据包，跳过
    }
    
    // 转换IP为字符串（用于日志）
    char src_ip_str[INET_ADDRSTRLEN];
    char dst_ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &conn_key.src_ip, src_ip_str, INET_ADDRSTRLEN);
    inet_ntop(AF_INET, &conn_key.dst_ip, dst_ip_str, INET_ADDRSTRLEN);
    
    // 添加到缓存系统
    int cache_ret = add_packet_to_cache(
        src_ip_str, dst_ip_str,
        conn_key.src_port, conn_key.dst_port,
        conn_key.src_qp, conn_key.dest_qp,
        psn, payload, payload_len
    );
    
    // 更新全局状态（用于重传测试）
    pthread_mutex_lock(&global_lock);
    packet_count++;
    last_psn = psn;
    strncpy(last_src_ip, src_ip_str, INET_ADDRSTRLEN-1);
    strncpy(last_dst_ip, dst_ip_str, INET_ADDRSTRLEN-1);
    last_src_port = conn_key.src_port;
    last_dst_port = conn_key.dst_port;
    last_src_qp = conn_key.src_qp;
    last_dest_qp = conn_key.dest_qp;
    pthread_mutex_unlock(&global_lock);
    
    // 每50个包打印一次日志
    if (packet_count % 50 == 0) {
        if (cache_ret == 0) {
            printf("[%d] 成功缓存: %s:%u -> %s:%u | PSN=%u | 长度=%d\n",
                   packet_count, src_ip_str, conn_key.src_port,
                   dst_ip_str, conn_key.dst_port, psn, payload_len);
        } else {
            fprintf(stderr, "[%d] 缓存失败: PSN=%u | 错误码=%d\n",
                    packet_count, psn, cache_ret);
        }
    }
    
    // 每500个包打印缓存状态
    if (packet_count % 500 == 0) {
        print_all_connections_status();
    }
}

// 信号处理函数（优雅退出）
void sigint_handler(int sig) {
    printf("\n收到退出信号，正在清理资源...\n");
    
    if (pcap_handle) {
        pcap_breakloop(pcap_handle);
    }
    
    // 销毁缓存管理器
    if (g_cache_mgr) {
        destroy_cache_manager(g_cache_mgr);
    }
    
    exit(0);
}

int main() {
    char errbuf[PCAP_ERRBUF_SIZE];
    struct bpf_program filter_prog;
    char filter_exp[128];
    pthread_t test_thread;
    
    // 初始化缓存管理器
    g_cache_mgr = init_cache_manager(16);
    if (!g_cache_mgr) {
        fprintf(stderr, "缓存管理器初始化失败\n");
        return 1;
    }
    
    // 注册信号处理
    signal(SIGINT, sigint_handler);
    
    // 启动重传测试线程
    if (pthread_create(&test_thread, NULL, retransmit_test_thread, NULL) != 0) {
        fprintf(stderr, "创建重传测试线程失败\n");
        destroy_cache_manager(g_cache_mgr);
        return 1;
    }
    pthread_detach(test_thread);  // 分离线程，无需join
    
    // 打开网卡（eth0）
    pcap_handle = pcap_open_live("eth0", SNAP_LEN, 1, 100, errbuf);
    if (!pcap_handle) {
        fprintf(stderr, "打开网卡失败: %s\n", errbuf);
        destroy_cache_manager(g_cache_mgr);
        return 1;
    }
    
    // 设置过滤器：仅捕获UDP目标端口4791的包
    snprintf(filter_exp, sizeof(filter_exp), "udp dst port %d", RDMA_PORT);
    if (pcap_compile(pcap_handle, &filter_prog, filter_exp, 0, PCAP_NETMASK_UNKNOWN) == -1) {
        fprintf(stderr, "编译过滤器失败: %s\n", pcap_geterr(pcap_handle));
        pcap_close(pcap_handle);
        destroy_cache_manager(g_cache_mgr);
        return 1;
    }
    
    if (pcap_setfilter(pcap_handle, &filter_prog) == -1) {
        fprintf(stderr, "设置过滤器失败: %s\n", pcap_geterr(pcap_handle));
        pcap_close(pcap_handle);
        destroy_cache_manager(g_cache_mgr);
        return 1;
    }
    
    // 打印启动信息
    printf("=== RDMA缓存验证程序 ===\n");
    printf("监听网卡: eth0\n");
    printf("过滤规则: UDP dst port %d\n", RDMA_PORT);
    printf("最大处理包数: %d\n", MAX_PACKETS);
    printf("按 Ctrl+C 退出程序\n\n");
    
    // 开始捕获数据包
    pcap_loop(pcap_handle, 0, packet_handler, NULL);
    
    // 清理资源
    printf("\n捕获结束，共处理 %d 个数据包\n", packet_count);
    pcap_close(pcap_handle);
    destroy_cache_manager(g_cache_mgr);
    
    return 0;
}
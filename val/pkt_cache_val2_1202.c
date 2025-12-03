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
#include <stdint.h>

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

// RDMA BTH头部结构（参考IBTA规范）
struct bth_header {
    uint8_t opcode;          // 操作码（8位）
    uint8_t flags;           // 标志位
    uint16_t pkey;           // 分区密钥（网络字节序）
    uint8_t reserved[3];     // 保留字段
    uint8_t dest_qp[3];      // 目标QP（24位，网络字节序）
    uint8_t psn[3];          // PSN（24位，网络字节序）
};

// 辅助函数：解析24位无符号整数（网络字节序）
static uint32_t get_24bit_value(const uint8_t *data) {
    return (data[0] << 16) | (data[1] << 8) | data[2];
}


// 解析BTH头部
int parse_bth_header(const unsigned char *bth_start, 
                     uint8_t *opcode, uint16_t *pkey,
                     uint32_t *dest_qp, uint32_t *psn,
                     int *packet_type) {
    struct bth_header *bth = (struct bth_header*)bth_start;
    
    *opcode = bth->opcode;
    *pkey = ntohs(bth->pkey);
    *dest_qp = get_24bit_value(bth->dest_qp);
    *psn = get_24bit_value(bth->psn);
    
    // 根据opcode判断包类型
    switch (*opcode) {
        case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05:
        case 0x06: case 0x07: case 0x08: case 0x09: case 0x0A: case 0x0B:
            *packet_type = 0; // 数据包
            break;
        case 0x10: case 0x11: case 0x12: case 0x13:
            *packet_type = 1; // ACK
            break;
        case 0x14: case 0x15:
            *packet_type = 2; // NACK
            break;
        default:
            *packet_type = -1; // 未知
    }
    
    return 0;
}


// 重传测试线程（验证专利快速查找）
// 重传测试线程 - 优化版
void *retransmit_test_thread(void *arg) {
    srand(time(NULL));
    int test_count = 0;
    
    while (1) {
        sleep(RETRANSMIT_TEST_INTERVAL);
        test_count++;
        
        pthread_mutex_lock(&retransmit_lock);
        if (last_test_psn > 20 && strlen(last_src_ip) > 0) {
            printf("\n===== 专利架构 - 重传测试 #%d =====\n", test_count);
            
            // 生成要查找的PSN列表
            uint32_t test_psns[4];
            for (int i = 0; i < 4; i++) {
                test_psns[i] = last_test_psn - (5 * (i+1));
                if (test_psns[i] < 1) test_psns[i] = 1;
            }
            
            // 查找每个PSN
            for (int i = 0; i < 4; i++) {
                uint32_t target_psn = test_psns[i];
                
                printf("查找 PSN=%u... ", target_psn);
                
                // 专利O(1)查找：直接通过哈希计算位置
                struct cached_packet *pkt = find_packet_by_psn(
                    last_src_ip, last_dst_ip,
                    last_src_qp, last_dest_qp,
                    target_psn
                );
                
                if (pkt) {
                    printf("✅ 成功找到\n");
                    printf("  数据包信息: PSN=%u, 长度=%d, QP%u->QP%u\n",
                           pkt->psn, pkt->data_len, 
                           last_src_qp, last_dest_qp);
                    printf("  查找时间: O(1)直接定位\n");
                    free(pkt);
                } else {
                    printf("❌ 未找到\n");
                    
                    // 未找到的可能原因分析
                    printf("  可能原因:\n");
                    printf("  1. PSN %u 可能不在缓存窗口中\n", target_psn);
                    printf("  2. 数据包可能已被覆盖(环形缓冲区大小: %d)\n", RING_BUFFER_SIZE);
                    printf("  3. 哈希冲突导致位置被占用\n");
                }
                
                printf("  存储位置计算过程:\n");
                printf("    - 连接键: %s QP%u -> %s QP%u\n",
                       last_src_ip, last_src_qp,
                       last_dst_ip, last_dest_qp);
                printf("    - 一级哈希(流特征): IP+QP CRC32\n");
                printf("    - 二级哈希(结合PSN): 流特征+PSN异或合成\n");
                printf("    - 最终索引: 二级哈希 %% %d\n", RING_BUFFER_SIZE);
                printf("\n");
            }
            
            printf("当前连接状态:\n");
            printf("  源: %s QP%u\n", last_src_ip, last_src_qp);
            printf("  目的: %s QP%u\n", last_dst_ip, last_dest_qp);
            printf("  最新PSN: %u\n", last_test_psn);
            printf("==============================\n");
        } else {
            printf("等待足够数据包进行重传测试...\n");
            if (strlen(last_src_ip) > 0) {
                printf("当前连接: %s QP%u -> %s QP%u, 最新PSN=%u\n",
                       last_src_ip, last_src_qp,
                       last_dst_ip, last_dest_qp,
                       last_test_psn);
            }
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

    // 解析UDP头部
    int ip_header_len = ip_hdr->ip_hl * 4;
    const struct udphdr *udp_hdr = (struct udphdr*)((u_char*)ip_hdr + ip_header_len);
    if (ntohs(udp_hdr->dest) != RDMA_PORT) return;

    // 提取负载
    int udp_total_len = ntohs(udp_hdr->len);
    int payload_len = udp_total_len - 8;
    const unsigned char *payload = (u_char*)udp_hdr + 8;
    
    // 检查是否是有效的RDMA包（至少包含BTH头部）
    if (payload_len < 12) {  // BTH头部至少8字节，加上一些数据
        return;
    }
    
    // 解析BTH头部
    uint8_t opcode;
    uint16_t pkey;
    uint32_t dest_qp, psn;
    int packet_type;
    
    if (parse_bth_header(payload, &opcode, &pkey, &dest_qp, &psn, &packet_type) != 0) {
        return;
    }
    
    // 只缓存数据包，忽略ACK/NACK
    if (packet_type != 0) {
        return;
    }
    
    // 源QP号（简化处理：使用源端口作为近似）
    uint32_t src_qp = ntohs(udp_hdr->source) % 65536;
    
    // 保存重传测试信息
    pthread_mutex_lock(&retransmit_lock);
    // 安全复制IP地址
    struct in_addr src_addr = ip_hdr->ip_src;
    struct in_addr dst_addr = ip_hdr->ip_dst;
    inet_ntop(AF_INET, &src_addr, last_src_ip, INET_ADDRSTRLEN);
    inet_ntop(AF_INET, &dst_addr, last_dst_ip, INET_ADDRSTRLEN);
    last_src_qp = src_qp;
    last_dest_qp = dest_qp;
    last_test_psn = psn;
    pthread_mutex_unlock(&retransmit_lock);
    
    // 提取应用数据（跳过BTH头部）
    int bth_header_len = 12;  // 标准BTH头部长度
    if (payload_len <= bth_header_len) {
        return;
    }
    
    const unsigned char *app_data = payload + bth_header_len;
    int app_data_len = payload_len - bth_header_len;
    
    // 调用缓存接口
    int ret = add_to_batch_queue(
        last_src_ip,
        last_dst_ip,
        src_qp, dest_qp,
        psn, app_data, app_data_len
    );
    
    // 打印结果
    if (psn % 50 == 0) {
        if (ret == 0) {
            printf("✅ 专利缓存成功：%s QP%u -> %s QP%u，PSN=%u，长度=%d\n",
                   last_src_ip, src_qp,
                   last_dst_ip, dest_qp,
                   psn, app_data_len);
        } else {
            printf("❌ 专利缓存失败：PSN=%u\n", psn);
        }
    }
    
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
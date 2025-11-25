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



gcc pkt_cache_val2_1125.c -o pkt_cache_val2_1125 -lpcap
*/

// RDMA缓存验证程序 - 修复批量缓存和重传查找问题
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pcap.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/ipc.h>
#include <sys/shm.h>

#include "../251125/pkt_cache.c"  // 包含缓存模块

#define RDMA_PORT 4791
#define ETH_HDR_LEN 14
#define MAX_PACKETS 10000  // 最大处理包数
#define RETRANSMIT_TEST_INTERVAL 1000  // 重传测试间隔(包数)
#define MSG_QUEUE_KEY 0x123456  // 消息队列共享内存键值

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
struct packet_msg *msg_queue = NULL;  // 全局消息队列
int msg_queue_id = -1;

// 初始化消息队列
int init_message_queue() {
    // 创建或获取共享内存
    msg_queue_id = shmget(MSG_QUEUE_KEY, sizeof(struct packet_msg) * BATCH_THRESHOLD, 0666 | IPC_CREAT);
    if (msg_queue_id == -1) {
        perror("shmget failed for msg queue");
        return -1;
    }

    // 映射共享内存
    msg_queue = (struct packet_msg*)shmat(msg_queue_id, NULL, 0);
    if (msg_queue == (void*)-1) {
        perror("shmat failed for msg queue");
        shmctl(msg_queue_id, IPC_RMID, NULL);
        return -1;
    }

    // 初始化消息队列
    memset(msg_queue, 0, sizeof(struct packet_msg) * BATCH_THRESHOLD);
    return 0;
}

// 重传测试线程
void *retransmit_test_thread(void *arg) {
    while (1) {
        sleep(2);  // 每2秒测试一次
        
        pthread_mutex_lock(&retransmit_lock);
        if (last_test_psn > 0 && strlen(last_src_ip) > 0) {
            // 测试窗口内的PSN（确保在窗口范围内）
            uint32_t test_psn = last_test_psn - (rand() % 20);  // 缩小测试范围
            if (test_psn < 1) test_psn = 1;
            
            printf("\n===== 重传测试: 查找PSN=%u =====\n", test_psn);
            struct cached_packet *pkt = find_packet_by_psn(
                last_src_ip, last_dst_ip,
                last_src_port, last_dst_port,
                last_src_qp, last_dest_qp,
                test_psn
            );
            
            if (pkt) {
                printf("找到PSN=%u的数据包, 长度=%d\n", pkt->psn, pkt->data_len);
                free(pkt);
            } else {
                printf("未找到PSN=%u的数据包\n", test_psn);
            }
            printf("==============================\n");
        }
        pthread_mutex_unlock(&retransmit_lock);
    }
    return NULL;
}

// 批量处理线程
void *batch_processor_thread(void *arg) {
    while (1) {
        struct timeval last_process_time;
        gettimeofday(&last_process_time, NULL);
        int pending_count = 0;

        // 统计未处理的消息数
        for (int i = 0; i < BATCH_THRESHOLD; i++) {
            if (!msg_queue[i].processed && msg_queue[i].data_len > 0) {
                pending_count++;
            }
        }

        // 检查超时或达到阈值
        struct timeval now;
        gettimeofday(&now, NULL);
        long elapsed = (now.tv_sec - last_process_time.tv_sec) * 1000 +
                      (now.tv_usec - last_process_time.tv_usec) / 1000;

        if (pending_count >= BATCH_THRESHOLD || elapsed >= BATCH_TIMEOUT_MS) {
            if (pending_count > 0) {
                // 处理所有未处理的消息
                for (int i = 0; i < BATCH_THRESHOLD; i++) {
                    if (!msg_queue[i].processed && msg_queue[i].data_len > 0) {
                        // 先创建连接键变量，再取地址
                        struct connection_key conn_key = create_connection_key(
                            last_src_ip, last_dst_ip,
                            last_src_port, last_dst_port,
                            last_src_qp, last_dest_qp
                        );
                        
                        insert_packet(
                            get_or_create_cache(g_cache_mgr, 
                                &conn_key,  // 使用变量地址
                                msg_queue[i].psn),
                            msg_queue[i].psn,
                            msg_queue[i].data,
                            msg_queue[i].data_len
                        );
                        msg_queue[i].processed = 1;
                    }
                }
                pending_count = 0;
                gettimeofday(&last_process_time, NULL);
            }
        }

        usleep(1000);  // 降低CPU占用
    }
    return NULL;
}

// 数据包处理回调
void packet_handler(u_char *user, const struct pcap_pkthdr *hdr, const u_char *packet) {
    // 限制处理包数
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
    if (payload_len <= 0 || payload_len > MAX_PACKET_SIZE) return;

    // 提取RDMA信息（模拟PSN递增）
    static uint32_t psn_counter = 1;
    uint32_t src_qp = 0x1234;
    uint32_t dest_qp = 0x5678;
    uint32_t psn = psn_counter++;

    // 保存信息用于重传测试和批量处理
    pthread_mutex_lock(&retransmit_lock);
    strncpy(last_src_ip, inet_ntoa(ip_hdr->ip_src), INET_ADDRSTRLEN-1);
    strncpy(last_dst_ip, inet_ntoa(ip_hdr->ip_dst), INET_ADDRSTRLEN-1);
    last_src_port = ntohs(udp_hdr->source);
    last_dst_port = ntohs(udp_hdr->dest);
    last_src_qp = src_qp;
    last_dest_qp = dest_qp;
    last_test_psn = psn;
    pthread_mutex_unlock(&retransmit_lock);

    // 将数据包加入消息队列（批量处理）
    int added = 0;
    for (int i = 0; i < BATCH_THRESHOLD; i++) {
        if (!msg_queue[i].processed) {
            msg_queue[i].psn = psn;
            memcpy(msg_queue[i].data, payload, payload_len);
            msg_queue[i].data_len = payload_len;
            msg_queue[i].processed = 0;
            added = 1;
            break;
        }
    }

    // 打印处理结果
    if (added) {
        if (psn % 100 == 0) { // 每100个包打印一次
            printf("成功加入缓存队列 #%d: %s:%d -> %s:%d PSN=%u 长度=%d\n",
                   packet_count,
                   inet_ntoa(ip_hdr->ip_src), ntohs(udp_hdr->source),
                   inet_ntoa(ip_hdr->ip_dst), ntohs(udp_hdr->dest),
                   psn, payload_len);
        }
    } else {
        printf("缓存队列满，丢弃PSN=%u\n", psn);
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
    pthread_t retransmit_thread, batch_thread;

    printf("=== RDMA缓存验证程序启动 ===\n");

    // 初始化消息队列
    if (init_message_queue() != 0) {
        fprintf(stderr, "消息队列初始化失败\n");
        return 1;
    }

    // 初始化缓存管理器
    g_cache_mgr = init_cache_manager(8); // 哈希表大小=8
    if (!g_cache_mgr) {
        fprintf(stderr, "缓存管理器初始化失败\n");
        shmdt(msg_queue);
        shmctl(msg_queue_id, IPC_RMID, NULL);
        return 1;
    }

    // 启动重传测试线程
    if (pthread_create(&retransmit_thread, NULL, retransmit_test_thread, NULL) != 0) {
        fprintf(stderr, "创建重传测试线程失败\n");
        destroy_cache_manager(g_cache_mgr);
        shmdt(msg_queue);
        shmctl(msg_queue_id, IPC_RMID, NULL);
        return 1;
    }

    // 启动批量处理线程
    if (pthread_create(&batch_thread, NULL, batch_processor_thread, NULL) != 0) {
        fprintf(stderr, "创建批量处理线程失败\n");
        pthread_cancel(retransmit_thread);
        destroy_cache_manager(g_cache_mgr);
        shmdt(msg_queue);
        shmctl(msg_queue_id, IPC_RMID, NULL);
        return 1;
    }

    // 打开网卡
    handle = pcap_open_live("eth0", BUFSIZ, 1, 1000, errbuf);
    if (!handle) {
        fprintf(stderr, "无法打开网卡eth0: %s\n", errbuf);
        destroy_cache_manager(g_cache_mgr);
        shmdt(msg_queue);
        shmctl(msg_queue_id, IPC_RMID, NULL);
        return 1;
    }

    // 设置过滤器
    snprintf(filter_exp, sizeof(filter_exp), "udp dst port %d", RDMA_PORT);
    if (pcap_compile(handle, &fp, filter_exp, 0, PCAP_NETMASK_UNKNOWN) == -1) {
        fprintf(stderr, "过滤器编译失败: %s\n", pcap_geterr(handle));
        pcap_close(handle);
        destroy_cache_manager(g_cache_mgr);
        shmdt(msg_queue);
        shmctl(msg_queue_id, IPC_RMID, NULL);
        return 1;
    }
    
    if (pcap_setfilter(handle, &fp) == -1) {
        fprintf(stderr, "设置过滤器失败: %s\n", pcap_geterr(handle));
        pcap_close(handle);
        destroy_cache_manager(g_cache_mgr);
        shmdt(msg_queue);
        shmctl(msg_queue_id, IPC_RMID, NULL);
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
    pthread_cancel(batch_thread);
    pthread_join(retransmit_thread, NULL);
    pthread_join(batch_thread, NULL);
    shmdt(msg_queue);
    shmctl(msg_queue_id, IPC_RMID, NULL);
    
    printf("\n程序正常结束，共处理 %d 个数据包\n", packet_count);
    return 0;
}
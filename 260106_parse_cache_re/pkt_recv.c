#include "pkt_recv.h"

#include <string.h>             // memset()
#include <unistd.h>             // close()
#include <stdlib.h>             // free()

#include <net/if.h>             // if_nametoindex()-将接口名转换为索引号
#include <sys/socket.h>         // socket()-创建套接字  bind()-绑定套接字到接口 
                                // setsockopt()-设置套接字选项  recvfrom()-接收数据报 
                                // AF_PACKET,SOCK_RAW-套接字常量
#include <arpa/inet.h>          // htons(),ntohs()-字节序转换 inet_ntop()-将网络地址转换为字符串
#include <net/ethernet.h>       // struct ethhdr-以太网头部结构 ETH_P_ALL,ETH_P_IP-以太网协议类型常量
#include <linux/if_packet.h>    // struct sockaddr_ll-链路层套接字地址 
                                // struct packet_mreq-多播/混杂模式请求结构 PACKET_MR_PROMISC-混杂模式常量

#include <netinet/ip.h>         // struct iphdr-IP头部结构  IPPROTO_UDP-IP协议号常量
#include <netinet/udp.h>        // struct udphdr-UDP头部结构

#include <signal.h>             // sig_atomic_t
//#include <time.h>
#include <sys/time.h>         // timeval
#include <errno.h>

// 全局运行控制变量
static volatile sig_atomic_t g_receiver_running = 0;
static pthread_t g_receiver_thread;
static int g_packet_sockfd = -1; // 新增：记录接收线程的套接字


//全局连接老化线程控制变量
static int g_age_thread_running = 1;            // 老化线程运行标志
static pthread_t g_global_age_thread;           // 全局老化线程ID

void* packet_receiver_thread(void* arg) {

    //const char* interface = (const char*)arg;
    char* interface = (char*)arg;
    
    int sockfd = create_promiscuous_socket(interface);
    if (sockfd < 0) {
        fprintf(stderr, "原始套接字混杂模式创建失败(接口:%s)\n", interface);
        free(interface);  // 释放内存
        return NULL;
    }

    free(interface);  // 释放内存

    receive_and_parse_frames(sockfd);

    close(sockfd);

    printf("成功关闭原始套接字\n");

    return NULL;
}

int start_packet_receiver(const char* interface_name) {

    if (g_receiver_running) {
        printf("报文接收已经在运行\n");
        return -1;
    }
    
    g_receiver_running = 1;
    
    if (pthread_create(&g_receiver_thread, NULL, packet_receiver_thread, (void*)interface_name) != 0) {
        perror("创建报文接收失败");
        g_receiver_running = 0;
        return -1;
    }
    
    printf("报文接收启动成功\n");

    return 0;
}

// 停止报文接收器
int stop_packet_receiver() {

    if (!g_receiver_running) {
        printf("报文接收未运行\n");
        return -1;
    }
    
    g_receiver_running = 0;

    // 关键：关闭套接字，强制recvfrom退出阻塞
    if (g_packet_sockfd != -1) {
        close(g_packet_sockfd);
        g_packet_sockfd = -1;
    }

    pthread_join(g_receiver_thread, NULL);
    
    printf("报文接收已停止\n");

    return 0;
}

int is_receiver_running() {

    return g_receiver_running;
    
}

int create_promiscuous_socket(const char *interface_name) {

    // create socket
    int sockfd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (sockfd == -1) {
        perror("socket");
        return -1;
    }

    printf("成功在接口 %s 上创建原始套接字\n", interface_name);

    // bind interface
    struct sockaddr_ll sll;
    memset(&sll, 0, sizeof(sll));
    sll.sll_family = AF_PACKET;
    sll.sll_ifindex = if_nametoindex(interface_name);
    if (sll.sll_ifindex == 0) {
        perror("if_nametoindex");
        close(sockfd);
        return -1;
    }
    sll.sll_protocol = htons(ETH_P_ALL);
    if (bind(sockfd, (struct sockaddr *)&sll, sizeof(sll)) == -1) {
        perror("bind");
        close(sockfd);
        return -1;
    }

    // set mode PROMISC
    struct packet_mreq mr;
    memset(&mr, 0, sizeof(mr));
    mr.mr_ifindex = sll.sll_ifindex;
    mr.mr_type = PACKET_MR_PROMISC;
    if (setsockopt(sockfd, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &mr, sizeof(mr)) == -1) {
        perror("setsockopt promisc");
        close(sockfd);
        return -1;
    }

    printf("成功在接口 %s 上开启混杂模式\n", interface_name);

    g_packet_sockfd = sockfd; // 保存套接字句柄

    return sockfd;
}

void receive_and_parse_frames(int sockfd) {

    // copy 2times ToDo  AF_packet --> DPDK?
    unsigned char buffer[2048];
    struct sockaddr_ll saddr;
    socklen_t saddr_len = sizeof(saddr);

    // 设置socket超时
    struct timeval tv;
    tv.tv_sec = 1;  // 1秒超时
    tv.tv_usec = 0;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    
    //uint64_t last_cleanup_time = get_current_time_ns();
    //const uint64_t BUSY_CHECK_INTERVAL = 5ULL * 1000000000ULL; // 5秒

    //while (g_receiver_running && !g_graceful_shutdown_recv) {
    while (g_receiver_running) {

        // recvfrom
        ssize_t msg_len = recvfrom(sockfd, buffer, sizeof(buffer), 0, 
                                  (struct sockaddr *)&saddr, &saddr_len);
        
        if (msg_len < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 超时，继续检查运行状态
                // 接收超时(即网络空闲)，执行清理
                // cleanup_expired_connections();
                // 重置忙时计时器
                // last_cleanup_time = get_current_time_ns();
                continue;
            //} else if (g_receiver_running && !g_graceful_shutdown_recv) {
            } else if (g_receiver_running) {
                perror("recvfrom");
            }
            continue;
        }
        
        //printf("收到报文: %zd bytes\n", msg_len);

        // 调用完整的解析函数
        process_rdma_packet(buffer, msg_len);

        // 如果一直有包，recvfrom不会超时，需要主动检查
        // uint64_t now = get_current_time_ns();

        // if (now - last_cleanup_time > BUSY_CHECK_INTERVAL) {
            // =============== [新增打印] ===============
            // printf("[DEBUG] 触发 BusyCheck | 当前时间: %lu ns (约 %.2f 秒)\n", 
            //        now, (double)now / 1000000000.0);
            // =========================================
            
            // ToDo block not connection
            // cleanup_expired_connections();
            // last_cleanup_time = now;
        // }
    }
}

void process_rdma_packet(const unsigned char *buffer, ssize_t length) {

    //printf("process rdma pakcet.....\n");

    //  1.解析头部
    //      以太网
    struct ethhdr *eth = (struct ethhdr*)buffer;
    if (ntohs(eth->h_proto) != ETH_P_IP) return;
    //      IP
    struct iphdr *ip = (struct iphdr*)(buffer + sizeof(struct ethhdr));
    int ip_header_len = ip->ihl * 4;
    if (ip->protocol != IPPROTO_UDP) return;
    //      UDP
    struct udphdr *udp = (struct udphdr*)(buffer + sizeof(struct ethhdr) + ip_header_len);
    if (ntohs(udp->dest) != 4791) return;

    //  2.计算BTH位置
    int udp_header_len = sizeof(struct udphdr);
    int bth_offset = sizeof(struct ethhdr) + ip_header_len + udp_header_len;
    if (bth_offset + 12 > length) {
        printf("报文过短，无法解析BTH\n");
        return;
    }
    const unsigned char *bth_start = buffer + bth_offset;

    // 3. 解析BTH头部（增强版）
    uint8_t opcode;
    uint16_t pkey;
    uint32_t dest_qp;
    uint32_t psn;
    rdma_packet_type_t pkt_type;
    
    if (parse_bth_header(bth_start, &opcode, &pkey,
                         &dest_qp, &psn, &pkt_type) != 0) {
        printf("BTH解析失败\n");
        return;
    }

    // 4. 提取IP地址和端口
    char src_ip[INET_ADDRSTRLEN], dst_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(ip->saddr), src_ip, INET_ADDRSTRLEN);
    inet_ntop(AF_INET, &(ip->daddr), dst_ip, INET_ADDRSTRLEN);
    uint16_t src_port = ntohs(udp->source);
    uint16_t dst_port = ntohs(udp->dest);

    // ============================================================
    // A:查流表
    // ============================================================
    struct flow_entry *flow = lookup_flow(src_ip, dst_ip, src_port, dst_port, dest_qp, pkey);
    if (!flow) {
        // 没查到流表，说明控制面没建链，或者是非法包，直接丢弃
        printf("[WARN] 丢弃未知数据包 DstQP:%u\n", dest_qp);
        return;
    }
    // ============================================================
    // B:组装连接Key 
    // ============================================================
    struct connection_key key = create_connection_key(src_ip, dst_ip, src_port, dst_port,
                                                    flow->src_qp, dest_qp, pkey);
    // ============================================================
    // C:获取 Bucket
    // ============================================================
    struct connection_bucket *bucket = get_connection_bucket(key);

    // 5. 根据报文类型分别处理
    switch (pkt_type) {
        case PKT_TYPE_DATA:
            // process_data_packet(buffer, length, bth_offset, 
            //                    src_ip, dst_ip, src_port, dst_port,
            //                    pkey, dest_qp, psn, opcode);
            process_data_packet(buffer, length, bth_offset, bucket, key, psn);
            break;
        case PKT_TYPE_ACK:
            // process_ack_packet(buffer, length, bth_offset,
            //                   src_ip, dst_ip, src_port, dst_port,
            //                   pkey, dest_qp, psn, opcode);
            break;
        case PKT_TYPE_NACK:
            // process_nack_packet(buffer, length, bth_offset,
            //                    src_ip, dst_ip, src_port, dst_port,
            //                    pkey, dest_qp, psn, opcode);
            break;
        default:
            printf("Unknown: OPCode=0x%02x\n", opcode);
            break;
    }
}

// WTC
int parse_bth_header(const unsigned char *bth_start, 
                     uint8_t *opcode, uint16_t *pkey,
                     uint32_t *dest_qp, uint32_t *psn,
                     rdma_packet_type_t *pkt_type) {
    struct bth *bth = (struct bth*)bth_start;
    
    *opcode = bth->opcode;
    *pkey = ntohs(bth->pkey);

    printf("--------------------------------------------\n");
    printf("BTH解析中\n");

    // 提取24位的dest_qp
    const uint8_t *dest_qp_ptr = (const uint8_t*)(bth_start + 5);
    *dest_qp = get_24bit_value(dest_qp_ptr) & 0x00FFFFFF;

    // 提取24位的psn
    const uint8_t *psn_ptr = (const uint8_t*)(bth_start + 9);
    *psn = get_24bit_value(psn_ptr) & 0x00FFFFFF;
    
    // 判断报文类型
    *pkt_type = get_packet_type_rc(*opcode);
    
    printf("******** ******** ******** ******** ********\n");
    switch (*pkt_type) {
        case PKT_TYPE_DATA:
            printf("DATA/REQUEST:");
            break;
        case PKT_TYPE_ACK:
            printf("ACK:");
            break;
        case PKT_TYPE_NACK:
            printf("NACK:");
            break; 
        default:
            printf("UNKNOWN:");
            break;
    }
    printf("\n\tOPCode=\t0x%02x \t | %u\n", *opcode, *opcode);
    printf("\tPKey=\t0x%04x\t | %u\n", *pkey, *pkey);
    printf("\tDstQP=\t0x%06X | %u\n", *dest_qp, *dest_qp);
    printf("\tPSN=\t0x%06X | %u\n", *psn, *psn);
    printf("******** ******** ******** ******** ********\n");

    return 0;
}

void process_data_packet(const unsigned char *buffer, ssize_t length, int bth_offset, 
                        struct connection_bucket *bucket, struct connection_key key, uint32_t psn) {

    // 计算应用数据位置
    int bth_header_len = 12;
    int app_data_offset = bth_offset + bth_header_len;
    int udp_header_len = sizeof(struct udphdr);
    int ip_header_len = ((struct iphdr*)(buffer + sizeof(struct ethhdr)))->ihl * 4;
    int udp_payload_len = ntohs(((struct udphdr*)(buffer + sizeof(struct ethhdr) + ip_header_len))->len);
    int app_data_len = udp_payload_len - udp_header_len - bth_header_len;

    if (app_data_len > 0 && app_data_offset + app_data_len <= length) {

        const unsigned char *packet = buffer; //+ app_data_offset;
        

        pthread_rwlock_rdlock(&bucket->rwlock); // 先上读锁

        // ============================================================
        // 获取或创建缓存
        // ============================================================
        struct connection_cache_array *cache = get_or_create_connection_cache_array(bucket, key);

        if (cache) {
            // ============================================================
            // 写入数据
            // ============================================================
            
            // 添加到缓存
            add_to_connection_cache(cache, psn, packet, app_data_len);



        //         // ========== 新增：连接级老化处理 ==========
        //     // 1. 获取当前连接的缓存结构体（替换为你实际的查找函数）
        //     struct connection_cache_array *conn = get_connection_cache(bucket, key);
        //     if (!conn) return;

        //     // 2. 时间戳逻辑
        //     uint64_t cur_stamp = get_current_timestamp_ms();
        //     uint64_t five_ms_stamp = CONN_AGE_CHECK_INTERVAL_MS;

        //     // 初始化连接的老化检查时间戳（复用conn->last_age_stamp字段）
        //     if (conn->last_age_stamp == 0) {
        //         conn->last_age_stamp = cur_stamp + five_ms_stamp;
        //     }

        //     // 达到检查时间，执行老化
        //     if (cur_stamp >= conn->last_age_stamp) {
        //         age_out_expired_packets(conn, cur_stamp);
        //         conn->last_age_stamp = cur_stamp + five_ms_stamp; // 更新下次检查时间
        //     }
            
        // }

        pthread_rwlock_unlock(&bucket->rwlock); // 解锁 
    }
};
    
// uint8_t infer_service_type(uint8_t opcode) {
//     // RC (Reliable Connected) 服务类型的操作码范围
//     if ((opcode >= 0x00 && opcode <= 0x1F) || 
//         (opcode >= 0x80 && opcode <= 0x9F)) {
//         return 1;  // RC
//     }
//     // UC (Unreliable Connected) 服务类型的操作码范围  
//     else if ((opcode >= 0x20 && opcode <= 0x3F) ||
//              (opcode >= 0xA0 && opcode <= 0xBF)) {
//         return 2;  // UC
//     }
//     // UD (Unreliable Datagram) 服务类型的操作码范围
//     else if ((opcode >= 0x40 && opcode <= 0x5F) ||
//              (opcode >= 0xC0 && opcode <= 0xDF)) {
//         return 3;  // UD
//     }
//     // RAW (Raw Datagram) 等服务类型
//     else {
//         return 0;  // 未知或其它
//     }
// }

// void process_ack_packet(const unsigned char *buffer, ssize_t length,
//                        int bth_offset,
//                        const char *src_ip, const char *dst_ip,
//                        uint16_t src_port, uint16_t dst_port,
//                        uint16_t pkey, uint32_t dest_qp, uint32_t psn,
//                        uint8_t opcode) {
void process_ack_packet(const unsigned char *buffer, ssize_t length, int bth_offset, 
                        struct connection_bucket *bucket, struct connection_key key, uint32_t epsn) {
    // printf("ACK: %s:%d -> %s:%d\n", src_ip, src_port, dst_ip, dst_port);
    // printf("PKey=0x%04x, QP=%u, PSN=%u, ePSN=%u, Syndrome=0x%02x\n",
    //        pkey, dest_qp, psn, epsn, syndrome);

    // ACK报文在BTH之后有AETH
    int aeth_offset = bth_offset + 12; // BTH固定12字节
    
    if (aeth_offset + 4 > length) { // AETH固定4字节
        printf("ACK报文过短，无法解析AETH\n");
        return;
    }
    
    const unsigned char *aeth_start = buffer + aeth_offset;
    uint8_t syndrome;
    uint32_t msn;
    
    // TODO: RC类型的ACK报文一定有AETH头吗？
    // 对于RC（Reliable Connected）服务类型，ACK报文通常都包含AETH头，但存在一些特殊情况。
    if (parse_aeth_header(aeth_start, &syndrome, &msn) != 0) {
        printf("AETH解析失败\n");
        return;
    }

    // [新增]
    pthread_rwlock_rdlock(&bucket->rwlock); // 先上读锁

    // AETH Syndrome 的高 3 位决定了是 ACK, RNR 还是 NAK
    // Mask: 1110 0000 (0xE0)
    uint8_t type_bits = (syndrome >> 5) & 0x07;

    switch (type_bits) {
        case 0x00: // 000xxxxx -> ACK
            // handle_ack_received(src_ip, dst_ip, dest_qp, psn);
            break;
        case 0x01: // 001xxxxx -> RNR (Receiver Not Ready)
            // handle_nack_received(src_ip, dst_ip, dest_qp, psn);
            break;
        case 0x03: // 011xxxxx -> NAK (Sequence Error, etc.)
            // handle_nack_received(src_ip, dst_ip, dest_qp, psn);
            break;
        default:
            break;
    }
    // 处理ACK逻辑：确认数据包接收，可以清理缓存

    // [新增]
    pthread_rwlock_unlock(&bucket->rwlock); // 解锁 
};

// void process_nack_packet(const unsigned char *buffer, ssize_t length,
//                         int bth_offset,
//                         const char *src_ip, const char *dst_ip,
//                         uint16_t src_port, uint16_t dst_port,
//                         uint16_t pkey, uint32_t dest_qp, uint32_t psn,
//                         uint8_t opcode) {
void process_nack_packet(const unsigned char *buffer, ssize_t length, int bth_offset, 
                        struct connection_bucket *bucket, struct connection_key key, uint32_t epsn) {    
    // printf("NACK: %s:%d -> %s:%d\n", src_ip, src_port, dst_ip, dst_port);
    // printf("PKey=0x%04x, QP=%u, PSN=%u, ePSN=%u, Syndrome=0x%02x\n",
    //        pkey, dest_qp, psn, epsn, syndrome);
                            
    // NACK报文在BTH之后有AETH
    int aeth_offset = bth_offset + 12;
    
    if (aeth_offset + 4 > length) {
        printf("NACK报文过短，无法解析AETH\n");
        return;
    }
    
    const unsigned char *aeth_start = buffer + aeth_offset;
    uint8_t syndrome;
    uint32_t msn;
    
    if (parse_aeth_header(aeth_start, &syndrome, &msn) != 0) {
        printf("AETH解析失败\n");
        return;
    }
    
    // [新增]
    pthread_rwlock_rdlock(&bucket->rwlock); // 先上读锁

    // 处理NACK逻辑：触发重传
    // handle_nack_received(src_ip, dst_ip, dest_qp, psn);

    // [新增]
    pthread_rwlock_unlock(&bucket->rwlock); // 解锁 
};

// WTC
int parse_aeth_header(const unsigned char *aeth_start,
                     uint8_t *syndrome, uint32_t *msn) {

    struct aeth *aeth = (struct aeth*)aeth_start;
    
    *syndrome = aeth->syndrome;
    *msn = get_24bit_value(aeth->MSN) & 0x00FFFFFF;

    return 0;
}

void handle_ack_received(struct connection_bucket *bucket, struct connection_key key, uint32_t epsn) {

    // ToDo

}

void handle_nack_received(struct connection_bucket *bucket, struct connection_key key, uint32_t epsn) {

    // ToDo

}










//======================全局连接老化线程相关=======================

/**
 * @brief 全局老化线程：遍历哈希桶，清理超期空闲的连接
 */
static void *global_conn_age_thread(void *arg) {
    (void)arg;
    while (g_age_thread_running) { // 全局运行标志，需外部定义
        uint64_t current_ts = get_current_timestamp_ms(); // 当前时间戳(毫秒)

        // 遍历所有哈希桶（最小化锁范围）
        for (int bucket_idx = 0; bucket_idx < CONN_BUCKET_COUNT; bucket_idx++) {
            struct connection_bucket *cur_bucket = &g_conn_buckets[bucket_idx];
            if (!cur_bucket) continue;

            // ========== 哈希桶级写锁（后续替换为王滕超的锁逻辑） ==========
            // pthread_rwlock_wrlock(&cur_bucket->rwlock); 

            // 遍历当前桶的连接链表（节点类型为connection_entry）
            struct connection_entry *prev_entry = NULL;
            struct connection_entry *cur_entry = cur_bucket->head;
            
            while (cur_entry) {
                // 跳过无效连接（提前标记为无效的可直接清理）
                if (cur_entry->valid != 1) {
                    struct connection_entry *next_entry = cur_entry->next;
                    // 从链表移除无效节点
                    if (prev_entry == NULL) {
                        cur_bucket->head = next_entry;
                    } else {
                        prev_entry->next = next_entry;
                    }
                    // 释放资源
                    if (cur_entry->cache_array) {
                        free_connection_array(cur_entry->cache_array);
                        cur_entry->cache_array = NULL;
                    }
                    free(cur_entry); // 释放连接条目本身
                    cur_entry = next_entry;
                    printf("[GLOBAL_AGE] 清理无效连接，桶索引=%d\n", bucket_idx);
                    continue;
                }

                // 判断连接是否空闲超期：最后活动时间 + 阈值 ≤ 当前时间
                int is_expired = 0;
                if (cur_entry->cache_array && cur_entry->cache_array->last_active_stamp != 0) {
                    uint64_t idle_duration_ms = current_ts - cur_entry->last_active_stamp;
                    if (idle_duration_ms >= CONN_IDLE_EXPIRE_THRESHOLD_MS) {
                        is_expired = 1;
                    }
                }

                if (is_expired) {
                    // 1. 从链表中移除当前连接条目
                    struct connection_entry *next_entry = cur_entry->next;
                    if (prev_entry == NULL) {
                        // 头节点
                        cur_bucket->head = next_entry;
                    } else {
                        prev_entry->next = next_entry;
                    }

                    // 2. 清理当前连接的所有资源
                    if (cur_entry->cache_array) {
                        free_connection_array(cur_entry->cache_array);
                        cur_entry->cache_array = NULL;
                    }
                    free(cur_entry); // 释放连接条目结构体
                    
                    // 3. 输出日志
                    printf("[GLOBAL_AGE] 清理超期空闲连接，桶索引=%d\n", bucket_idx);
                    
                    // 4. 移动到下一个节点
                    cur_entry = next_entry;
                } else {
                    // 未过期，继续遍历
                    prev_entry = cur_entry;
                    cur_entry = cur_entry->next;
                }
            }

            // ========== 解锁哈希桶 ==========
            // pthread_rwlock_unlock(&cur_bucket->rwlock);

            usleep(100); // 桶间休眠，降低CPU占用（微秒级）
        }

        sleep(AGE_THREAD_SLEEP_SEC); // 全局线程休眠（秒级）
    }
    return NULL;
}

/**
 * @brief 启动全局老化线程
 */
int init_global_conn_age_thread(void) {
    int ret = pthread_create(&g_global_age_thread, NULL, global_conn_age_thread, NULL);
    if (ret != 0) {
        printf("[ERROR] 全局老化线程创建失败，错误码：%d\n", ret);
        return -1;
    }
    return 0;
}

/**
 * @brief 停止全局老化线程
 */
void stop_global_conn_age_thread(void) {
    g_age_thread_running = 0;
    pthread_join(g_global_age_thread, NULL);
}




#ifdef STANDALONE_TEST

void receive_and_parse_frames(int sockfd) {

    unsigned char buffer[2048];
    struct sockaddr_ll saddr;
    socklen_t saddr_len = sizeof(saddr);

    while(1) {

        // recvfrom
        ssize_t msg_len = recvfrom(sockfd, buffer, sizeof(buffer), 0, 
                                  (struct sockaddr *)&saddr, &saddr_len);

        if (msg_len < 0) {
            perror("recvfrom");
            continue;
        }
        if (msg_len < (ssize_t)sizeof(struct ethhdr)) {
            printf("收到过短报文: %zd bytes\n", msg_len);
            continue;
        }
        
        //printf("收到报文: %zd bytes\n", msg_len);

        // 调用完整的解析函数
        process_rdma_packet(buffer, msg_len);

    }
}

int start_packet_receiver(const char* interface_name) {

    int sockfd = create_promiscuous_socket(interface_name);

    if (sockfd == -1) {
        fprintf(stderr, "套接字创建失败\n");
        return 1;
    }

    receive_and_parse_frames(sockfd);

    close(sockfd);

    return 0;
}

int main(){

    start_packet_receiver("ens37");

    return 0;
}

#endif
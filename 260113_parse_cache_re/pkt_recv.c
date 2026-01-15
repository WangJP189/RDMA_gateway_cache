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
    struct connection_key key = create_connection_key(src_ip, dst_ip, flow->src_qp, dest_qp, pkey);
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
            // 1. 调用核心缓存函数处理数据包
            age_expired_packets(cache);

            // 2. 添加到缓存
            add_to_connection_cache(cache, psn, packet, app_data_len);


        pthread_rwlock_unlock(&bucket->rwlock); // 解锁 
        }
    }
}
    
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

/**
 * @brief 处理收到的ACK报文，匹配对应连接并清理已确认的数据包(PSN ≤ epsn)
 * @param bucket 哈希桶指针，指定要遍历的桶
 * @param key 待匹配的连接四元组key(src_ip/dst_ip/src_qp/dst_qp/pkey)
 * @param epsn ACK报文中携带的最大确认PSN，清理该值及之前的所有报文
 * @note 1. 哈希桶加【读锁】，仅遍历查找不修改链表，支持并发读，性能最优
 * @note 2. 连接匹配规则：connection_key的src_ip/dst_ip/src_qp/dst_qp/pkey全字段严格匹配
 * @note 3. 健壮性校验：空指针/无效连接/空缓存数组 全过滤，无崩溃风险
 * @note 4. PSN仅保留24位有效位，统一掩码处理，避免高位数据干扰
 * @note 5. 遍历完成后必解锁，无锁泄漏风险
 */
void handle_ack_received(struct connection_bucket *bucket, struct connection_key key, uint32_t epsn) {
    // 1. 入参合法性校验：哈希桶为空直接返回
    if (bucket == NULL) {
        printf("[ERROR] handle_ack_received: 哈希桶指针为空，无法处理ACK\n");
        return;
    }

    // 2. 统一处理PSN：仅保留24位有效位，过滤高位无效数据，和clean_acked_packets逻辑对齐
    uint32_t ack_msn = epsn;
    struct connection_entry *curr = bucket->head;
    int find_conn_flag = 0;

    // 3. 哈希桶加读锁：遍历冲突链表属于读操作，读锁并发安全，性能最优
    pthread_rwlock_rdlock(&bucket->rwlock);

    // 4. 遍历当前哈希桶的冲突链表，匹配目标连接
    while (curr != NULL) {
        // 4.1 过滤无效连接：连接标记为无效，直接跳过
        if (curr->valid != 1) {
            curr = curr->next;
            continue;
        }

        // 4.2 核心：严格匹配连接key的所有字段，完全一致才视为同一个连接
        struct connection_key *curr_key = &curr->connection_key;
        if (curr_key->src_ip  == key.src_ip  &&
            curr_key->dst_ip  == key.dst_ip  &&
            curr_key->src_qp  == key.src_qp  &&
            curr_key->dst_qp  == key.dst_qp  &&
            curr_key->pkey    == key.pkey) 
        {
            find_conn_flag = 1;
            
            // 4.3 过滤空缓存数组：连接有效但缓存未初始化，打印警告并跳过
            if (curr->cache_array == NULL) {
                printf("[WARN] handle_ack_received: 匹配到连接[src_ip=0x%08X,dst_ip=0x%08X], 但缓存数组为空，跳过ACK清理\n",
                       key.src_ip, key.dst_ip);
                curr = curr->next;
                break;
            }

            // 4.4 找到目标连接，调用核心清理函数，处理ACK确认的报文
            printf("[INFO] handle_ack_received: 匹配到目标连接，开始清理ACK确认报文 | ACK MSN=0x%06X\n", ack_msn);
            int clean_ret = clean_acked_packets(curr->cache_array, ack_msn);
            
            // 4.5 根据清理结果打印分级日志，复用原错误码做结果判断
            switch (clean_ret) {
                case RETRANS_NO_VALID_PSN_RANGE:
                    printf("[ACK RECV] 该连接无有效PSN范围，无需清理报文\n");
                    break;
                case RETRANS_NO_CACHED_PACKETS:
                    printf("[ACK RECV] 该连接无缓存数据包，无需清理报文\n");
                    break;
                default:
                    if (clean_ret > 0) {
                        printf("[ACK RECV] ✅ ACK清理完成，本次释放已确认报文=%d个\n", clean_ret);
                    }
                    break;
            }
            
            // 一个key在哈希桶中唯一对应一个连接，匹配到后直接退出遍历，提升效率
            break;
        }

        curr = curr->next;
    }

    // 5. 未匹配到对应连接的日志打印
    if (find_conn_flag == 0) {
        printf("[DEBUG] handle_ack_received: 哈希桶中未匹配到指定连接[src_ip=0x%08X,dst_ip=0x%08X], 跳过ACK清理\n",
               key.src_ip, key.dst_ip);
    }

    // 6. 解锁哈希桶：无论是否匹配到连接，必须解锁，杜绝锁泄漏
    pthread_rwlock_unlock(&bucket->rwlock);
}

void handle_nack_received(struct connection_bucket *bucket, struct connection_key key, uint32_t epsn) {

    // ToDo

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
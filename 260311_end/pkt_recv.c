// ZPY
#define _GNU_SOURCE // ensure struct ifreq is available even under strict POSIX
                    // macros
// ZPY

#include "pkt_recv.h"
#include "simulate.h"

#include <stdlib.h> // free()
#include <string.h> // memset()
#include <unistd.h> // close()

#include <net/if.h>     // if_nametoindex()-将接口名转换为索引号
#include <sys/socket.h> // socket()-创建套接字  bind()-绑定套接字到接口
                        // setsockopt()-设置套接字选项  recvfrom()-接收数据报
                        // AF_PACKET,SOCK_RAW-套接字常量
#include <arpa/inet.h> // htons(),ntohs()-字节序转换 inet_ntop()-将网络地址转换为字符串
#include <linux/if_packet.h> // struct sockaddr_ll-链路层套接字地址
#include <net/ethernet.h> // struct ethhdr-以太网头部结构 ETH_P_ALL,ETH_P_IP-以太网协议类型常量
// struct packet_mreq-多播/混杂模式请求结构 PACKET_MR_PROMISC-混杂模式常量

#include <netinet/ip.h>  // struct iphdr-IP头部结构  IPPROTO_UDP-IP协议号常量
#include <netinet/udp.h> // struct udphdr-UDP头部结构

#include <errno.h>
#include <signal.h>   // sig_atomic_t
#include <sys/time.h> // timeval

// ZPY
#include <fcntl.h>     // fcntl(), O_NONBLOCK
#include <sys/ioctl.h> // ioctl, SIOCGIFHWADDR
// ZPY

// 全局运行控制变量
extern pthread_t g_receiver_thread;
static int g_packet_sockfd = -1; // 新增：记录接收线程的套接字

extern pthread_t g_retransmit_thread; // 全局重传线程ID
extern int g_retransmit_sockfd;       // 全局AF_PACKET传输的socket_fd
extern int g_tcp_server_sockfd;       // 全局服务端TCP监听socket_fd
extern int g_sr_client_sockfd;        // 全局客户端socket_fd
extern char g_sr_target_ip[32];       // 全局SR服务端目标IP
extern int g_sr_target_port;          // 全局SR服务端目标端口
extern char
    g_dst_gbn_interface_name[IF_NAMESIZE]; // 全局目的网关GBN重传接口名称
extern char
    g_src_nack_interface_name[IF_NAMESIZE]; // 全局源网关发送NACK接口名称
extern char
    g_src_sr_interface_name[IF_NAMESIZE]; // 全局源网关发送SR数据接口名称
// ZPY

void *rx_thread_proc(void *arg) {
    char *interface = (char *)arg;

    int sockfd = create_promiscuous_socket(interface);
    if (sockfd < 0) {
        fprintf(stderr, "原始套接字混杂模式创建失败(接口:%s)\n", interface);
        free(interface); // 释放内存
        return NULL;
    }

    free(interface); // 释放内存
    receive_and_parse_frames(sockfd);
    printf("RX线程退出\n");
    return NULL;
}

int start_rx_thread(const char *interface_name) {
    if (g_receiver_thread > 0) {
        printf("报文接收已经在运行\n");
        return -1;
    }

    if (pthread_create(&g_receiver_thread, NULL, rx_thread_proc,
                       (void *)interface_name) != 0) {
        perror("创建报文接收失败");
        g_receiver_thread = 0;
        return -1;
    }

    printf("报文接收启动成功\n");
    return 0;
}

// 停止报文接收器
void stop_rx_thread(void) {
    if (g_receiver_thread > 0) {
        pthread_join(g_receiver_thread, NULL);
        printf("[RX_THREAD] 收包线程已退出并回收\n");
        g_receiver_thread = 0;
    }
}

void cleanup_rx_resources(void) {
    // 关键：关闭套接字，强制recvfrom退出阻塞
    if (g_packet_sockfd > 0) {
        close(g_packet_sockfd);
        g_packet_sockfd = -1;
    }

    printf("[RX_THREAD] 资源释放\n");
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
    if (setsockopt(sockfd, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &mr,
                   sizeof(mr)) == -1) {
        perror("setsockopt promisc");
        close(sockfd);
        return -1;
    }

    printf("成功在接口 %s 上开启混杂模式\n", interface_name);
    g_packet_sockfd = sockfd; // 保存套接字句柄
    return sockfd;
}

void receive_and_parse_frames(int sockfd) {
    size_t buffer_size = 5192;

    unsigned char *buffer = (unsigned char *)malloc(buffer_size);
    if (!buffer) {
        perror("Buffer memory allocation failed");
        return;
    }

    struct sockaddr_ll saddr;
    socklen_t saddr_len = sizeof(saddr);

    // 设置socket超时
    struct timeval tv;
    tv.tv_sec = 1; // 1秒超时
    tv.tv_usec = 0;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    while (g_running) {
        ssize_t msg_len = recvfrom(sockfd, buffer, buffer_size, 0,
                                   (struct sockaddr *)&saddr, &saddr_len);
        if (msg_len < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            } else {
                perror("recvfrom");
            }
            continue;
        }

        // 新增：报文接收成功，统计接收报文
        if (g_perf_monitoring) {
            record_packet(&g_perf_stats,
                          msg_len); // msg_len是实际接收的报文长度
        }

        process_rdma_packet(buffer, msg_len);
    }
    free(buffer);
}

void process_rdma_packet(const unsigned char *buffer, ssize_t length) {
    // 1.解析头部
    // 以太网
    struct ethhdr *eth = (struct ethhdr *)buffer;
    if (ntohs(eth->h_proto) != ETH_P_IP)
        return;
    // IP
    struct iphdr *ip = (struct iphdr *)(buffer + sizeof(struct ethhdr));
    int ip_header_len = ip->ihl * 4;
    if (ip->protocol != IPPROTO_UDP)
        return;
    // UDP
    struct udphdr *udp =
        (struct udphdr *)(buffer + sizeof(struct ethhdr) + ip_header_len);
    if (ntohs(udp->dest) != 4791)
        return;

    // 2.计算BTH位置
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

    if (parse_bth_header(bth_start, &opcode, &pkey, &dest_qp, &psn,
                         &pkt_type) != 0) {
        printf("BTH解析失败\n");
        return;
    }

    // 4. 提取IP地址和端口
    char src_ip[INET_ADDRSTRLEN], dst_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(ip->saddr), src_ip, INET_ADDRSTRLEN);
    inet_ntop(AF_INET, &(ip->daddr), dst_ip, INET_ADDRSTRLEN);

    struct flow_entry *flow = lookup_flow(src_ip, dst_ip, dest_qp, pkey);
    if (!flow) {
        // 没查到流表，说明控制面没建链，或者是非法包，直接丢弃
        printf("[WARN] 丢弃未知数据包 DstQP:%u\n", dest_qp);
        return;
    }
    // ZPY
    struct connection_key key =
        create_connection_key(src_ip, dst_ip, flow->src_qp, dest_qp, pkey);
    struct connection_bucket *bucket = get_connection_bucket(key);
    // 判断bucket是否存在
    if (bucket == NULL) {
        printf(
            "[WARN] 跳过报文处理，哈希桶不存在[src_ip=0x%08X,dst_ip=0x%08X]\n",
            key.src_ip, key.dst_ip);
        return;
    }
    // ZPY
    switch (pkt_type) {
    case PKT_TYPE_DATA:
        process_data_packet(buffer, length, bth_offset, bucket, key, psn);
        break;
    case PKT_TYPE_ACK:
        // ZPY
        process_control_packet(buffer, length, bth_offset, bucket, key, psn,
                               flow->role);
        break;
    default:
        printf("Unknown: OPCode=0x%02x\n", opcode);
        break;
    }
}

// WTC
int parse_bth_header(const unsigned char *bth_start, uint8_t *opcode,
                     uint16_t *pkey, uint32_t *dest_qp, uint32_t *psn,
                     rdma_packet_type_t *pkt_type) {
    struct bth *bth = (struct bth *)bth_start;

    *opcode = bth->opcode;
    *pkey = ntohs(bth->pkey);

    // printf("--------------------------------------------\n");
    // printf("BTH解析中\n");

    // 提取24位的dest_qp
    const uint8_t *dest_qp_ptr = (const uint8_t *)(bth_start + 5);
    *dest_qp = get_24bit_value(dest_qp_ptr) & 0x00FFFFFF;

    // 提取24位的psn
    const uint8_t *psn_ptr = (const uint8_t *)(bth_start + 9);
    *psn = get_24bit_value(psn_ptr) & 0x00FFFFFF;

    // 判断报文类型
    *pkt_type = get_packet_type_rc(*opcode);

    // printf("******** ******** ******** ******** ********\n");
    // switch (*pkt_type) {
    // case PKT_TYPE_DATA:
    //     printf("DATA/REQUEST:");
    //     break;
    // case PKT_TYPE_ACK:
    //     printf("ACK:");
    //     break;
    // case PKT_TYPE_NACK:
    //     printf("NACK:");
    //     break;
    // default:
    //     printf("UNKNOWN:");
    //     break;
    // }
    // printf("\n\tOPCode=\t0x%02x \t | %u\n", *opcode, *opcode);
    // printf("\tPKey=\t0x%04x\t | %u\n", *pkey, *pkey);
    // printf("\tDstQP=\t0x%06X | %u\n", *dest_qp, *dest_qp);
    // printf("\tPSN=\t0x%06X | %u\n", *psn, *psn);
    // printf("******** ******** ******** ******** ********\n");

    return 0;
}

void process_data_packet(const unsigned char *buffer, ssize_t length,
                         int bth_offset, struct connection_bucket *bucket,
                         struct connection_key key, uint32_t psn) {
    // 计算应用数据位置
    int bth_header_len = 12;
    int app_data_offset = bth_offset + bth_header_len;
    int udp_header_len = sizeof(struct udphdr);
    int ip_header_len =
        ((struct iphdr *)(buffer + sizeof(struct ethhdr)))->ihl * 4;
    int udp_payload_len = ntohs(
        ((struct udphdr *)(buffer + sizeof(struct ethhdr) + ip_header_len))
            ->len);
    int app_data_len = udp_payload_len - udp_header_len - bth_header_len;

    if (app_data_len > 0 && app_data_offset + app_data_len <= length) {
        const unsigned char *packet = buffer; //+ app_data_offset;

        pthread_rwlock_rdlock(&bucket->rwlock); // 先上读锁
        struct connection_cache_array *cache =
            get_connection_cache_array(bucket, key);
        if (cache) {
            age_expired_packets(cache);
            add_to_connection_cache(cache, psn, packet, length);
        }
        pthread_rwlock_unlock(&bucket->rwlock); // 解锁
    }
}

void process_control_packet(const unsigned char *buffer, ssize_t length,
                            int bth_offset, struct connection_bucket *bucket,
                            struct connection_key key, uint32_t epsn,
                            enum gateway_role role) {
    // ACK报文在BTH之后有AETH
    int aeth_offset = bth_offset + 12; // BTH固定12字节

    if (aeth_offset + 4 > length) { // AETH固定4字节
        printf("ACK报文过短，无法解析AETH\n");
        return;
    }

    const unsigned char *aeth_start = buffer + aeth_offset;
    uint8_t syndrome;
    uint32_t msn;
    if (parse_aeth_header(aeth_start, &syndrome, &msn) != 0) {
        printf("AETH解析失败\n");
        return;
    }

    // AETH Syndrome 的高 3 位决定了是 ACK, RNR 还是 NAK
    // Mask: 1110 0000 (0xE0)
    uint8_t type_bits = (syndrome >> 5) & 0x07;

    switch (type_bits) {
    case 0x00: // 000xxxxx -> ACK
        // ZPY
        handle_ack_received(buffer, length, bucket, key, epsn, role);
        break;
    case 0x01: // 001xxxxx -> RNR (Receiver Not Ready)
        // ZPY
        handle_nack_received(buffer, length, bucket, key, epsn, role);
        break;
    case 0x03: // 011xxxxx -> NAK (Sequence Error, etc.)
        // ZPY
        handle_nack_received(buffer, length, bucket, key, epsn, role);
        break;
    default:
        break;
    }
};

// WTC
int parse_aeth_header(const unsigned char *aeth_start, uint8_t *syndrome,
                      uint32_t *msn) {

    struct aeth *aeth = (struct aeth *)aeth_start;

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
 * @note 2.
 * 连接匹配规则：connection_key的src_ip/dst_ip/src_qp/dst_qp/pkey全字段严格匹配
 * @note 3. 健壮性校验：空指针/无效连接/空缓存数组 全过滤，无崩溃风险
 * @note 4. PSN仅保留24位有效位，统一掩码处理，避免高位数据干扰
 * @note 5. 遍历完成后必解锁，无锁泄漏风险
 */
void handle_ack_received(const unsigned char *buffer, ssize_t length,
                         struct connection_bucket *bucket,
                         struct connection_key key, uint32_t epsn,
                         enum gateway_role role) {
    // 1. 入参合法性校验
    // ZPY
    // 组装连接反向Key
    struct connection_key reverse_key = create_connection_key_u32(
        key.dst_ip, key.src_ip, key.dst_qp, key.src_qp, key.pkey);
    // 获取反向Bucket
    struct connection_bucket *reverse_bucket =
        get_connection_bucket(reverse_key);
    if (reverse_bucket == NULL) {
        printf(
            "[ERROR] handle_ack_received: 反向哈希桶指针为空，无法处理ACK\n");
        return;
    }

    // 统一处理PSN
    uint32_t ack_msn = epsn;
    struct connection_cache_array *target_cache = NULL;

    // 2. 哈希桶加读锁：遍历/查找连接属于读操作，读锁保证并发安全
    pthread_rwlock_rdlock(&reverse_bucket->rwlock);

    // 3. 复用封装函数查找连接缓存
    target_cache = get_connection_cache(reverse_bucket->head, reverse_key);
    // 4. 处理连接查找结果
    if (target_cache != NULL) {
        // 过滤空缓存数组：连接有效但缓存未初始化
        if (target_cache->ring_buf == NULL) {
            printf("[WARN] handle_ack_received: "
                   "匹配到连接[src_ip=0x%08X,dst_ip=0x%08X], "
                   "但缓存环形数组未初始化，跳过ACK清理\n",
                   key.src_ip, key.dst_ip);
            pthread_rwlock_unlock(
                &reverse_bucket->rwlock); // 提前解锁，避免泄漏
            return;
        }

        // 调用核心清理函数，处理ACK确认的报文
        // printf("[INFO] handle_ack_received: "
        //        "匹配到目标连接，开始清理ACK确认报文 | ACK MSN=0x%06X\n",
        //        ack_msn);
        int clean_ret = clean_acked_packets(target_cache, ack_msn);

        // 根据清理结果打印分级日志（复用原错误码）
        switch (clean_ret) {
        case RETRANS_NO_VALID_PSN_RANGE:
            // printf("[ACK RECV] 该连接无有效PSN范围，无需清理报文\n");
            break;
        case RETRANS_NO_CACHED_PACKETS:
            printf("[ACK RECV] 该连接无缓存数据包，无需清理报文\n");
            break;
        default:
            if (clean_ret > 0) {
                // printf("[ACK RECV] ✅ ACK清理完成，本次释放已确认报文=%d个\n",
                //        clean_ret);
            } else if (clean_ret < 0) {
                printf("[ERROR] handle_ack_received: ACK清理失败，错误码=%d\n",
                       clean_ret);
            }
            break;
        }
    } else {
        // 未匹配到对应连接的日志打印
        printf("[DEBUG] handle_ack_received: "
               "哈希桶中未匹配到指定连接[src_ip=0x%08X,dst_ip=0x%08X], "
               "跳过ACK清理\n",
               key.src_ip, key.dst_ip);
    }
    // 5. 解锁哈希桶：无论是否匹配到连接，必须解锁（杜绝锁泄漏）
    pthread_rwlock_unlock(&reverse_bucket->rwlock);
}

// ZPY
uint32_t calculate_psn_number(uint32_t start_psn, uint32_t end_psn) {
    // 确保PSN在有效范围内
    start_psn &= PSN_MASK;
    end_psn &= PSN_MASK;

    // 计算需要遍历的PSN数量
    uint32_t count;
    if (end_psn >= start_psn) {
        count = end_psn - start_psn + 1;
    } else {
        // 溢出情况：从start到最大值，再从0到end
        count = (PSN_MASK - start_psn + 1) + (end_psn + 1);
    }
    return count;
}

// ZPY
// 判断丢包函数
int is_packet_lost(struct connection_cache_array *conn, uint32_t psn) {
    uint32_t psn_masked = psn & PSN_MASK;
    int ring_index = psn_masked % RING_BUFFER_SIZE;
    // 判断标准：缓存数组中对应位置为空或者当前数缓存报文的psn和真实psn不相等，则视为丢包
    if (conn->ring_buf[ring_index] == 0) {
        // 新增：统计丢包
        if (g_perf_monitoring) {
            record_drop_packet(&g_perf_stats);
        }
        return 1; // 丢包
    } else {
        // 存在缓存数据，需要判断PSN是否匹配
        // 验证内存块数据
        unsigned char *mem_block = (unsigned char *)conn->ring_buf[ring_index];
        struct mem_block_header *header = (struct mem_block_header *)mem_block;

        // 检查头部PSN是否匹配
        if ((header->psn & PSN_MASK) != psn_masked) {
            // PSN不匹配，视为丢包
            // 新增：统计丢包
            if (g_perf_monitoring) {
                record_drop_packet(&g_perf_stats);
            }
            // 释放对应内存块,并将缓存位置设为NULL
            free(mem_block);
            conn->ring_buf[ring_index] = 0;
            return 1;
        }
        return 0; // 未丢包
    }
}

// ZPY
//  计算丢包位图
lost_segment *calculate_bitmap_loss(struct connection_cache_array *conn,
                                    uint32_t *seg_num) {
    uint32_t seg_count = 0;
    uint32_t start_psn = conn->start_psn;
    uint32_t end_psn = conn->end_psn;

    if (start_psn == PSN_INVALID || end_psn == PSN_INVALID) {
        printf("[NACK CLEAN] 无有效数据包，无需清理\n");
        return NULL;
    }
    // 溢出PSN处理
    start_psn &= PSN_MASK;
    end_psn &= PSN_MASK;
    // 计算需要遍历的PSN数量
    uint32_t count = calculate_psn_number(start_psn, end_psn);

    // 修改，不用计算seg_num，直接固定分配最大可能的丢包段数量
    lost_segment *lost_segs =
        (lost_segment *)malloc(MAX_LOST_SEGMENTS * sizeof(lost_segment));
    seg_count = 0;
    // 遍历所有PSN
    for (uint32_t i = 0; i < count; i++) {
        uint32_t current_psn = (start_psn + i) & PSN_MASK;
        // uint32_t ring_index = current_psn % RING_BUFFER_SIZE;
        if (is_packet_lost(conn, current_psn)) {
            if (seg_count >= MAX_LOST_SEGMENTS) {
                // 避免越界写导致堆损坏
                printf("[WARN] 超过最大丢包段数量限制，停止计算\n");
                break;
            }
            lost_segs[seg_count].start_psn = current_psn;
            uint32_t num = 1;
            int32_t j = 0;
            // 计算连续的NULL数量
            for (j = i + 1; j < count; j++) {
                if (is_packet_lost(conn, (start_psn + j) & PSN_MASK)) {
                    num++;
                } else {
                    break;
                }
            }
            i = j; // 更新i以跳过已计算的段
            lost_segs[seg_count].loss_num = num;
            seg_count++;
        }
    }
    *seg_num = seg_count;
    return lost_segs;
}

// ZPY
// 获取网关的MAC地址
int get_gateway_mac(const char *ifname, unsigned char *mac_addr) {
    int sfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sfd < 0) {
        perror("socket (ioctl)");
        return -1;
    }
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IF_NAMESIZE - 1);
    if (ioctl(sfd, SIOCGIFHWADDR, &ifr) == 0) {
        memcpy(mac_addr, (unsigned char *)ifr.ifr_hwaddr.sa_data, ETH_ALEN);
    } else { // 调用失败
        perror("ioctl SIOCGIFHWADDR");
        close(sfd);
        return -1;
    }
    close(sfd);
    return 1;
}

// ZPY
// 收到NACK后清理已被确认的报文（PSN < nack_psn），释放对应内存块
int clean_nacked_packets(struct connection_cache_array *conn,
                         uint32_t nack_psn) {
    if (!conn) {
        printf("[ERROR] 清理已确认报文失败：连接缓存为空\n");
        return RETRANS_NO_CACHED_PACKETS;
    }

    // 空缓存检查：无有效数据包，直接返回
    if (conn->start_psn == PSN_INVALID || conn->end_psn == PSN_INVALID) {
        printf("[NACK CLEAN] 无有效数据包，无需清理\n");
        return RETRANS_NO_VALID_PSN_RANGE;
    }

    // 仅保留nack_psn的24位有效部分，避免高位干扰，统一做掩码处理
    uint32_t temp_end =
        (nack_psn - 1) & PSN_MASK; // NACK确认的最后PSN是nack_psn-1
    uint32_t current_start = conn->start_psn;
    uint32_t current_end = conn->end_psn;
    int cleaned_count = 0;
    // bug修复
    if (!psn_in_ring_range(temp_end, current_start, current_end)) {
        // nack_psn不在当前缓存的PSN范围内
        // 若为非回绕区间且nack_psn > end_psn，仅清理到end_psn为止
        if (psn_greater_than(temp_end, current_end)) {
            temp_end = current_end;
            printf("[NACK CLEAN] NACK PSN=%u 超出当前缓存PSN范围[%u~%u]，"
                   "仅清理到end_psn=%u\n",
                   (nack_psn - 1) & PSN_MASK, current_start, current_end,
                   temp_end);
        } else {
            printf("[NACK CLEAN] NACK PSN=%u 不在当前缓存PSN范围内[%u~%u]，"
                   "无需清理\n",
                   temp_end, current_start, current_end);
            return RETRANS_NO_VALID_PSN_RANGE;
        }
    }
    // bug修复
    printf("[NACK CLEAN] 开始清理已确认报文：NACK PSN=%u | 原始PSN范围=[%u~%u] "
           "| 清理区间=[%u~%u]\n",
           temp_end, current_start, current_end, current_start, temp_end);

    // 批量清理指定PSN区间的数据包
    cleaned_count = batch_clean_psn_range(conn, current_start, temp_end);

    // 计算新的起始PSN，处理回绕+仅保留24位有效位
    uint32_t new_start = (temp_end + 1) & PSN_MASK;
    printf("[NACK CLEAN] 清理完成：释放已确认报文=%d个 | 新start_psn=%u\n",
           cleaned_count, new_start);

    // 正常nak流程epsn不会等于end_psn，因此直接更新start_psn就可以
    // conn->start_psn = new_start;
    // 补充：受限于AF_PACKET性能问题，导致缓存报文缺失，可能出现nack_psn>end_psn的情况
    // 判断是否所有包都被清理（环形语境下new_start大于current_end代表无剩余包）
    if (psn_greater_than(new_start, current_end)) {
        // 无剩余有效包，重置PSN核心参数
        conn->start_psn = PSN_INVALID;
        conn->end_psn = PSN_INVALID;
        conn->cur_psn = 0;
        printf("[NACK CLEAN] 所有报文已被清理，重置PSN参数\n");
    } else {
        // 有剩余有效包，仅更新start_psn，end_psn保持不变
        conn->start_psn = new_start;
    }

    return cleaned_count;
}

// ZPY
// WTC sr_control.c中函数
void set_sr_client_target_info(const char *ip, int port) {
    if (ip) {
        strncpy(g_sr_target_ip, ip, sizeof(g_sr_target_ip) - 1);
    }
    g_sr_target_port = port;
    printf("[SR-Config] 对端地址已更新: %s:%d\n", g_sr_target_ip,
           g_sr_target_port);
}

// ZPY
// 目的发送SR重传请求给源网关
// 源网关IP和端口获取全局变量
// 假设保存在全局的一个 g_sr_client_sockfd中，这里只负责发送重传请求
// 封装一个获取sr客户端fd的函数，g_sr_client_sockfd为全局变量
int get_sr_client_sockfd(void) {
    if (g_sr_client_sockfd == -1) {
        int fd;
        struct sockaddr_in server_addr;
        // 创建socket
        if ((fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
            perror("socket创建失败");
            return -1;
        }
        // 连接服务器
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(g_sr_target_port);
        if (inet_pton(AF_INET, g_sr_target_ip, &server_addr.sin_addr) <= 0) {
            perror("地址转换失败");
            close(fd);
            return -1;
        }

        // 根据函数执行人工set的sr服务端IP和端口进行connect
        if (connect(fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) <
            0) {
            perror("连接服务器失败");
            close(fd);
            return -1;
        } else {
            g_sr_client_sockfd = fd;
            return g_sr_client_sockfd;
        }
    } else {
        return g_sr_client_sockfd;
    }
}

// SR重传请求发送失败时调用下面接口释放fd
void close_sr_client_fd(void) {
    if (g_sr_client_sockfd != -1) {
        close(g_sr_client_sockfd);
        g_sr_client_sockfd = -1;
    }
    return;
}

// ZPY
// 关闭AF_PACKET重传socket
void close_retransmit_sockfd(void) {
    if (g_retransmit_sockfd != -1) {
        close(g_retransmit_sockfd);
        g_retransmit_sockfd = -1;
    }
    return;
}

// ZPY
int get_retransmit_sockfd(void) {
    if (g_retransmit_sockfd == -1) {
        int sockfd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
        if (sockfd < 0) {
            perror("socket AF_PACKET 创建失败");
            return -1;
        }
        g_retransmit_sockfd = sockfd;
        return g_retransmit_sockfd;
    } else {
        return g_retransmit_sockfd;
    }
}

// ZPY
// 通过AF_PACKET重传RDMA数据包
void retransmit_rdma_packet(const unsigned char *packet_data, int length,
                            const char *interface_name, uint32_t psn,
                            uint32_t dest_qp) {
    // TODO: 实现RDMA数据包的重传逻辑
    // packet_data是完整的报文，无论是数据报文，还是ACK/NACK报文，包含前面的Ethernet/IP/UDP等header
    // 数据报文需要在调用该函数前剥离缓存报文的前置结构[mem_block_header][RDMA数据包数据]
    // NACK报文直接使用packet_data发送即可
    // 假设data是完整的以太网帧，且目的MAC已经是正确的，只需修改源MAC为网关的MAC
    // 1) 获取中间设备的源MAC
    char ifname[IF_NAMESIZE];
    unsigned char src_mac[ETH_ALEN];
    memcpy(ifname, interface_name, IF_NAMESIZE);
    if (get_gateway_mac(ifname, src_mac) == 1) {
        printf("获取网关MAC成功\n");
    } else {
        printf("获取网关MAC失败\n");
        return;
    }

    // 2) 复制帧并修改源MAC（假设为正确的MAC地址，目的MAC保持不变）
    unsigned char *frame = malloc(length);
    if (!frame) {
        perror("malloc frame");
        return;
    }
    memcpy(frame, packet_data, length);

    // 修改以太网头的源MAC
    struct ethhdr *eth = (struct ethhdr *)frame;
    memcpy(eth->h_source, src_mac, ETH_ALEN);
    // 目的MAC保持不变

    // 3) AF_PACKET发送，socket改为全局创建g_retransmit_sockfd
    int psock = get_retransmit_sockfd();
    if (psock < 0) {
        perror("socket AF_PACKET");
        free(frame);
        return;
    }

    struct sockaddr_ll sll;
    memset(&sll, 0, sizeof(sll));
    sll.sll_family = AF_PACKET;
    sll.sll_ifindex = if_nametoindex(ifname);
    sll.sll_halen = ETH_ALEN;
    // 使用帧中的目的MAC
    memcpy(sll.sll_addr, eth->h_dest, ETH_ALEN);

    ssize_t sent =
        sendto(psock, frame, length, 0, (struct sockaddr *)&sll, sizeof(sll));
    if (sent < 0) {
        fprintf(stderr, "sendto failed: %s\n", strerror(errno));
        close_retransmit_sockfd(); // 关闭socket，重置全局变量
    } else {
        // 新增：重传成功，统计重传报文
        if (g_perf_monitoring) {
            record_retransmit_packet(&g_perf_stats);
        }
        printf("通过 %s 发送 %zd 字节 (PSN=%u, QP=%u)\n", ifname, sent, psn,
               dest_qp);
    }
    // psock不关闭，保持全局socket复用
    free(frame);
}

void set_gateway_msg(struct connection_key key,
                     struct connection_cache_array *conn,
                     lost_segment *ls_sgments,
                     gateway_control_msg *gw_control_msg,
                     gateway_data_msg *gw_data_msg, uint32_t seg_num,
                     size_t msg_size) {
    uint32_t msg_seg_num = seg_num;
    // 填充控制信息结构体数据
    gw_control_msg->message_type = 1; // 消息类型1表示SR请求
    // 这里就将源目的数据调换顺序，接收到消息后可直接使用
    gw_control_msg->src_ip = htonl(key.dst_ip);
    gw_control_msg->dest_ip = htonl(key.src_ip);
    gw_control_msg->src_qp = htonl(key.dst_qp);
    gw_control_msg->dest_qp = htonl(key.src_qp);
    gw_control_msg->pkey = htons(key.pkey);
    // 填充数据信息结构体数据
    gw_data_msg->total_data_length = htonl(msg_size); // 总长度
    gw_data_msg->seg_num = htonl(msg_seg_num);        // 丢包段数量

    // 填充丢包段
    for (uint32_t i = 0; i < msg_seg_num; i++) {
        gw_data_msg->lost_segments[i].start_psn =
            htonl(ls_sgments[i].start_psn);
        gw_data_msg->lost_segments[i].loss_num = htonl(ls_sgments[i].loss_num);
    }
    return;
}

// ZPY
// 发送SR重传请求给源网关
void send_sr_request_to_src_gateway(struct connection_key key,
                                    struct connection_cache_array *conn) {
    int sockfd = get_sr_client_sockfd();
    if (sockfd == -1) {
        perror("获取SR客户端socket失败，无法发送SR重传请求\n");
        return;
    }

    // 准备要发送的gateway_data_msg结构体
    uint32_t seg_num = 0;
    // 填充后回收该指针内存
    lost_segment *ls_sgments = calculate_bitmap_loss(conn, &seg_num);
    if (ls_sgments == NULL || seg_num == 0) {
        printf("无需发送SR重传请求\n");
        if (ls_sgments) {
            free(ls_sgments);
        }
        return;
    }
    size_t msg_size = sizeof(gateway_data_msg) + seg_num * sizeof(lost_segment);
    gateway_control_msg *gw_control_msg =
        (gateway_control_msg *)malloc(sizeof(gateway_control_msg));
    gateway_data_msg *gw_data_msg = (gateway_data_msg *)malloc(msg_size);
    if (!gw_control_msg || !gw_data_msg) {
        perror("内存分配失败");
        return;
    }
    // 填充控制信息结构体数据
    set_gateway_msg(key, conn, ls_sgments, gw_control_msg, gw_data_msg, seg_num,
                    msg_size);

    // 准备iovec，需要2个元素指向整个消息
    struct iovec iov[2];
    iov[0].iov_base = gw_control_msg;
    iov[0].iov_len = sizeof(gateway_control_msg);
    iov[1].iov_base = gw_data_msg;
    iov[1].iov_len = msg_size;

    // 准备msghdr结构
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = iov;   // 设置IO向量
    msg.msg_iovlen = 2;  // 2个缓冲区
    msg.msg_name = NULL; // 不需要指定目标地址（已连接）
    msg.msg_namelen = 0;

    // 发送数据
    printf("正在发送gateway_msg结构体...\n");
    for (uint32_t i = 0; i < seg_num; i++) {
        printf("丢包段 %d: start_psn=%d, num=%d\n", i,
               ntohl(gw_data_msg->lost_segments[i].start_psn),
               ntohl(gw_data_msg->lost_segments[i].loss_num));
    }

    ssize_t total_sent = sendmsg(sockfd, &msg, 0);
    if (total_sent == -1) {
        // 本次SR重传请求处理终结，记录异常日志后返回
        perror("sendmsg失败");
        close_sr_client_fd(); // 关闭socket，重置全局变量
    } else {
        printf("sendmsg成功发送 %ld 字节数据\n", total_sent);
    }

    // 释放内存
    free(gw_control_msg);
    free(gw_data_msg);
    free(ls_sgments);
    return;
}

// ZPY
// GBN重传函数的子函数:重传最近一个丢包段后面的第1个缓存报文
void retransmit_latest_one_packet(struct connection_cache_array *conn,
                                  char *interface_name, uint32_t epsn,
                                  uint32_t src_qp) {
    uint32_t lost_psn = epsn & PSN_MASK; // PSN_MASK处理
    uint32_t end_psn = conn->end_psn & PSN_MASK;
    uint32_t epsn_end_count = calculate_psn_number(lost_psn + 1, end_psn);

    for (uint32_t i = 0; i < epsn_end_count; i++) {
        uint32_t psn =
            (lost_psn + 1 + i) &
            PSN_MASK; // epsn对应的数据包丢失，从epsn+1开始查找该丢包段后面的第1个缓存报文
        // 重传最近一个丢包段后面的第1个缓存报文
        if (!is_packet_lost(conn, psn)) {
            // 触发重传该数据包
            int ring_index = psn % RING_BUFFER_SIZE;
            unsigned char *packet_data =
                (unsigned char *)conn->ring_buf[ring_index];
            struct mem_block_header *header =
                (struct mem_block_header *)packet_data;
            unsigned char *rdma_packet_data =
                packet_data + sizeof(struct mem_block_header);
            // 发送重传数据包
            // 此处的dest_qp应为NACK报文的发送端QP（src_qp）
            retransmit_rdma_packet(rdma_packet_data, header->data_len,
                                   interface_name, header->psn, src_qp);
            // 新增：重传触发时统计（可选，也可只在retransmit_rdma_packet中统计）
            if (g_perf_monitoring) {
                record_retransmit_packet(&g_perf_stats);
            }
            printf("[GBN_RETRANSMIT] 重传PSN=%u | 环形数组索引=%d | "
                   "内存块地址=0x%lx\n",
                   psn, ring_index, (uintptr_t)packet_data);
            break; // 只重传第1个缓存报文
        }
    }
    return;
}

// ZPY
// GBN重传函数
int dst_gateway_gbn_retransmit(struct connection_cache_array *conn,
                               char *interface_name, uint32_t nack_epsn,
                               uint32_t src_qp) {
    // GBN重传：重传从nack_epsn开始到最近一个丢包段后面的第1个缓存报文，触发目的主机快速发送新的NAK
    // 维护一个epsn，记录最近一个丢包段的位置
    char dst_interface_name[IF_NAMESIZE];
    memcpy(dst_interface_name, interface_name, IF_NAMESIZE);
    uint32_t start_psn = conn->start_psn & PSN_MASK;
    uint32_t end_psn = conn->end_psn & PSN_MASK;
    uint32_t epsn = nack_epsn & PSN_MASK; // PSN_MASK处理
    uint32_t nack_end_count = calculate_psn_number(nack_epsn, end_psn);

    int loss_packet_found = 0;
    int retransmit_count = 0;
    for (uint32_t i = 0; i < nack_end_count; i++) {
        uint32_t psn = (start_psn + i) & PSN_MASK;
        if (is_packet_lost(conn, psn)) {
            // 发现丢包段，停止重传
            // 更新epsn为丢包段的起始PSN,
            // 该psn对应的数据包丢失，从psn+1开始查找该丢包段后面的第1个缓存报文
            epsn = psn;
            loss_packet_found = 1;
            break;
        } else {
            // 触发重传该数据包
            int ring_index = psn % RING_BUFFER_SIZE;
            unsigned char *packet_data =
                (unsigned char *)conn->ring_buf[ring_index];
            struct mem_block_header *header =
                (struct mem_block_header *)packet_data;
            unsigned char *rdma_packet_data =
                packet_data + sizeof(struct mem_block_header);
            // 发送重传数据包
            // 此处的dest_qp应为NACK报文的发送端QP（src_qp）
            retransmit_rdma_packet(rdma_packet_data, header->data_len,
                                   interface_name, header->psn, src_qp);
            retransmit_count++;
            printf("[GBN_RETRANSMIT] 重传PSN=%u | 环形数组索引=%d | "
                   "内存块地址=0x%lx\n",
                   psn, ring_index, (uintptr_t)packet_data);
        }
    }
    if (loss_packet_found) {
        printf("[GBN_RETRANSMIT] 重传遇到丢包段，新的epsn=%u\n", epsn);
        // 重传最近一个丢包段后面的第1个缓存报文, epsn为丢包段的起始PSN
        retransmit_latest_one_packet(conn, dst_interface_name, epsn, src_qp);
    }
    return retransmit_count;
}

// ZPY
// 处理收到的网关消息
void handle_gateway_msg(gateway_control_msg *gw_control_msg,
                        gateway_data_msg *gw_data_msg,
                        const char *interface_name) {
    // 转换字节序
    uint32_t seg_num = ntohl(gw_data_msg->seg_num);
    gw_control_msg->src_ip = ntohl(gw_control_msg->src_ip);
    gw_control_msg->dest_ip = ntohl(gw_control_msg->dest_ip);
    gw_control_msg->src_qp = ntohl(gw_control_msg->src_qp);
    gw_control_msg->dest_qp = ntohl(gw_control_msg->dest_qp);
    gw_control_msg->src_port = ntohs(gw_control_msg->src_port);
    gw_control_msg->dest_port = ntohs(gw_control_msg->dest_port);
    gw_control_msg->pkey = ntohs(gw_control_msg->pkey);
    gw_data_msg->total_data_length = ntohl(gw_data_msg->total_data_length);

    printf("消息类型: %d\n", gw_control_msg->message_type);
    printf("源IP: %u\n", gw_control_msg->src_ip);
    printf("目的IP: %u\n", gw_control_msg->dest_ip);
    printf("源端口: %d\n", gw_control_msg->src_port);
    printf("目的端口: %d\n", gw_control_msg->dest_port);
    printf("源QP号: %d\n", gw_control_msg->src_qp);
    printf("目的QP号: %d\n", gw_control_msg->dest_qp);
    printf("总数据长度: %u\n", gw_data_msg->total_data_length);
    printf("丢包段数量: %u\n", seg_num);

    // 根据解析的控制信息找到对应的连接缓存
    // 控制信息构造时源目的方向已经调换，直接查找即可获得正确方向的缓冲区指针
    // 补充cache的连接级锁
    // 组装连接Key
    struct connection_key key = create_connection_key_u32(
        gw_control_msg->src_ip, gw_control_msg->dest_ip, gw_control_msg->src_qp,
        gw_control_msg->dest_qp, gw_control_msg->pkey);
    // 获取Bucket
    struct connection_bucket *bucket = get_connection_bucket(key);
    // 判断bucket是否存在
    if (bucket == NULL) {
        printf(
            "[WARN] 跳过报文处理，哈希桶不存在[src_ip=0x%08X,dst_ip=0x%08X]\n",
            key.src_ip, key.dst_ip);
        return;
    }
    pthread_rwlock_wrlock(&bucket->rwlock);
    // 获取连接缓存
    struct connection_cache_array *conn =
        get_connection_cache_array(bucket, key);
    // ZPY
    // 判断连接缓存是否存在
    if (conn == NULL) {
        printf("[ERROR] handle_gateway_msg: "
               "连接缓存指针为空，无法处理SR重传请求\n");
        pthread_rwlock_unlock(&bucket->rwlock);
        return;
    }
    if (conn->ring_buf == NULL) {
        printf("[SR_THREAD] 连接缓存未初始化（ring_buf==NULL），丢弃SR请求\n");
        pthread_rwlock_unlock(&bucket->rwlock);
        return;
    }
    // ZPY
    // 删除retransmit_type后做如下修改：
    // 检查lost_segment[0]的start_psn是否缓存
    // 如果缓存，进行SR重传操作
    // 如果未缓存，则丢弃SR重传请求，避免无效重传
    printf("[SR_THREAD] 开始SR重传处理\n");
    uint32_t first_start_psn =
        ntohl(gw_data_msg->lost_segments[0].start_psn) & PSN_MASK;
    if (is_packet_lost(conn, first_start_psn)) {
        printf("[SR_THREAD] "
               "丢弃收到的SR重传请求，lost_segment[0]的start_psn=%u未缓存\n",
               first_start_psn);
        // 这里不做内存释放，由外部函数释放，避免double_free
        pthread_rwlock_unlock(&bucket->rwlock);
        return;
    }
    // 遍历丢包段，进行SR重传
    for (uint32_t i = 0; i < seg_num; i++) {
        uint32_t start_psn = ntohl(gw_data_msg->lost_segments[i].start_psn);
        uint32_t loss_num = ntohl(gw_data_msg->lost_segments[i].loss_num);
        printf("[SR_THREAD] 丢包段 %u: start_psn=%u, loss_num=%u\n", i,
               start_psn, loss_num);

        // 重传该段的包
        uint32_t end_psn = start_psn + loss_num - 1;
        // 计算需要遍历的PSN数量
        uint32_t count = calculate_psn_number(start_psn, end_psn);
        for (uint32_t j = 0; j < count; j++) {
            uint32_t psn = (start_psn + j) & PSN_MASK;
            int ring_index = psn % RING_BUFFER_SIZE;
            if (!is_packet_lost(conn, psn)) {
                struct mem_block_header *packet_data =
                    (struct mem_block_header *)conn->ring_buf[ring_index];
                struct mem_block_header *header =
                    (struct mem_block_header *)packet_data;
                unsigned char *rdma_packet_data =
                    (unsigned char *)packet_data +
                    sizeof(struct mem_block_header);
                if (header->psn == psn) {
                    retransmit_rdma_packet(rdma_packet_data, header->data_len,
                                           interface_name, psn,
                                           gw_control_msg->dest_qp);
                    printf("[SR_THREAD] 重传PSN=%u, QP=%u\n", psn,
                           gw_control_msg->dest_qp);
                }
            }
        }
    }
    pthread_rwlock_unlock(&bucket->rwlock);
    return;
}

// ZPY
// 将套接字设为非阻塞
static int set_socket_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        perror("fcntl get flags");
        return -1;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl set nonblock");
        return -1;
    }
    return 0;
}

// ZPY
// 监听并处理SR重传请求
void handle_sr_requests(void) {
    // interface_name应为源网关对应的接口
    // 根据实际情况设置接口名称
    char src_sr_interface_name[IF_NAMESIZE];
    memcpy(src_sr_interface_name, g_src_sr_interface_name, IF_NAMESIZE);
    int client_fd = -1;
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    size_t buffer_size =
        sizeof(uint32_t) * 2 + MAX_LOST_SEGMENTS * sizeof(lost_segment);
    gateway_control_msg *gw_control_msg =
        (gateway_control_msg *)malloc(sizeof(gateway_control_msg));
    char *buffer = (char *)malloc(buffer_size);
    if (!gw_control_msg || !buffer) {
        perror("内存分配失败");
        free(gw_control_msg);
        free(buffer);
        return;
    }
    // 可以修改全局变量中断监听循环
    while (g_running) {
        // 接受客户端连接
        client_fd = accept(g_tcp_server_sockfd, (struct sockaddr *)&client_addr,
                           &client_len);
        if (client_fd == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(10000); // 非阻塞模式下无连接，稍后重试
                continue;
            }
            perror("accept失败");
            continue; // 继续监听下一个连接
        }
        printf("客户端连接成功: %s:%d\n", inet_ntoa(client_addr.sin_addr),
               ntohs(client_addr.sin_port));

        // 客户端socket设为非阻塞，避免recvmsg阻塞
        if (set_socket_nonblocking(client_fd) == -1) {
            close(client_fd);
            continue;
        }

        while (g_running) {
            memset(gw_control_msg, 0, sizeof(gateway_control_msg));
            memset(buffer, 0, buffer_size);

            // 使用recvmsg接收数据
            struct iovec iov[2];
            iov[0].iov_base = gw_control_msg;
            iov[0].iov_len = sizeof(gateway_control_msg);
            iov[1].iov_base = buffer;
            iov[1].iov_len = buffer_size;

            struct msghdr msg;
            memset(&msg, 0, sizeof(msg));
            msg.msg_iov = iov;
            msg.msg_iovlen = 2;

            // 接收辅助数据（可选）
            char ctrl_buf[256];
            msg.msg_control = ctrl_buf;
            msg.msg_controllen = sizeof(ctrl_buf);

            printf("等待接收数据...\n");

            ssize_t total_received = recvmsg(client_fd, &msg, 0);
            if (total_received == -1) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    usleep(10000); // 无数据可读，稍后重试
                    continue;
                }
                // 接收失败
                perror("recvmsg失败, 监听下一个连接");
                close(client_fd);
                break;
            }
            if (total_received == 0) {
                // 客户端正常关闭连接
                printf("客户端已关闭连接: %s:%d\n",
                       inet_ntoa(client_addr.sin_addr),
                       ntohs(client_addr.sin_port));
                close(client_fd);
                break;
            }

            printf("总共接收 %ld 字节数据\n", total_received);

            // 基本长度校验：至少要包含控制头和数据头
            if (total_received < (ssize_t)(sizeof(gateway_control_msg) +
                                           sizeof(gateway_data_msg))) {
                printf("接收数据长度不足: %ld < %zu, 丢弃本次消息\n",
                       total_received,
                       sizeof(gateway_control_msg) + sizeof(gateway_data_msg));
                // 跳过本次循环，继续等待下一条消息
                continue;
            }
            // 解析gateway_data_msg
            gateway_data_msg *gw_data_msg = (gateway_data_msg *)buffer;
            // 重传类型为SR时才处理重传请求
            handle_gateway_msg(gw_control_msg, gw_data_msg,
                               src_sr_interface_name);
            // 检查是否收到辅助数据
            if (msg.msg_controllen > 0) {
                printf("收到辅助数据，长度: %zu\n", msg.msg_controllen);
            }
            // // 获取发送方地址信息
            // printf("接收标志: 0x%x\n", msg.msg_flags);
        }
    }
    // 退出循环后中断连接
    if (client_fd != -1) {
        close(client_fd);
    }
    // 回收内存
    free(gw_control_msg);
    free(buffer);
    return;
}

// ZPY
// SR重传线程
void *sr_retransmit_worker(void *arg) {
    int fd;
    int port = *(int *)arg;
    free(arg);

    struct sockaddr_in server_addr;

    // 创建socket
    if ((fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        perror("socket创建失败");
        exit(EXIT_FAILURE);
    }

    // 设置SO_REUSEADDR选项
    int opt = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
        perror("setsockopt失败");
        close(fd);
        exit(EXIT_FAILURE);
    }

    // 绑定地址
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(port);

    if (bind(fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        perror("bind失败");
        close(fd);
        return NULL;
    }

    // 监听
    if (listen(fd, 5) == -1) {
        perror("listen失败");
        close(fd);
        return NULL;
    }

    // 监听socket设为非阻塞，避免accept阻塞退出流程
    if (set_socket_nonblocking(fd) == -1) {
        close(fd);
        return NULL;
    }

    g_tcp_server_sockfd = fd;
    printf("服务器监听端口 %d...\n", port);
    handle_sr_requests();
    printf("[SR_THREAD] SR重传线程退出\n");
    return NULL;
}

// ZPY
// 启动SR重传线程，建立TCP连接，等待SR重传请求
// 这里是简单逻辑版本
int start_retransmit_thread(int port) {
    int *port_ptr = malloc(sizeof(int));
    if (!port_ptr) {
        perror("分配SR线程端口参数失败");
        return -1;
    }
    *port_ptr = port;

    if (g_retransmit_thread > 0) {
        printf("[SR_THREAD] SR重传线程已在运行\n");
        free(port_ptr);
        return -1;
    }

    if (pthread_create(&g_retransmit_thread, NULL, sr_retransmit_worker,
                       (void *)port_ptr) != 0) {
        perror("创建SR重传线程失败");
        free(port_ptr);
        return -1;
    }

    // 不分离线程，主线程可以在需要时join
    // thread内已经释放port_ptr内存，这里不再释放
    printf("[SR_THREAD] SR重传线程已启动，监听端口%d\n", port);
    return 0;
}

// ZPY
// 重传线程回收函数
void stop_retransmit_thread(void) {
    if (g_retransmit_thread > 0) {
        pthread_join(g_retransmit_thread, NULL);
        printf("[SR_THREAD] 重传线程已退出并回收\n");
        g_retransmit_thread = 0;
    }
}

void cleanup_retransmit_resources(void) {
    // 关闭监听套接字，唤醒阻塞的 accept
    if (g_tcp_server_sockfd > 0) {
        close(g_tcp_server_sockfd);
        g_tcp_server_sockfd = -1;
    }

    // 关闭可能存在的客户端/发送套接字，防止阻塞的 recvmsg
    if (g_sr_client_sockfd > 0) {
        close(g_sr_client_sockfd);
        g_sr_client_sockfd = -1;
    }

    // 关闭AF_PACKET传输的socket_fd
    if (g_retransmit_sockfd > 0) {
        close(g_retransmit_sockfd);
        g_retransmit_sockfd = -1;
    }
}

// ZPY
// 向源主机发送NAK
void send_nak_to_source_host(const unsigned char *pack_data, int length,
                             const char *interface_name, uint32_t nack_epsn,
                             uint32_t dest_qp) {
    // 修改源MAC发送给源主机, 调用retransmit_rdma_packet函数
    printf("[NAK_TO_HOST] 向源主机发送NAK: epsn=%u \n", nack_epsn);
    retransmit_rdma_packet(pack_data, length, interface_name, nack_epsn,
                           dest_qp);
}

// ZPY
// 基于nak报头获取流表后获取到的网关角色，需要角色互换后才是数据报文方向上网关的角色
void handle_nack_received(const unsigned char *buffer, ssize_t length,
                          struct connection_bucket *bucket,
                          struct connection_key key, uint32_t epsn,
                          enum gateway_role role) {
    // 入参合法性校验
    // 组装连接反向Key
    struct connection_key reverse_key = create_connection_key_u32(
        key.dst_ip, key.src_ip, key.dst_qp, key.src_qp, key.pkey);
    // 获取反向Bucket
    struct connection_bucket *reverse_bucket =
        get_connection_bucket(reverse_key);
    // 获取网关角色
    enum gateway_role reverse_role =
        (role == src_gateway) ? dst_gateway : src_gateway;
    if (reverse_bucket == NULL) {
        printf("[ERROR] handle_nack_received: 哈希桶指针为空，无法处理NACK\n");
        return;
    }
    // 统一处理PSN
    uint32_t nack_epsn = epsn;
    struct connection_cache_array *cache_array = NULL;

    // 哈希桶加读锁：遍历/查找连接属于读操作，读锁保证并发安全
    pthread_rwlock_rdlock(&reverse_bucket->rwlock);

    // 复用封装函数查找连接缓存
    // 在传参时获取的就是反向的bucket和key
    // 获取连接条目对应的缓存数组
    cache_array = get_connection_cache(reverse_bucket->head, reverse_key);

    // 添加判断，确保cache_array不为NULL后再进行后续处理
    int clean_success = 0;
    // 处理连接查找结果
    if (cache_array != NULL) {
        // 过滤空缓存数组：连接有效但缓存未初始化
        if (cache_array->ring_buf == NULL) {
            printf("[WARN] handle_nack_received: "
                   "匹配到连接[src_ip=0x%08X,dst_ip=0x%08X], "
                   "但缓存环形数组未初始化，跳过NACK清理\n",
                   key.src_ip, key.dst_ip);
            pthread_rwlock_unlock(
                &reverse_bucket->rwlock); // 提前解锁，避免泄漏
            return;
        }

        // 步骤1.调用核心清理函数，处理NACK确认的报文
        // printf("[INFO] handle_nack_received: "
        //        "匹配到目标连接，开始清理NACK确认报文 | NACK PSN=0x%06X\n",
        //        nack_epsn);
        // 在调用clean_nacked_packets时注意内部nack_epsn-1
        int clean_ret = clean_nacked_packets(cache_array, nack_epsn);

        // 根据清理结果打印分级日志（复用原错误码）
        switch (clean_ret) {
        case RETRANS_NO_VALID_PSN_RANGE:
            // printf("[NACK RECV] 该连接无有效PSN范围，无需清理报文\n");
            break;
        case RETRANS_NO_CACHED_PACKETS:
            printf("[NACK RECV] 该连接无缓存数据包，无需清理报文\n");
            break;
        default:
            if (clean_ret > 0) {
                clean_success = 1;
                // printf("[NACK RECV] ✅ NACK清理完成，本次释放已确认报文=%d个\n",
                //        clean_ret);
            } else if (clean_ret < 0) {
                printf(
                    "[ERROR] handle_nack_received: NACK清理失败，错误码=%d\n",
                    clean_ret);
            }
            break;
        }
    } else {
        // 未匹配到对应连接的日志打印
        printf("[DEBUG] handle_nack_received: "
               "哈希桶中未匹配到指定连接[src_ip=0x%08X,dst_ip=0x%08X], "
               "跳过NACK清理\n",
               key.src_ip, key.dst_ip);
    }
    if (!clean_success) {
        printf("[NACK RECV] NACK清理未成功，跳过后续重传处理\n");
        pthread_rwlock_unlock(&reverse_bucket->rwlock); // 解锁
        return;
    }
    // 区分RDMA的ACK和TCP的ACK，TCP的ACK是累计确认，表示下一个期待的字节序号
    // RDMA的ACK报文中的PSN表示小于等于PSN的数据包都已收到
    // RDMA的NAK报文中的PSN才表示下一个期待的PSN，因为该PSN未被接收处理
    // 基于网关角色进行不同的操作
    if (reverse_role == dst_gateway) {
        printf("[NACK] DESTINATION_GATEWAY收到NACK: src_ip=%u, dst_ip=%u, "
               "dest_qp=%u, nack_epsn=%u\n",
               key.src_ip, key.dst_ip, key.dst_qp, nack_epsn);
        // interface_name应为目的网关对应的接口
        // 根据实际情况设置接口名称
        char dst_gbn_interface_name[IF_NAMESIZE];
        memcpy(dst_gbn_interface_name, g_dst_gbn_interface_name, IF_NAMESIZE);
        // 目的网关收到NAK：触发GBN重传或发起SR重传请求
        // NAK报文中psn在目的网关上存在，目的网关触发GBN重传。
        // NAK报文中psn在目的网关上不存在，目的网关向源网关发送SR重传请求。
        printf("[DEST_GW] 目的网关处理NACK\n");

        // 执行重传逻辑，不用process_retransmit_by_epsn函数
        // 考察返回的第一个数据包，如果PSN大于nack_epsn，证明目的网关没有缓存epsn对应的数据包，此时应发起SR重传请求
        // 如果PSN等于nack_epsn（不存在小于的情况），则进行GBN重传，重传数据考虑nack_epsn及之后的所有连续包
        // GBN重传：重传从nack_epsn开始到最近一个丢包段后面的第1个缓存报文，触发目的主机快速发送新的NAK
        // 维护一个epsn，记录最近一个丢包段的位置
        // 步骤2：处理psn ≥ epsn的数据包
        // 步骤2.1：判断是否存在nack_epsn对应的数据包
        // 步骤2.2：根据存在与否，选择GBN重传或发起SR重传请求
        // bug修复，受限于AF_PACKET性能问题，可能出现该情况
        if (cache_array->start_psn == PSN_INVALID ||
            cache_array->end_psn == PSN_INVALID) {
            printf("[WARN] "
                   "NACK清理完目的网关所有缓存，epsn=%"
                   "u，无法进行GBN重传或SR请求，不做操作\n",
                   nack_epsn);
            pthread_rwlock_unlock(&reverse_bucket->rwlock);
            return;
        }
        if (is_packet_lost(cache_array, nack_epsn)) {
            // 不存在，发起SR重传请求
            // 目的网关没有nack_epsn对应的数据包，发起SR重传请求给源网关
            printf("[SR_REQUEST] 目的网关发起SR重传请求给源网关: src_ip=%u, "
                   "dst_ip=%u, qp=%u\n",
                   key.dst_ip, key.src_ip, key.src_qp);
            // 原始顺序，源目的需要交换
            send_sr_request_to_src_gateway(key, cache_array);
        } else {
            // 存在，进行GBN重传
            // 目的网关有nack_epsn对应的数据包，进行GBN重传
            printf("[GBN_RETRANSMIT] 目的网关进行GBN重传: src_ip=%u, "
                   "dst_ip=%u, qp=%u\n",
                   key.src_ip, key.dst_ip, key.dst_qp);
            int retransmit_count = dst_gateway_gbn_retransmit(
                cache_array, dst_gbn_interface_name, nack_epsn, key.src_qp);
            if (retransmit_count == 0) {
                printf("[ERROR] "
                       "目的网关GBN重传数据包失败或者没有数据包需要重传\n");
            } else {
                printf(
                    "[GBN_RETRANSMIT] 目的网关GBN重传完成，重传报文数量=%d\n",
                    retransmit_count);
            }
        }
    } else { // 源网关
        printf("[NACK] SOURCE_GATEWAY收到NACK: src_ip=%u, dst_ip=%u, "
               "dest_qp=%u, nack_epsn=%u\n",
               key.src_ip, key.dst_ip, key.dst_qp, nack_epsn);

        // 源网关SR重传请求接收线程已启动，该部分由WTC负责，重传线程中收到SR重传请求后的操作由ZPY负责
        // 源网关收到NAK重定向报文（NAK已经重定向给源网关，源主机收不到目的主机发送的NAK）
        printf("[SOURCE_GW] 源网关收到NACK重定向报文\n");

        // interface_name应为源网关对应的接口
        // 根据实际情况设置接口名称
        char src_nack_interface_name[IF_NAMESIZE];
        memcpy(src_nack_interface_name, g_src_nack_interface_name, IF_NAMESIZE);
        //  步骤2：检查缓存情况
        //  NAK报文中psn在源网关上存在，不做动作，等收到SR重传请求后触发SR选择重传，此时源主机不触发GBN重传。
        //  NAK报文中psn在源网关上不存在，基于收到的NAK报文修改源MAC发送给源主机，由源主机触发GBN重传，同时源网关丢弃SR重传请求。
        // bug修复，受限于AF_PACKET性能问题，可能出现该情况
        if (cache_array->start_psn == PSN_INVALID ||
            cache_array->end_psn == PSN_INVALID) {
            printf("[WARN] "
                   "NACK清理完源网关所有缓存，直接向源主机发送NAK，epsn=%u\n",
                   nack_epsn);
            send_nak_to_source_host(buffer, length, src_nack_interface_name,
                                    nack_epsn, key.dst_qp);
            pthread_rwlock_unlock(&reverse_bucket->rwlock);
            return;
        }
        if (!is_packet_lost(cache_array, nack_epsn)) {
            // 有epsn缓存，设置重传类型为SR重传
            printf("[SR_RETRANS] 源网关有epsn=%u的缓存，设置重传类型为SR重传\n",
                   nack_epsn);
        } else {
            // NAK报文中psn在源网关上不存在，设置重传类型为GBN重传，修改源MAC发送给源主机
            printf("[GBN_RETRANS] "
                   "源网关不存在epsn=%u"
                   "的缓存，设置重传类型为GBN重传，向源主机发送NAK\n",
                   nack_epsn);
            send_nak_to_source_host(buffer, length, src_nack_interface_name,
                                    nack_epsn, key.dst_qp);
        }
    }
    // 解锁哈希桶
    pthread_rwlock_unlock(&reverse_bucket->rwlock);
    return;
}
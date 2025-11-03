// 用户态通过AF_PACKET截获网卡报文
// 开启网卡混杂模式 + 原始套接字，绕过标准网络栈

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <netinet/ip.h>
#include <netinet/udp.h>

#include "rdma_opcode.h"

// 以太网头部结构 (已经存在于 <netinet/if_ether.h>)
// 编译调测时注释掉
struct ethhdr {
    unsigned char h_dest[ETH_ALEN];   // 目标MAC地址
    unsigned char h_source[ETH_ALEN]; // 源MAC地址
    unsigned short h_proto;           // 上层协议类型
};

// IP头部结构 (已经存在于 <netinet/ip.h>)
// 编译调测时注释掉
struct iphdr {
#if __BYTE_ORDER == __LITTLE_ENDIAN
    unsigned int ihl:4;               // IP头部长度
    unsigned int version:4;           // IP版本
#elif __BYTE_ORDER == __BIG_ENDIAN
    unsigned int version:4;           // IP版本
    unsigned int ihl:4;               // IP头部长度
#else
# error "Please fix <bits/endian.h>"
#endif
    uint8_t tos;                      // 服务类型
    uint16_t tot_len;                 // 总长度
    uint16_t id;                      // 标识
    uint16_t frag_off;                // 分片偏移
    uint8_t ttl;                      // 生存时间
    uint8_t protocol;                 // 上层协议
    uint16_t check;                   // 校验和
    uint32_t saddr;                   // 源IP地址
    uint32_t daddr;                   // 目标IP地址
};

// UDP头部结构 (已经存在于 <netinet/udp.h>)
// 编译调测时注释掉
struct udphdr {
    uint16_t source;                  // 源端口
    uint16_t dest;                    // 目标端口
    uint16_t len;                     // UDP长度
    uint16_t check;                   // 校验和
};

// RoCEv2 BTH (Base Transport Header) 结构
struct bth {
    uint8_t opcode;                   // 操作码 (8位)
    uint8_t solicited_mig_req;        // 标志位 (8位)
    uint8_t pad_count;                // 填充计数 (8位)
    uint8_t version;                  // 传输版本 (8位)
    uint16_t pkey;                    // 分区键 (16位)
    uint8_t reserved_f;               // 保留字段 + F位 (8位)
    uint32_t dest_qp;                 // 目标QP号 (24位)
    uint8_t ack_req;                  // ACK请求 (4位)
    uint8_t reserved[3];              // 保留字段 (12位)
    uint32_t psn;                     // 包序列号 (24位)
} __attribute__((packed));

// AETH结构定义
struct aeth {
    uint8_t syndrome;        // 综合征（包含MSN和代码）
    uint32_t credit;         // 信用值（24位）
} __attribute__((packed));

// 典型的ACK报文结构
struct rc_ack_packet {
    struct bth bth_header;    // BTH头
    struct aeth aeth_header;  // AETH头
    // 可能还包含立即数数据
};

// AETH头的作用：
// 信用更新：通过信用值字段通知发送方可用的接收缓冲区
// 状态确认：通过综合征字段确认接收状态
// MSN更新：包含消息序列号，用于流量控制

// ❌ 特殊情况：无AETH头的ACK
// 在某些特定场景下，ACK可能不包含AETH头：
// 1. 搭载ACK（Piggybacked ACK）
// 在数据报文中搭载ACK信息，无需独立的AETH头
// struct piggybacked_ack {
//     struct bth bth_header;           // BTH头（包含ACK信息）
//     // 可能在其他头字段中包含ACK状态
//     uint8_t payload[];               // 数据载荷
// };
// 2. 隐式ACK
// 在某些优化场景中，接收方可能通过以下方式隐式确认：
// 反向流量：响应数据包本身隐含ACK
// 定时器机制：超时未收到NAK视为ACK

// 创建混杂模式socket，用于从网卡拦截报文到用户态
int create_promiscuous_socket(const char *interface_name)
{
    // 创建原始套接字
    int sockfd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (sockfd < 0) {
        perror("socket");
        return -1;
    }
    
    // 绑定到指定网络接口
    struct sockaddr_ll sll;
    memset(&sll, 0, sizeof(sll));
    sll.sll_family = AF_PACKET;
    sll.sll_ifindex = if_nametoindex(interface_name); // eth0/wlan0/rmnet0
    sll.sll_protocol = htons(ETH_P_ALL);
    
    if (bind(sockfd, (struct sockaddr*)&sll, sizeof(sll)) < 0) {
        perror("bind");
        close(sockfd);
        return -1;
    }
    
    // 设置混杂模式 - 这是关键
    struct packet_mreq mr;
    memset(&mr, 0, sizeof(mr));
    mr.mr_ifindex = sll.sll_ifindex;
    mr.mr_type = PACKET_MR_PROMISC;
    
    if (setsockopt(sockfd, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &mr, sizeof(mr)) < 0) {
        perror("setsockopt promisc");
        close(sockfd);
        return -1;
    }
    
    printf("成功在接口 %s 上开启混杂模式\n", interface_name);
    return sockfd;
}

// RDMA服务类型获取
uint8_t infer_service_type(uint8_t opcode) {
    // RC (Reliable Connected) 服务类型的操作码范围
    if ((opcode >= 0x00 && opcode <= 0x1F) || 
        (opcode >= 0x80 && opcode <= 0x9F)) {
        return 1;  // RC
    }
    // UC (Unreliable Connected) 服务类型的操作码范围  
    else if ((opcode >= 0x20 && opcode <= 0x3F) ||
             (opcode >= 0xA0 && opcode <= 0xBF)) {
        return 2;  // UC
    }
    // UD (Unreliable Datagram) 服务类型的操作码范围
    else if ((opcode >= 0x40 && opcode <= 0x5F) ||
             (opcode >= 0xC0 && opcode <= 0xDF)) {
        return 3;  // UD
    }
    // RAW (Raw Datagram) 等服务类型
    else {
        return 0;  // 未知或其它
    }
}

// 从BTH中提取24位字段的辅助函数
static inline uint32_t get_24bit_value(const uint8_t *data) {
    return (data[0] << 16) | (data[1] << 8) | data[2];
}

// 增强的BTH解析，支持ACK/NACK
int parse_bth_header(const unsigned char *bth_start, 
                     uint8_t *opcode, uint16_t *pkey,
                     uint32_t *dest_qp, uint32_t *psn,
                     rdma_packet_type_t *pkt_type) {
    struct bth *bth = (struct bth*)bth_start;
    
    *opcode = bth->opcode;
    *pkey = ntohs(bth->pkey);
    
    // 提取24位的dest_qp
    const uint8_t *dest_qp_ptr = (const uint8_t*)(bth_start + 6);
    *dest_qp = get_24bit_value(dest_qp_ptr) & 0x00FFFFFF;
    
    // 提取24位的psn
    const uint8_t *psn_ptr = (const uint8_t*)(bth_start + 9);
    *psn = get_24bit_value(psn_ptr) & 0x00FFFFFF;
    
    // 判断报文类型
    *pkt_type = get_packet_type_rc(*opcode);
    
    return 0;
}

// 解析AETH头部（ACK/NACK专用）
int parse_aeth_header(const unsigned char *aeth_start,
                     uint8_t *syndrome, uint32_t *epsn) {
    struct aeth *aeth = (struct aeth*)aeth_start;
    
    *syndrome = get_aeth_syndrome(aeth);
    *epsn = get_aeth_epsn(aeth);
    
    return 0;
}

// 处理数据报文
void process_data_packet(const unsigned char *buffer, ssize_t length,
                        int bth_offset,
                        const char *src_ip, const char *dst_ip,
                        uint16_t src_port, uint16_t dst_port,
                        uint16_t pkey, uint32_t dest_qp, uint32_t psn,
                        uint8_t opcode) {
    printf("数据报文: %s:%d -> %s:%d, ", src_ip, src_port, dst_ip, dst_port);
    printf("PKey=0x%04x, QP=%u, PSN=%u, 操作码=0x%02x\n", 
           pkey, dest_qp, psn, opcode);
    
    // 计算应用数据位置
    int bth_header_len = 12;
    int app_data_offset = bth_offset + bth_header_len;
    int udp_header_len = sizeof(struct udphdr);
    int ip_header_len = ((struct iphdr*)(buffer + sizeof(struct ethhdr)))->ihl * 4;
    int udp_payload_len = ntohs(((struct udphdr*)(buffer + sizeof(struct ethhdr) + ip_header_len))->len);
    int app_data_len = udp_payload_len - udp_header_len - bth_header_len;
    
    if (app_data_len > 0 && app_data_offset + app_data_len <= length) {
        const unsigned char *app_data = buffer + app_data_offset;
        
        // 添加到缓存
        uint8_t service_type = infer_service_type(opcode);
        add_to_connection_cache(src_ip, dst_ip, src_port, dst_port,
                               service_type, pkey, dest_qp, psn,
                               app_data, app_data_len);
    }
}

// 处理ACK报文
void process_ack_packet(const unsigned char *buffer, ssize_t length,
                       int bth_offset,
                       const char *src_ip, const char *dst_ip,
                       uint16_t src_port, uint16_t dst_port,
                       uint16_t pkey, uint32_t dest_qp, uint32_t psn,
                       uint8_t opcode) {
    // ACK报文在BTH之后有AETH
    int aeth_offset = bth_offset + 12; // BTH固定12字节
    
    if (aeth_offset + 4 > length) { // AETH固定4字节
        printf("ACK报文过短，无法解析AETH\n");
        return;
    }
    
    const unsigned char *aeth_start = buffer + aeth_offset;
    uint8_t syndrome;
    uint32_t epsn;
    
    // TODO: RC类型的ACK报文一定有AETH头吗？
    // 对于RC（Reliable Connected）服务类型，ACK报文通常都包含AETH头，但存在一些特殊情况。
    if (parse_aeth_header(aeth_start, &syndrome, &epsn) != 0) {
        printf("AETH解析失败\n");
        return;
    }
    
    printf("ACK报文: %s:%d -> %s:%d, ", src_ip, src_port, dst_ip, dst_port);
    printf("PKey=0x%04x, QP=%u, PSN=%u, ePSN=%u, Syndrome=0x%02x\n",
           pkey, dest_qp, psn, epsn, syndrome);
    
    // 处理ACK逻辑：确认数据包接收，可以清理缓存
    handle_ack_received(src_ip, dst_ip, src_port, dst_port, pkey, epsn);
}

// 处理ACK接收
void handle_ack_received(const char *src_ip, const char *dst_ip,
                        uint16_t src_port, uint16_t dst_port,
                        uint16_t pkey, uint32_t ack_epsn)
{
    // 创建连接键（注意源目的对调，因为ACK是从对端发回的）
    struct connection_key key = create_connection_key(dst_ip, src_ip, 
                                                     dst_port, src_port,
                                                     0, pkey);
    
    printf("收到ACK: %s:%d -> %s:%d, 确认PSN <= %u\n", 
           src_ip, src_port, dst_ip, dst_port, ack_epsn);
    
    // 查找并释放所有PSN <= ack_epsn 的已确认报文
    int cleaned_count = 0;
    size_t freed_bytes = 0;
    
    pthread_mutex_lock(&g_cache_mgr->global_lock);
    
    // 查找连接缓存
    uint32_t hash_index = calculate_hash(&key, g_cache_mgr->hash_table_size);
    struct hash_table_entry *entry = g_cache_mgr->hash_table[hash_index];
    
    while (entry) {
        if (connection_keys_equal(&entry->key, &key)) {
            struct connection_cache *cache = entry->cache;
            pthread_mutex_lock(&cache->lock);
            
            // 遍历有序链表，释放PSN <= ack_epsn的所有报文
            struct cached_packet *current = cache->head;
            struct cached_packet *prev = NULL;
            
            while (current && current->psn <= ack_epsn) {
                struct cached_packet *to_free = current;
                current = current->next;
                
                // 从链表中移除
                if (prev) {
                    prev->next = current;
                } else {
                    cache->head = current;
                }
                
                if (current) {
                    current->prev = prev;
                } else {
                    cache->tail = prev;
                }
                
                // 更新统计
                freed_bytes += to_free->data_len + sizeof(struct cached_packet);
                cleaned_count++;
                cache->count--;
                cache->total_bytes -= (to_free->data_len + sizeof(struct cached_packet));
                
                // 释放内存
                free(to_free->app_data);
                free(to_free);
            }
            
            // 更新最小PSN
            if (cache->head) {
                cache->min_psn = cache->head->psn;
            } else {
                cache->min_psn = 0;
                cache->max_psn = 0;
            }
            
            pthread_mutex_unlock(&cache->lock);
            break;
        }
        entry = entry->next;
    }
    
    pthread_mutex_unlock(&g_cache_mgr->global_lock);
    
    if (cleaned_count > 0) {
        printf("ACK处理: 清理了 %d 个已确认报文 (PSN <= %u), 释放 %zu 字节\n", 
               cleaned_count, ack_epsn, freed_bytes);
    } else {
        printf("ACK处理: 没有找到需要清理的报文 (PSN <= %u)\n", ack_epsn);
    }
}

// 处理NACK报文
void process_nack_packet(const unsigned char *buffer, ssize_t length,
                        int bth_offset,
                        const char *src_ip, const char *dst_ip,
                        uint16_t src_port, uint16_t dst_port,
                        uint16_t pkey, uint32_t dest_qp, uint32_t psn,
                        uint8_t opcode) {
    // NACK报文在BTH之后有AETH
    int aeth_offset = bth_offset + 12;
    
    if (aeth_offset + 4 > length) {
        printf("NACK报文过短，无法解析AETH\n");
        return;
    }
    
    const unsigned char *aeth_start = buffer + aeth_offset;
    uint8_t syndrome;
    uint32_t epsn;
    
    if (parse_aeth_header(aeth_start, &syndrome, &epsn) != 0) {
        printf("AETH解析失败\n");
        return;
    }
    
    printf("NACK报文: %s:%d -> %s:%d, ", src_ip, src_port, dst_ip, dst_port);
    printf("PKey=0x%04x, QP=%u, PSN=%u, ePSN=%u, Syndrome=0x%02x\n",
           pkey, dest_qp, psn, epsn, syndrome);
    
    // 处理NACK逻辑：触发重传
    handle_nack_received(src_ip, dst_ip, src_port, dst_port, pkey, epsn, syndrome);
}

// 处理NACK接收
void handle_nack_received(const char *src_ip, const char *dst_ip,
                         uint16_t src_port, uint16_t dst_port,
                         uint16_t pkey, uint32_t nack_epsn, uint8_t syndrome)
{
    // 创建连接键（注意源目的对调）
    struct connection_key key = create_connection_key(dst_ip, src_ip,
                                                     dst_port, src_port,
                                                     0, pkey);
    
    const char *error_desc = get_nack_error_description(syndrome);
    printf("收到NACK: %s:%d -> %s:%d, ePSN=%u, 错误: %s (0x%02x)\n",
           src_ip, src_port, dst_ip, dst_port, nack_epsn, error_desc, syndrome);
    
    // 精确查找ePSN对应的报文（不是范围，而是精确匹配）
    struct cached_packet *exact_packet = NULL;
    
    pthread_mutex_lock(&g_cache_mgr->global_lock);
    
    uint32_t hash_index = calculate_hash(&key, g_cache_mgr->hash_table_size);
    struct hash_table_entry *entry = g_cache_mgr->hash_table[hash_index];
    uint32_t epsn_exists = false;
    
    while (entry) {
        if (connection_keys_equal(&entry->key, &key)) {
            struct connection_cache *cache = entry->cache;
            pthread_mutex_lock(&cache->lock);

            // TODO: 不要直接遍历当前连接的报文缓存链表，效率低
            // 先遍历bitma丢包链表，判断ePSN是否处于丢包节点范围
            // 获取本身边是否缓存了ePSN报文，然后再根据当前数据流方向上本设备的角色做不同处理
            // 1、目的网关角色：
            //    01.有ePSN缓存，目的主机侧丢包，触发GBN重传
            //    02.无ePSN缓存，广域网/源主机侧丢包，向源网关发起SR重传请求(需要创建TCP socket)
            // 2、源网关角色：
            //    01.有ePSN缓存，广域网丢包，收到NAK不处理，等待目的网关发送SR重传请求。
            //    02.无ePSN缓存，源主机侧丢包，给源主机构造发送NAK，触发GBN重传
            
            // 在有序链表中精确查找ePSN对应的报文
            struct cached_packet *current = cache->head;
            while (current) {
                if (current->psn >= nack_epsn) {
                    // 通过AF_PACKET修改源MAC方式重传缓存的报文
                    retransmit_rdma_packet(key, current->app_data, current->data_len, 
                                           current->dest_qp, current->psn);
                } else {
                    // TODO: 摘除已经收到报文缓存节点
                    // 如果本次重传仍未收到，后面再次触发重传时前面已收到报文节点就不用重复遍历
                }
                current = current->next;
            }
            
            pthread_mutex_unlock(&cache->lock);
            break;
        }
        entry = entry->next;
    }
    
    pthread_mutex_unlock(&g_cache_mgr->global_lock);
    
    // 触发精确重传
    if (!epsn_exists) {
        printf("NACK处理: 未找到需要重传的报文 PSN=%u\n", nack_epsn);

        // TODO: 此处需要判断当前数据流方向上本设备的角色
        // 1、目的网关角色：无ePSN缓存，广域网丢包，向源网关发起SR重传请求(需要创建TCP socket)
        // 2、源网关角色：无ePSN缓存，源主机侧丢包，给源主机构造发送NAK，触发GBN重传
        
        // 如果精确PSN没找到，尝试重传ePSN之后的所有报文（容错处理）
        handle_nack_fallback_retransmit(&key, nack_epsn);
    }
}

// 获取NACK错误描述(调试使用，非功能代码)
const char* get_nack_error_description(uint8_t syndrome)
{
    switch (syndrome) {
        case 0x01: return "PSN序列错误";
        case 0x02: return "无效请求";
        case 0x03: return "远程操作错误";
        case 0x04: return "远程访问错误";
        case 0x05: return "远程操作权限错误";
        case 0x06: return "无效RDMA请求";
        case 0x07: return "远程原子操作错误";
        case 0x08: return "RNR (Receiver Not Ready)";
        case 0x09: return "RNR重传超时";
        case 0x0A: return "传输重传超时";
        case 0x0B: return "QP状态错误";
        case 0x0C: return "操作超时";
        case 0x0D: return "一般性传输错误";
        default:   return "未知错误";
    }
}

// GBN批量重传
void trigger_batch_retransmit(struct cached_packet *packet, 
                              int retransmit_count,
                              const struct connection_key *key)
{
    printf("触发批量重传: PSN=%u, 大小=%d 字节\n", 
           packet->psn, packet->data_len);
    
    struct cached_packet *current = packet;
    struct cached_packet *prev = NULL;
    while (current) {
        // 通过AF_PACKET修改源MAC方式重传缓存的报文
        retransmit_rdma_packet(key, packet->app_data, packet->data_len, 
                            packet->dest_qp, packet->psn);
        prev = current;
        current = current->next;
        // 重传后释放报文内存（假设重传函数会复制数据）
        free(prev->app_data);
        free(prev);
    }
}


// 单个报文重传
void trigger_single_retransmit(struct cached_packet *packet, 
                               const struct connection_key *key) {
    printf("触发精确重传: PSN=%u, 大小=%d 字节\n", 
           packet->psn, packet->data_len);
    
    // 通过AF_PACKET修改源MAC方式重传缓存的报文
    retransmit_rdma_packet(key, packet->app_data, packet->data_len, 
                          packet->dest_qp, packet->psn);
}

// 容错重传：当精确PSN找不到时，重传ePSN之后的所有报文
void handle_nack_fallback_retransmit(const struct connection_key *key, 
                                   uint32_t nack_epsn) {
    printf("启动容错重传: 重传PSN >= %u 的所有报文\n", nack_epsn);
    
    int retransmit_count = 0;
    uint32_t current_max_psn;
    
    // 获取当前最大PSN
    if (get_connection_psn_range(key, NULL, &current_max_psn) != 0) {
        printf("容错重传: 无法获取连接PSN范围\n");
        return;
    }
    
    // 查找需要重传的报文范围
    struct cached_packet *to_retransmit = find_packets_by_psn_range(key, nack_epsn, current_max_psn, &retransmit_count);
    
    if (retransmit_count > 0) {
        printf("容错重传: 准备重传 %d 个报文 (PSN %u - %u)\n",
               retransmit_count, nack_epsn, current_max_psn);
        
        trigger_batch_retransmit(to_retransmit, retransmit_count, key);
        
        // 释放报文内存（假设重传函数会复制数据）
        free_cached_packets(to_retransmit);
    } else {
        printf("容错重传: 没有找到需要重传的报文\n");
    }
}


// 增强的RDMA报文处理函数
void process_rdma_packet(const unsigned char *buffer, ssize_t length)
{
    // 1. 解析以太网、IP、UDP头部（同前）
    struct ethhdr *eth = (struct ethhdr*)buffer;
    if (ntohs(eth->h_proto) != ETH_P_IP) return;
    
    struct iphdr *ip = (struct iphdr*)(buffer + sizeof(struct ethhdr));
    int ip_header_len = ip->ihl * 4;
    if (ip->protocol != IPPROTO_UDP) return;
    
    struct udphdr *udp = (struct udphdr*)(buffer + sizeof(struct ethhdr) + ip_header_len);
    if (ntohs(udp->dest) != 4791) return;
    
    // 2. 计算BTH位置
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
    
    // 5. 根据报文类型分别处理
    switch (pkt_type) {
        case PKT_TYPE_DATA:
            process_data_packet(buffer, length, bth_offset, 
                               src_ip, dst_ip, src_port, dst_port,
                               pkey, dest_qp, psn, opcode);
            break;
            
        case PKT_TYPE_ACK:
            process_ack_packet(buffer, length, bth_offset,
                              src_ip, dst_ip, src_port, dst_port,
                              pkey, dest_qp, psn, opcode);
            break;
            
        case PKT_TYPE_NACK:
            process_nack_packet(buffer, length, bth_offset,
                               src_ip, dst_ip, src_port, dst_port,
                               pkey, dest_qp, psn, opcode);
            break;
            
        default:
            printf("未知报文类型: 操作码=0x%02x\n", opcode);
            break;
    }
}

void receive_and_parse_frames(int sockfd) {
    unsigned char buffer[2048];
    struct sockaddr_ll saddr;
    socklen_t saddr_len = sizeof(saddr);
    
    printf("开始捕获RDMA报文...\n");
    
    while (1) {
        ssize_t len = recvfrom(sockfd, buffer, sizeof(buffer), 0,
                              (struct sockaddr*)&saddr, &saddr_len);
        if (len < 0) {
            perror("recvfrom");
            continue;
        }
        
        if (len < (ssize_t)sizeof(struct ethhdr)) {
            printf("收到过短报文: %zd bytes\n", len);
            continue;
        }
        
        // 调用完整的解析函数
        process_rdma_packet(buffer, len);
    }
}

int main() {
    // 创建原始套接字并开启混杂模式
    int sockfd = create_promiscuous_socket("eth0");
    if (sockfd < 0) {
        fprintf(stderr, "无法创建原始套接字\n");
        return 1;
    }
    
    // 开始捕获和解析报文
    receive_and_parse_frames(sockfd);
    
    close(sockfd);
    return 0;
}

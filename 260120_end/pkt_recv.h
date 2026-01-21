#ifndef PKT_RECV_H
#define PKT_RECV_H

#include "pkt_cache.h"
#include "rdma_opcode.h"
#include <pthread.h>
#include <stdio.h>
#include <sys/types.h>

// ZPY
#include "gateway_msg.h"
// ZPY
/**** **** **** **** **** ****
 **** ****  启动函数  **** ****
 **** **** **** **** **** ****/

void *packet_receiver_thread(void *arg);

int start_packet_receiver(const char *interface);

int stop_packet_receiver();

int is_receiver_running();

/**** **** **** **** **** ****
 **** ****  流程函数  **** ****
 **** **** **** **** **** ****/

// 创建原始套接字并开启混杂模式
int create_promiscuous_socket(const char *interface_name);

// 捕获和解析报文
void receive_and_parse_frames(int sockfd);

// 增强的RDMA报文处理函数
void process_rdma_packet(const unsigned char *buffer, ssize_t length);

// RoCEv2 BTH 结构
struct bth {
    // 第1字节
    uint8_t opcode;              // 操作码 (8位)
                                 // 第2字节
    uint8_t solicited_event : 1; // Solicited Event (1位)
    uint8_t mig_req : 1;         // Migration Request (1位)
    uint8_t pad_count : 2;       // 填充计数 (2位)
    uint8_t version : 4;         // 传输版本 (4位)
                                 // 第3-5字节
    uint16_t pkey;               // 分区键 (16位)
    uint8_t reserved1;           // 保留字段 (8位)
                                 // 第6-8字节
    uint8_t dest_qp[3];          // 目标QP号(24位) 使用3字节数组存储24位值
                                 // 第9字节
    uint8_t ack_req : 1;         // ACK请求 (1位)
    uint8_t reserved2 : 7;       // 保留字段 (7位)
                                 // 第10-12字节
    uint8_t psn[3];              // 包序列号(24位) 使用3字节数组存储24位值
} __attribute__((packed));

// 增强的BTH解析，支持ACK/NACK
int parse_bth_header(const unsigned char *bth_start, uint8_t *opcode,
                     uint16_t *pkey, uint32_t *dest_qp, uint32_t *psn,
                     rdma_packet_type_t *pkt_type);

// 处理数据报文
void process_data_packet(const unsigned char *buffer, ssize_t length,
                         int bth_offset, struct connection_bucket *bucket,
                         struct connection_key key, uint32_t psn);
// void process_data_packet(const unsigned char *buffer, ssize_t length,
//                         int bth_offset,
//                         const char *src_ip, const char *dst_ip,
//                         uint16_t src_port, uint16_t dst_port,
//                         uint16_t pkey, uint32_t dest_qp, uint32_t psn,
//                         uint8_t opcode);

// RoCEv2 AETH 结构
struct aeth {
    uint8_t syndrome; // 综合征（包含MSN和代码）
    uint8_t MSN[3];   // 信用值（24位）
} __attribute__((packed));

// RDMA服务类型获取
// uint8_t infer_service_type(uint8_t opcode);

// 处理ACK报文
// void process_ack_packet(const unsigned char *buffer, ssize_t length,
//                        int bth_offset,
//                        const char *src_ip, const char *dst_ip,
//                        uint16_t src_port, uint16_t dst_port,
//                        uint16_t pkey, uint32_t dest_qp, uint32_t psn,
//                        uint8_t opcode);

// ZPY
// 添加参数gateway_role
void process_ack_packet(const unsigned char *buffer, ssize_t length,
                        int bth_offset, struct connection_bucket *bucket,
                        struct connection_key key, uint32_t epsn,
                        enum gateway_role role);

// 处理NACK报文
// void process_nack_packet(const unsigned char *buffer, ssize_t length,
//                         int bth_offset,
//                         const char *src_ip, const char *dst_ip,
//                         uint16_t src_port, uint16_t dst_port,
//                         uint16_t pkey, uint32_t dest_qp, uint32_t psn,
//                         uint8_t opcode);

// 解析AETH头部（ACK/NACK专用）
int parse_aeth_header(const unsigned char *aeth_start, uint8_t *syndrome,
                      uint32_t *epsn);

// 处理ACK接收
// void handle_ack_received(const char *src_ip, const char *dst_ip,
//                         uint16_t src_port, uint16_t dst_port,
//                         uint16_t pkey, uint32_t ack_epsn);
// ZPY
// 处理ACK接收
void handle_ack_received(const unsigned char *buffer, ssize_t length,
                         struct connection_bucket *bucket,
                         struct connection_key key, uint32_t epsn,
                         enum gateway_role role);

// ZPY
int clean_nacked_packets(struct connection_cache_array *conn,
                         uint32_t nack_psn);
// ZPY
// 处理NACK接收
// void handle_nack_received(const char *src_ip, const char *dst_ip,
//                          uint16_t src_port, uint16_t dst_port,
//                          uint16_t pkey, uint32_t nack_epsn, uint8_t
//                          syndrome);

void handle_nack_received(const unsigned char *buffer, ssize_t length,
                          struct connection_bucket *bucket,
                          struct connection_key key, uint32_t epsn,
                          enum gateway_role role);

// 配置对端信息
void set_sr_client_target_info(const char *ip, int port);
// 发送SR重传请求
int get_sr_client_fd();

void close_sr_client_fd();

void close_retransmit_sockfd();

int get_retransmit_sockfd();
// ZPY
// 填充网关消息
void set_gateway_msg(struct connection_key key,
                     struct connection_cache_array *conn,
                     lost_segment *ls_sgments,
                     gateway_control_msg *gw_control_msg,
                     gateway_data_msg *gw_data_msg, uint32_t seg_num,
                     size_t msg_size);

// ZPY
// SR重传线程处理函数
void handle_sr_requests();

// ZPY
// 发送SR重传请求到源网关
void send_sr_request_to_src_gateway(struct connection_key key,
                                    struct connection_cache_array *conn);

// ZPY
// SR重传线程函数
void *sr_retransmit_worker(void *arg);

// ZPY
// 启动SR重传线程
int start_retransmit_thread(int port);

// ZPY
int is_retransmit_running();
// ZPY
// 资源回收函数
int stop_retransmit_thread();

// ZPY
// 处理网关消息
void handle_gateway_msg(gateway_control_msg *gw_control_msg,
                        gateway_data_msg *gw_data_msg,
                        const char *interface_name);

// ZPY
// 向源主机发送NAK
void send_nak_to_source_host(const unsigned char *pack_data, int length,
                             const char *interface_name, uint32_t nack_epsn,
                             uint32_t dest_qp);

// ZPY
// 重传epsn后最新一个缓存的数据包
void retransmit_latest_one_packet(struct connection_cache_array *conn,
                                  char *interface_name, uint32_t epsn,
                                  uint32_t src_qp);

// ZPY
// GBN重传
int dst_gateway_gbn_retransmit(struct connection_cache_array *conn,
                               char *interface_name, uint32_t nack_epsn,
                               uint32_t src_qp);

// ZPY
// 重传单个数据包
void retransmit_rdma_packet(const unsigned char *packet_data, int length,
                            const char *interface_name, uint32_t psn,
                            uint32_t dest_qp);

// ZPY
/**** **** **** **** **** ****
 **** ****  辅助函数  **** ****
 **** **** **** **** **** ****/
// ZPY
// 判断丢包函数
int is_packet_lost(struct connection_cache_array *conn, uint32_t psn);

// ZPY
//  计算丢包位图
lost_segment *calculate_bitmap_loss(struct connection_cache_array *conn,
                                    uint32_t *seg_num);

// ZPY
// 获取网关的MAC地址
int get_gateway_mac(const char *ifname, unsigned char *mac_addr);

// ZPY
// 计算PSN差值
uint32_t calculate_psn_number(uint32_t start_psn, uint32_t end_psn);

// 从BTH中提取24位字段
static inline uint32_t get_24bit_value(const uint8_t *data) {
    return (data[0] << 16) | (data[1] << 8) | data[2];
}

#endif //  PKT_RECV_H
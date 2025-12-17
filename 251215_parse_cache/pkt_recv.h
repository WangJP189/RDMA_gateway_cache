#ifndef PKT_RECV_H
#define PKT_RECV_H

#include <stdio.h>
#include <pthread.h>
#include "rdma_opcode.h"

                /**** **** **** **** **** ****
                 **** ****  启动函数  **** ****
                 **** **** **** **** **** ****/

void* packet_receiver_thread(void* arg);

int start_packet_receiver(const char* interface);

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
    uint8_t solicited_event:1;   // Solicited Event (1位)
    uint8_t mig_req:1;           // Migration Request (1位)  
    uint8_t pad_count:2;         // 填充计数 (2位)
    uint8_t version:4;           // 传输版本 (4位)
        // 第3-5字节
    uint16_t pkey;               // 分区键 (16位)
    uint8_t reserved1;           // 保留字段 (8位)
        // 第6-8字节
    uint8_t dest_qp[3];          // 目标QP号(24位) 使用3字节数组存储24位值
        // 第9字节
    uint8_t ack_req:1;           // ACK请求 (1位)
    uint8_t reserved2:7;         // 保留字段 (7位)
        // 第10-12字节
    uint8_t psn[3];              // 包序列号(24位) 使用3字节数组存储24位值
} __attribute__((packed));

// 增强的BTH解析，支持ACK/NACK
int parse_bth_header(const unsigned char *bth_start, 
                     uint8_t *opcode, uint16_t *pkey,
                     uint32_t *dest_qp, uint32_t *psn,
                     rdma_packet_type_t *pkt_type);

// 处理数据报文
void process_data_packet(const unsigned char *buffer, ssize_t length,
                        int bth_offset,
                        const char *src_ip, const char *dst_ip,
                        uint16_t src_port, uint16_t dst_port,
                        uint16_t pkey, uint32_t dest_qp, uint32_t psn,
                        uint8_t opcode);

// RoCEv2 AETH 结构
struct aeth {
    uint8_t syndrome;        // 综合征（包含MSN和代码）
    uint8_t MSN[3];       // 信用值（24位）
} __attribute__((packed));

// RDMA服务类型获取
uint8_t infer_service_type(uint8_t opcode);

// 处理ACK报文
void process_ack_packet(const unsigned char *buffer, ssize_t length,
                       int bth_offset,
                       const char *src_ip, const char *dst_ip,
                       uint16_t src_port, uint16_t dst_port,
                       uint16_t pkey, uint32_t dest_qp, uint32_t psn,
                       uint8_t opcode);

// 处理NACK报文
void process_nack_packet(const unsigned char *buffer, ssize_t length,
                        int bth_offset,
                        const char *src_ip, const char *dst_ip,
                        uint16_t src_port, uint16_t dst_port,
                        uint16_t pkey, uint32_t dest_qp, uint32_t psn,
                        uint8_t opcode);

// 解析AETH头部（ACK/NACK专用）
int parse_aeth_header(const unsigned char *aeth_start,
                     uint8_t *syndrome, uint32_t *epsn);
    
// 处理ACK接收
void handle_ack_received(const char *src_ip, const char *dst_ip,
                        uint16_t src_port, uint16_t dst_port,
                        uint16_t pkey, uint32_t ack_epsn);

// 处理NACK接收
void handle_nack_received(const char *src_ip, const char *dst_ip,
                         uint16_t src_port, uint16_t dst_port,
                         uint16_t pkey, uint32_t nack_epsn, uint8_t syndrome);

                /**** **** **** **** **** ****
                 **** ****  辅助函数  **** ****
                 **** **** **** **** **** ****/

// 从BTH中提取24位字段
static inline uint32_t get_24bit_value(const uint8_t *data) {
    return (data[0] << 16) | (data[1] << 8) | data[2];
}

#endif  //  PKT_RECV_H
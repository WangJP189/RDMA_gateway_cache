#ifndef GATEWAT_MSG_H
#define GATEWAT_MSG_H
// 计算可分配的丢包段数组的最大值
// 1400-1-4-4-2-2-4-4-2-4-4=1369
#define MAX_LOST_SEGMENTS 170
// 丢包段结构体
typedef struct{
    uint32_t start_psn;  // 起始PSN
    uint32_t loss_num;   // 连续丢包数量
}lost_segment;

// 源目的网关消息交互结构体
// 请求控制块
// 交换后的源目的IP，源目的端口，源目的qp
typedef struct{
    uint8_t message_type;   // 消息类型 0表示控制请求
    uint32_t src_ip; // 源网关IP
    uint32_t dest_ip; // 目的网关IP
    uint16_t src_port; // 源网关端口
    uint16_t dest_port; // 目的网关端口
    uint32_t src_qp;         // 源QP号
    uint32_t dest_qp;        // 目的QP号
    uint16_t pkey;          // PKey
}gateway_control_msg;
// 请求数据块
typedef struct{
    uint32_t total_data_length; // 总数据长度
    uint32_t seg_num;           // 丢包段数量
    lost_segment lost_segments[]; // 可变数量的丢包段数组
}gateway_data_msg;

#endif
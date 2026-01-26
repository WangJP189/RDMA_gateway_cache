#ifndef CONFIG_H
#define CONFIG_H

// 报文接收缓冲区相关 rte_pktmbuf_pool_create
#define NUM_MBUFS 8191      // 内存池中元素数量
#define MBUF_CACHE_SIZE 512 // 每个CPU核心的本地缓存大小
#define MBUF_NODE_SIZE 5120 // 每个 mbuf 数据缓冲区的大小（字节）

// 网卡接收，发送缓冲区长度
#define NIC_RX_RING_SIZE 1024
#define NIC_TX_RING_SIZE 1024

// 报文缓存RING BUF长度
#define USR_RING_SIZE 4096
#define BURST_SIZE 128 // 收包线程每次最大读取报文个数

#define NUM_BUCKETS 1024
#define MAX_PSN_ARRAY 9216 // PSN指针数组大小

#define ERR (-1)
#define OK (0)

// RoCEv2 操作码定义
#define ROCE_OPCODE_RC_SEND_FIRST 0x00
#define ROCE_OPCODE_RC_SEND_MIDDLE 0x01
#define ROCE_OPCODE_RC_SEND_LAST 0x02
#define ROCE_OPCODE_RC_SEND_ONLY 0x03
#define ROCE_OPCODE_RC_RDMA_WRITE_FIRST 0x04
#define ROCE_OPCODE_RC_RDMA_WRITE_MIDDLE 0x05
#define ROCE_OPCODE_RC_RDMA_WRITE_LAST 0x06
#define ROCE_OPCODE_RC_RDMA_WRITE_ONLY 0x07
#define ROCE_OPCODE_RC_RDMA_READ_REQUEST 0x08
#define ROCE_OPCODE_RC_RDMA_READ_RESPONSE_FIRST 0x09
#define ROCE_OPCODE_RC_RDMA_READ_RESPONSE_MIDDLE 0x0A
#define ROCE_OPCODE_RC_RDMA_READ_RESPONSE_LAST 0x0B
#define ROCE_OPCODE_RC_RDMA_READ_RESPONSE_ONLY 0x0C
#define ROCE_OPCODE_RC_ACK 0x60
#define ROCE_OPCODE_RC_ATOMIC_ACK 0x61
#define ROCE_OPCODE_RC_NAK 0x80

#define ROCE_V2_PORT 4791

// AETH类型定义
#define AETH_TYPE_ACK 0x00 // 000xxxxx -> ACK
#define AETH_TYPE_RNR 0x01 // 001xxxxx -> RNR
#define AETH_TYPE_NAK 0x03 // 011xxxxx -> NAK

// NAK代码定义
#define NAK_CODE_SEQ_ERR 0x00 // 序列错误
#define NAK_CODE_INV_REQ 0x01 // 无效请求
#define NAK_CODE_RMT_ACC 0x02 // 远程访问错误
#define NAK_CODE_RMT_OP 0x03  // 远程操作错误
#define NAK_CODE_INV_RD 0x04  // 无效RD请求
#define NAK_CODE_RNR 0x05     // RNR NAK
#define NAK_CODE_NAK 0x06     // NAK  其他未分类的错误
#define NAK_CODE_TIMEOUT 0x07 // 超时

// 宏定义
#define PSN_MASK 0x00FFFFFF     // 24位PSN掩码
#define PSN_HALF_CYCLE 0x800000 // 24位PSN的半周期
#define PSN_MAX_VALUE PSN_MASK  // PSN最大值
#define PSN_INVALID 0x1000000   // 无效PSN
#define PKEY_DEFAULT 0xFFFF     // 默认分区键

// 定时延时宏定义(毫秒)
#define RNR_TIMEOUT 1000            // RNR超时时间（毫秒）
#define DELAY_NAK_INTERVAL 5        // NAK延时处理时间（毫秒）
#define SESSION_AGING_INTERVAL 100  // 100ms老化周期
#define AGING_INTERVAL 5000         // 5秒老化周期
#define RETRANSMIT_INTERVAL 100     // 100ms重传检查
#define THREAD_CHECK_INTERVAL 30000 // 30秒

#endif // CONFIG_H
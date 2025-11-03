#ifndef RDMA_OPCODES_H
#define RDMA_OPCODES_H

/* 报文类型定义 */
typedef enum {
    PKT_TYPE_DATA,          // 数据报文
    PKT_TYPE_READ_RESPONSE, // 读响应报文
    PKT_TYPE_ACK,           // 确认报文
    PKT_TYPE_NACK,          // 否定确认报文
    PKT_TYPE_CONTROL,       // 控制报文
    PKT_TYPE_UNKNOWN        // 未知报文
} rdma_packet_type_t;

/* RC服务类型的RDMA操作码枚举 */
typedef enum {
    /* 发送操作 (Send Operations) - 0x00-0x05 */
    RDMA_OP_SEND_FIRST            = 0x00, /**< 多报文发送的第一个报文 */
    RDMA_OP_SEND_MIDDLE           = 0x01, /**< 多报文发送的中间报文 */
    RDMA_OP_SEND_LAST             = 0x02, /**< 多报文发送的最后一个报文 */
    RDMA_OP_SEND_LAST_IMM         = 0x03, /**< 带立即数的最后一个发送报文 */
    RDMA_OP_SEND_ONLY             = 0x04, /**< 单报文发送 */
    RDMA_OP_SEND_ONLY_IMM         = 0x05, /**< 带立即数的单报文发送 */

    /* RDMA写操作 (RDMA Write Operations) - 0x06-0x0B */
    RDMA_OP_WRITE_FIRST      = 0x06, /**< 多报文RDMA写的第一个报文 */
    RDMA_OP_WRITE_MIDDLE     = 0x07, /**< 多报文RDMA写的中间报文 */
    RDMA_OP_WRITE_LAST       = 0x08, /**< 多报文RDMA写的最后一个报文 */
    RDMA_OP_WRITE_LAST_IMM   = 0x09, /**< 带立即数的最后一个RDMA写报文 */
    RDMA_OP_WRITE_ONLY       = 0x0A, /**< 单报文RDMA写 */
    RDMA_OP_WRITE_ONLY_IMM   = 0x0B, /**< 带立即数的单报文RDMA写 */

    /* RDMA读操作 (RDMA Read Operations) - 0x0C */
    RDMA_OP_READ_REQUEST     = 0x0C, /**< RDMA读请求 */

    /* 原子操作 (Atomic Operations) - 0x0D-0x0E */
    RDMA_OP_ATOMIC_CMP_AND_SWP    = 0x0D, /**< 原子比较与交换 */
    RDMA_OP_ATOMIC_FETCH_AND_ADD  = 0x0E, /**< 原子获取与相加 */

    /* 确认与响应 (Acknowledgements & Responses) - 0x10-0x15 */
    RDMA_OP_ACK                   = 0x10, /**< 普通确认 */
    RDMA_OP_ATOMIC_ACK            = 0x11, /**< 原子操作确认 */
    RDMA_OP_READ_RESPONSE_FIRST  = 0x12, /**< RDMA读响应的第一个报文 */
    RDMA_OP_READ_RESPONSE_MIDDLE = 0x13, /**< RDMA读响应的中间报文 */
    RDMA_OP_READ_RESPONSE_LAST   = 0x14, /**< RDMA读响应的最后一个报文 */
    RDMA_OP_READ_RESPONSE_ONLY   = 0x15, /**< 单报文RDMA读响应 */

    /* 否定确认与错误 (Negative Acknowledgements & Errors) - 0x16-0x17 */
    RDMA_OP_RNR_NAK               = 0x16, /**< 接收端未就绪NAK */
    RDMA_OP_NAK_INVALID           = 0x17, /**< 无效请求NAK */

    /* 终止操作 (Terminate) - 0x18 */
    RDMA_OP_TERMINATE             = 0x18  /**< 终止连接 */

} rdma_opcode_rc_t;

/* 操作码分类宏 */
#define RDMA_OP_IS_SEND(op) \
    ((op) >= RDMA_OP_SEND_FIRST && (op) <= RDMA_OP_SEND_ONLY_IMM)

#define RDMA_OP_IS_WRITE(op) \
    ((op) >= RDMA_OP_WRITE_FIRST && (op) <= RDMA_OP_WRITE_ONLY_IMM)

#define RDMA_OP_IS_READ_REQUEST(op) \
    ((op) == RDMA_OP_READ_REQUEST)

#define RDMA_OP_IS_READ_RESPONSE(op) \
    ((op) >= RDMA_OP_READ_RESPONSE_FIRST && (op) <= RDMA_OP_READ_RESPONSE_ONLY)

#define RDMA_OP_IS_ATOMIC_REQUEST(op) \
    ((op) == RDMA_OP_ATOMIC_CMP_AND_SWP || (op) == RDMA_OP_ATOMIC_FETCH_AND_ADD)

#define RDMA_OP_IS_ACK(op) \
    ((op) == RDMA_OP_ACK || (op) == RDMA_OP_ATOMIC_ACK)

#define RDMA_OP_IS_NAK(op) \
    ((op) == RDMA_OP_RNR_NAK || (op) == RDMA_OP_NAK_INVALID)

#define RDMA_OP_IS_REQUEST(op) \
    (RDMA_OP_IS_SEND(op) || RDMA_OP_IS_WRITE(op) || \
     RDMA_OP_IS_READ_REQUEST(op) || RDMA_OP_IS_ATOMIC_REQUEST(op))

#define RDMA_OP_IS_RESPONSE(op) \
    (RDMA_OP_IS_READ_RESPONSE(op) || RDMA_OP_IS_ACK(op))

rdma_packet_type_t get_packet_type_rc(uint8_t opcode);

#endif // RDMA_OPCODES_H
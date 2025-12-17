#include "rdma_opcode.h"

/**
 * 根据RC服务类型的操作码判断报文类型
 * @param opcode 操作码
 * @return 报文类型
 */
rdma_packet_type_t get_packet_type_rc(uint8_t opcode)
{
    /* 使用分类宏进行初步判断，提高代码可读性和可维护性 */
    
    // if (RDMA_OP_IS_SEND(opcode) || 
    //     RDMA_OP_IS_WRITE(opcode) || 
    //     RDMA_OP_IS_READ_REQUEST(opcode) ||
    //     RDMA_OP_IS_ATOMIC_REQUEST(opcode)) {
    //     return PKT_TYPE_DATA;  // 数据报文（请求）
    // }

    if (RDMA_OP_IS_REQUEST(opcode)) {
        return PKT_TYPE_DATA;  // 数据报文（请求）
        //return PKT_TYPE_REQUEST;    // 请求报文
    }
    
    if (RDMA_OP_IS_READ_RESPONSE(opcode)) {
        return PKT_TYPE_READ_RESPONSE;  // 读响应报文
    }
    /*
    if (RDMA_OP_IS_ACK(opcode)) {
        return PKT_TYPE_ACK;  // 确认报文
    }
    */
    if (RDMA_OP_IS_ACK(opcode)) {
        // ToDo AETH parse
        return PKT_TYPE_ACK;  // 确认报文
    }

    if (RDMA_OP_IS_NAK(opcode)) {
        return PKT_TYPE_NACK;  // 否定确认报文
    }
    
    /* 特殊控制报文 */
    if (opcode == RDMA_OP_TERMINATE) {
        return PKT_TYPE_CONTROL;  // 控制报文
    }
    
    return PKT_TYPE_UNKNOWN;  // 未知报文
}

#include <stdint.h>
#include "rdma_opcode.h"

/**
 * 根据RC服务类型的操作码判断报文类型
 * @param opcode 操作码
 * @return 报文类型
 */
rdma_packet_type_t get_packet_type_rc(uint8_t opcode)
{
    /* 使用分类宏进行初步判断，提高代码可读性和可维护性 */
    if (RDMA_OP_IS_SEND(opcode) || 
        RDMA_OP_IS_WRITE(opcode) || 
        RDMA_OP_IS_READ_REQUEST(opcode) ||
        RDMA_OP_IS_ATOMIC_REQUEST(opcode)) {
        return PKT_TYPE_DATA;  // 数据报文（请求）
    }
    
    if (RDMA_OP_IS_READ_RESPONSE(opcode)) {
        return PKT_TYPE_READ_RESPONSE;  // 读响应报文
    }
    
    if (RDMA_OP_IS_ACK(opcode)) {
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

/**
 * 将操作码转换为字符串描述（用于调试和日志）
 * @param opcode 操作码
 * @return 字符串描述
 */
static inline const char* rdma_opcode_rc_to_string(rdma_opcode_rc_t opcode) {
    switch(opcode) {
        /* 发送操作 */
        case RDMA_OP_SEND_FIRST:      return "SEND_FIRST";
        case RDMA_OP_SEND_MIDDLE:     return "SEND_MIDDLE";
        case RDMA_OP_SEND_LAST:       return "SEND_LAST";
        case RDMA_OP_SEND_LAST_IMM:   return "SEND_LAST_IMM";
        case RDMA_OP_SEND_ONLY:       return "SEND_ONLY";
        case RDMA_OP_SEND_ONLY_IMM:   return "SEND_ONLY_IMM";
            
        /* RDMA写操作 */
        case RDMA_OP_RDMA_WRITE_FIRST:    return "RDMA_WRITE_FIRST";
        case RDMA_OP_RDMA_WRITE_MIDDLE:   return "RDMA_WRITE_MIDDLE";
        case RDMA_OP_RDMA_WRITE_LAST:     return "RDMA_WRITE_LAST";
        case RDMA_OP_RDMA_WRITE_LAST_IMM: return "RDMA_WRITE_LAST_IMM";
        case RDMA_OP_RDMA_WRITE_ONLY:     return "RDMA_WRITE_ONLY";
        case RDMA_OP_RDMA_WRITE_ONLY_IMM: return "RDMA_WRITE_ONLY_IMM";
            
        /* RDMA读操作 */
        case RDMA_OP_READ_REQUEST: return "READ_REQUEST";
            
        /* 原子操作 */
        case RDMA_OP_ATOMIC_CMP_AND_SWP:   return "ATOMIC_CMP_AND_SWP";
        case RDMA_OP_ATOMIC_FETCH_AND_ADD: return "ATOMIC_FETCH_AND_ADD";
            
        /* 确认与响应 */
        case RDMA_OP_ACK:                       return "ACK";
        case RDMA_OP_ATOMIC_ACK:                return "ATOMIC_ACK";
        case RDMA_OP_READ_RESPONSE_FIRST:  return "READ_RESPONSE_FIRST";
        case RDMA_OP_READ_RESPONSE_MIDDLE: return "READ_RESPONSE_MIDDLE";
        case RDMA_OP_READ_RESPONSE_LAST:   return "READ_RESPONSE_LAST";
        case RDMA_OP_READ_RESPONSE_ONLY:   return "READ_RESPONSE_ONLY";
            
        /* 否定确认 */
        case RDMA_OP_RNR_NAK:     return "RNR_NAK";
        case RDMA_OP_NAK_INVALID: return "NAK_INVALID";
            
        /* 终止操作 */
        case RDMA_OP_TERMINATE: return "TERMINATE";
            
        default: return "UNKNOWN_OPCODE";
    }
}

/**
 * 检查操作码是否为请求类型（需要响应）
 * @param opcode 操作码
 * @return 1-是请求，0-不是请求
 */
static inline int rdma_opcode_is_request(uint8_t opcode)
{
    return RDMA_OP_IS_REQUEST(opcode);
}

/**
 * 检查操作码是否为响应类型
 * @param opcode 操作码
 * @return 1-是响应，0-不是响应
 */
static inline int rdma_opcode_is_response(uint8_t opcode)
{
    return RDMA_OP_IS_RESPONSE(opcode) || RDMA_OP_IS_NAK(opcode);
}

/**
 * 获取操作码的详细描述信息
 * @param opcode 操作码
 * @return 描述字符串
 */
static inline const char* rdma_opcode_get_description(rdma_opcode_rc_t opcode)
{
    switch(opcode) {
        case RDMA_OP_SEND_FIRST:      return "First packet of multi-packet Send";
        case RDMA_OP_SEND_MIDDLE:     return "Middle packet of multi-packet Send";
        case RDMA_OP_SEND_LAST:       return "Last packet of multi-packet Send";
        case RDMA_OP_SEND_LAST_IMM:   return "Last packet of Send with immediate data";
        case RDMA_OP_SEND_ONLY:       return "Single packet Send";
        case RDMA_OP_SEND_ONLY_IMM:   return "Single packet Send with immediate data";
            
        case RDMA_OP_RDMA_WRITE_FIRST:    return "First packet of multi-packet RDMA Write";
        case RDMA_OP_RDMA_WRITE_MIDDLE:   return "Middle packet of multi-packet RDMA Write";
        case RDMA_OP_RDMA_WRITE_LAST:     return "Last packet of multi-packet RDMA Write";
        case RDMA_OP_RDMA_WRITE_LAST_IMM: return "Last packet of RDMA Write with immediate data";
        case RDMA_OP_RDMA_WRITE_ONLY:     return "Single packet RDMA Write";
        case RDMA_OP_RDMA_WRITE_ONLY_IMM: return "Single packet RDMA Write with immediate data";
            
        case RDMA_OP_READ_REQUEST: return "RDMA Read request";
            
        case RDMA_OP_ATOMIC_CMP_AND_SWP:   return "Atomic Compare and Swap";
        case RDMA_OP_ATOMIC_FETCH_AND_ADD: return "Atomic Fetch and Add";
            
        case RDMA_OP_ACK:                       return "Acknowledgment";
        case RDMA_OP_ATOMIC_ACK:                return "Atomic operation acknowledgment";
        case RDMA_OP_READ_RESPONSE_FIRST:  return "First packet of RDMA Read response";
        case RDMA_OP_READ_RESPONSE_MIDDLE: return "Middle packet of RDMA Read response";
        case RDMA_OP_READ_RESPONSE_LAST:   return "Last packet of RDMA Read response";
        case RDMA_OP_READ_RESPONSE_ONLY:   return "Single packet RDMA Read response";
            
        case RDMA_OP_RNR_NAK:     return "Receiver Not Ready negative acknowledgment";
        case RDMA_OP_NAK_INVALID: return "Invalid request negative acknowledgment";
            
        case RDMA_OP_TERMINATE: return "Connection termination";
            
        default: return "Unknown operation code";
    }
}

// 根据操作码判断是否需要AETH头
int is_aeth_expected(uint8_t opcode) {
    switch (opcode) {
        // ACK和原子操作ACK总是包含AETH
        case RDMA_OP_ACK:
        case RDMA_OP_ATOMIC_ACK:
            return 1;
            
        // 读响应操作包含AETH
        case RDMA_OP_READ_RESPONSE_FIRST:
        case RDMA_OP_READ_RESPONSE_MIDDLE:
        case RDMA_OP_READ_RESPONSE_LAST:
        case RDMA_OP_READ_RESPONSE_ONLY:
            return 1;
            
        // NAK报文包含AETH
        case RDMA_OP_RNR_NAK:
        case RDMA_OP_NAK_INVALID:
            return 1;
            
        // 其他操作通常不包含AETH
        default:
            return 0;
    }
}

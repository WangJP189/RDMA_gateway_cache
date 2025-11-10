#ifndef PKT_RECV_INTERNAL_H
#define PKT_RECV_INTERNAL_H

#include <stdint.h>

// ACK 处理
void handle_ack_received(const char *src_ip, const char *dst_ip,
                         uint16_t src_port, uint16_t dst_port,
                         uint16_t pkey, uint32_t ack_epsn);

// NACK 处理（注意包含 syndrome 参数）
void handle_nack_received(const char *src_ip, const char *dst_ip,
                          uint16_t src_port, uint16_t dst_port,
                          uint16_t pkey, uint32_t nack_epsn, uint8_t syndrome);

// 获取 NACK 错误描述（调试）
const char* get_nack_error_description(uint8_t syndrome);

#endif // PKT_RECV_INTERNAL_H
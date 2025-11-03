#ifndef PKT_RECV_INTERNAL_H
#define PKT_RECV_INTERNAL_H

// 函数声明
void handle_ack_received(const char *src_ip, const char *dst_ip,
                        uint16_t src_port, uint16_t dst_port,
                        uint16_t pkey, uint32_t ack_epsn);

// 函数声明
void handle_nack_received(const char *src_ip, const char *dst_ip,
                        uint16_t src_port, uint16_t dst_port,
                        uint16_t pkey, uint32_t ack_epsn)

// 函数声明
const char* get_nack_error_description(uint8_t syndrome);

#endif // PKT_RECV_INTERNAL_H
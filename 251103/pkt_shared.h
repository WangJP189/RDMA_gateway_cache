// 共享结构与辅助声明（用于 stub 与编译测试）
#ifndef PKT_SHARED_H
#define PKT_SHARED_H

#include <stdint.h>

struct connection_key {
    uint32_t src_ip;
    uint32_t dst_ip;
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t dest_qp;
    uint32_t src_qp;
    uint8_t service_type;
    uint16_t pkey;
};

struct cached_packet {
    unsigned char *app_data;
    int data_len;
    uint32_t dest_qp;
    uint32_t psn;
    struct cached_packet *next;
    struct cached_packet *prev;
};

struct aeth {
    uint8_t syndrome;
    uint32_t credit; // 24-bit used
} __attribute__((packed));

// 外部函数原型（在工程中其它文件会调用这些）
void retransmit_rdma_packet(const struct connection_key *key,
                            const unsigned char *data, int len,
                            uint32_t dest_qp, uint32_t psn);

void free_cached_packets(struct cached_packet *head);

uint8_t get_aeth_syndrome(const struct aeth *a);
uint32_t get_aeth_epsn(const struct aeth *a);

#endif // PKT_SHARED_H

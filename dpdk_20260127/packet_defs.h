#ifndef PACKET_DEFS_H
#define PACKET_DEFS_H

#include <dpdk/rte_ether.h>
#include <dpdk/rte_ip.h>
#include <dpdk/rte_udp.h>

// RoCEv2 BTH 结构
struct ib_bth {
    uint8_t opcode;
    uint8_t solicited_event:1;
    uint8_t mig_req:1;
    uint8_t pad_count:2;
    uint8_t version:4;
    uint16_t pkey;
    uint8_t reserved1;
    uint8_t dest_qp[3];
    uint8_t ack_req:1;
    uint8_t reserved2:7;
    uint8_t psn[3];
} __rte_packed;

// RoCEv2 AETH 结构
struct ib_aeth {
    uint8_t syndrome;
    uint8_t msn[3];
} __rte_packed;

// RoCEv2 数据报文结构
struct rocev2_packet {
    struct rte_ether_hdr eth_hdr;
    struct rte_ipv4_hdr ip_hdr;
    struct rte_udp_hdr udp_hdr;
    struct ib_bth bth;
    uint8_t payload[0];
} __rte_packed;

// RoCEv2 ACK/NAK报文结构
struct rocev2_packet_ack {
    struct rte_ether_hdr eth_hdr;
    struct rte_ipv4_hdr ip_hdr;
    struct rte_udp_hdr udp_hdr;
    struct ib_bth bth;
    struct ib_aeth aeth;
} __rte_packed;

#endif // PACKET_DEFS_H
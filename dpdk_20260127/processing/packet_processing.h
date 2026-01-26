#ifndef PACKET_PROCESSING_H
#define PACKET_PROCESSING_H

#include "../global.h"

// 报文处理函数
bool extract_and_validate_headers(struct rte_mbuf *mbuf,
                                  struct rte_ether_hdr **eth_hdr,
                                  struct rte_ipv4_hdr **ip_hdr,
                                  struct rte_udp_hdr **udp_hdr,
                                  struct ib_bth **bth);
int build_data_key_v4(struct cache_key_v4 *key, struct rte_ipv4_hdr *ip_hdr,
                      struct ib_bth *bth);
int build_control_key_v4(struct cache_key_v4 *key, struct rte_ipv4_hdr *ip_hdr,
                         struct ib_bth *bth, enum gateway_role *role);
void process_data_packet(struct global_data *gd, struct cache_key_v4 *key,
                         struct rte_mbuf *mbuf);
void process_control_packet(struct global_data *gd, struct cache_key_v4 *key,
                            struct rte_mbuf *mbuf, struct ib_bth *bth,
                            enum gateway_role *role);
void process_aeth_packet(struct global_data *gd, struct cache_key_v4 *key,
                         struct rte_mbuf *mbuf, struct ib_bth *bth,
                         enum gateway_role *role);

// 重传处理
void gbn_retrans_v4(struct global_data *gd, struct cache_entry_v4 *entry,
                    uint32_t nak_psn);
void dst_gateway_nak_v4(struct global_data *gd, struct cache_entry_v4 *entry,
                        uint32_t nak_psn);
void send_sr_request_v4(struct global_data *gd, struct cache_entry_v4 *entry,
                        uint32_t nak_psn);
void src_gateway_nak_v4(struct global_data *gd, struct cache_entry_v4 *entry,
                        uint32_t nak_psn);
uint32_t build_gbn_retrans(struct cache_entry_v4 *entry,
                           struct rte_mbuf **tx_burst, uint16_t max_burst,
                           uint32_t nak_psn);

#endif // PACKET_PROCESSING_H
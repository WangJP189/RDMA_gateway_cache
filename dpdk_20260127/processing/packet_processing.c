#include "processing/packet_processing.h"
#include "global.h"
#include "tables/connection_table.h"
#include "utils/utils.h"
#include <rte_ip.h>
#include <rte_udp.h>
#include <stdio.h>
#include <string.h>

// 流表查找函数
// TODO:
// 由王腾超补充人工设置相应代码，流表操作一个独立文件/tabkes/flow_tables_v4.c/h
// TODO: main中人工设置代码独立文件
struct flow_entry_v4 *lookup_flow_v4(struct cache_key_v4 *key) {
    // TODO: 实现完整的IPv4流表查找逻辑
    // 这里返回一个简化的示例结构
    static struct flow_entry_v4 dummy_entry = {0};
    dummy_entry.src_qp = 12345;     // 示例源QP
    dummy_entry.role = DST_GATEWAY; // 示例角色

    if (key->src_ip && key->dst_ip) {
        return &dummy_entry;
    }

    return NULL;
}

/* 提取并验证各层协议头 */
bool extract_and_validate_headers(struct rte_mbuf *mbuf,
                                  struct rte_ether_hdr **eth_hdr,
                                  struct rte_ipv4_hdr **ip_hdr,
                                  struct rte_udp_hdr **udp_hdr,
                                  struct ib_bth **bth) {
    *eth_hdr = rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr *);

    // 检查报文长度是否足够
    if (rte_pktmbuf_data_len(mbuf) < sizeof(struct rte_ether_hdr)) {
        printf("Packet too short for Ethernet header\n");
        return false;
    }

    // 检查是否为IPv4报文
    if ((*eth_hdr)->ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4)) {
        // 不是IPv4报文，可能是ARP或其他类型
        printf("Not an IPv4 packet (ether_type: 0x%04X)\n",
               rte_be_to_cpu_16((*eth_hdr)->ether_type));
        return false;
    }

    // 检查IPv4头长度
    if (rte_pktmbuf_data_len(mbuf) <
        sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr)) {
        printf("Packet too short for IP header\n");
        return false;
    }

    *ip_hdr = (struct rte_ipv4_hdr *)(*eth_hdr + 1);

    // 验证IP头版本和长度
    if ((*ip_hdr)->version_ihl >> 4 != 4) {
        printf("Not IPv4 packet (version: %u)\n", (*ip_hdr)->version_ihl >> 4);
        return false;
    }

    // 检查是否为UDP报文
    if ((*ip_hdr)->next_proto_id != IPPROTO_UDP) {
        printf("Not UDP packet (proto: %u)\n", (*ip_hdr)->next_proto_id);
        return false;
    }

    // 计算UDP头位置
    uint8_t ip_header_len = ((*ip_hdr)->version_ihl & 0x0F) * 4;
    *udp_hdr = (struct rte_udp_hdr *)((uint8_t *)(*ip_hdr) + ip_header_len);

    // 检查UDP头长度
    if (rte_pktmbuf_data_len(mbuf) < sizeof(struct rte_ether_hdr) +
                                         ip_header_len +
                                         sizeof(struct rte_udp_hdr)) {
        printf("Packet too short for UDP header\n");
        return false;
    }

    // 检查是否为RoCEv2端口（通常为4791）
    if ((*udp_hdr)->dst_port != rte_cpu_to_be_16(ROCE_V2_PORT) &&
        (*udp_hdr)->src_port != rte_cpu_to_be_16(ROCE_V2_PORT)) {
        printf("Not RoCEv2 port (dst: %u, src: %u)\n",
               rte_be_to_cpu_16((*udp_hdr)->dst_port),
               rte_be_to_cpu_16((*udp_hdr)->src_port));
        return false;
    }

    // 检查BTH头长度
    *bth = (struct ib_bth *)(*udp_hdr + 1);
    if (rte_pktmbuf_data_len(mbuf) <
        sizeof(struct rte_ether_hdr) + ip_header_len +
            sizeof(struct rte_udp_hdr) + sizeof(struct ib_bth)) {
        printf("Packet too short for BTH header\n");
        return false;
    }

    return true;
}

/* 构建数据报文key */
int build_data_key_v4(struct cache_key_v4 *key, struct rte_ipv4_hdr *ip_hdr,
                      struct ib_bth *bth) {
    memset(key, 0, sizeof(*key));
    key->src_ip = ip_hdr->src_addr;
    key->dst_ip = ip_hdr->dst_addr;
    key->src_qp = 0; // BTH中只包含目的QP，从IPv4流表中获取
    key->dst_qp = get_dest_qp_from_bth(bth);
    key->pkey = rte_be_to_cpu_16(bth->pkey); // 转换为主机字节序
    key->resv = 0;

    // 从流表查找源QP
    struct flow_entry_v4 *entry = lookup_flow_v4(key);
    if (!entry) {
        printf("No flow entry found for data packet\n");
        return ERR;
    }

    key->src_qp = entry->src_qp;

    // 验证QP号有效性
    if (key->src_qp == 0 || key->dst_qp == 0) {
        printf("Invalid QP numbers (src_qp: %u, dst_qp: %u)\n", key->src_qp,
               key->dst_qp);
        return ERR;
    }

    return OK;
}

/* 构建控制报文key（ACK，NAK） */
int build_control_key_v4(struct cache_key_v4 *key, struct rte_ipv4_hdr *ip_hdr,
                         struct ib_bth *bth, enum gateway_role *role) {
    struct cache_key_v4 key_temp;
    memset(&key_temp, 0, sizeof(key_temp));

    key_temp.src_ip = ip_hdr->src_addr;
    key_temp.dst_ip = ip_hdr->dst_addr;
    key_temp.src_qp = 0; // BTH中只包含目的QP，从IPv4流表中获取
    key_temp.dst_qp = get_dest_qp_from_bth(bth);
    key_temp.pkey = rte_be_to_cpu_16(bth->pkey);
    key_temp.resv = 0;

    struct flow_entry_v4 *entry = lookup_flow_v4(&key_temp);
    if (!entry) {
        printf("No flow entry found for control packet\n");
        return ERR;
    }

    key_temp.src_qp = entry->src_qp;

    // 对调控制报头源目的IP，源目的QP，构造反方向数据报文缓存表key
    memset(key, 0, sizeof(*key));
    key->src_ip = key_temp.dst_ip;
    key->dst_ip = key_temp.src_ip;
    key->src_qp = key_temp.dst_qp;
    key->dst_qp = key_temp.src_qp;
    key->pkey = key_temp.pkey;
    key->resv = key_temp.resv;

    // 基于控制报文（ACK，NAK）流表角色互换，构造反方向数据报文方向的网关角色
    *role = (entry->role == SRC_GATEWAY) ? DST_GATEWAY : SRC_GATEWAY;

    // 验证反向key的有效性
    if (key->src_qp == 0 || key->dst_qp == 0) {
        printf("Invalid reverse QP numbers (src_qp: %u, dst_qp: %u)\n",
               key->src_qp, key->dst_qp);
        return ERR;
    }

    return OK;
}

/* 处理数据报文 */
void process_data_packet(struct global_data *gd, struct cache_key_v4 *key,
                         struct rte_mbuf *mbuf) {
    // 克隆报文并缓存
    struct rte_mbuf *cached_mbuf =
        rte_pktmbuf_copy(mbuf, gd->mbuf_pool, 0, UINT32_MAX);
    if (cached_mbuf) {
        printf(
            "Data packet: src_ip=0x%08X, dst_ip=0x%08X, src_qp=%u, dst_qp=%u\n",
            key->src_ip, key->dst_ip, key->src_qp, key->dst_qp);

        int ret = cache_tbl_v4_insert_data(gd->cache_tbl_v4, key, cached_mbuf);
        if (ret == ERR) {
            printf("Failed to cache data packet\n");
            rte_pktmbuf_free(cached_mbuf);
        }
    } else {
        printf("Failed to clone mbuf for caching\n");
    }

    // 释放原始报文
    rte_pktmbuf_free(mbuf);
}

// 处理ACK报文
int process_ack_v4(struct cache_hash_v4 *ht, const struct cache_key_v4 *key,
                   struct rte_mbuf *mbuf, uint32_t ack_psn, uint32_t msn) {
    uint32_t hash = cache_hash_v4_func(key);
    uint32_t bucket_idx = hash % ht->num_buckets;

    pthread_spin_lock(&ht->bucket_locks[bucket_idx]);
    struct cache_entry_v4 *entry = ht->buckets[bucket_idx];
    while (entry) {
        if (memcmp(&entry->key, key, sizeof(struct cache_key_v4)) == 0) {
            pthread_spin_lock(&entry->lock);

            uint32_t start_psn = entry->start_psn;
            uint32_t end_psn = ack_psn;
            uint32_t count;

            if (end_psn >= start_psn) {
                count = end_psn - start_psn + 1;
            } else {
                count = (PSN_MASK - start_psn + 1) + (end_psn + 1);
            }

            uint32_t freed_count = 0;
            uint32_t max_clear = RTE_MIN(count, MAX_PSN_ARRAY);

            for (uint32_t i = 0; i < max_clear; i++) {
                uint32_t current_psn = (start_psn + i) & PSN_MASK;
                uint32_t array_idx = current_psn % MAX_PSN_ARRAY;

                if (entry->mbuf_array[array_idx]) {
                    destroy_pkt_cache(entry->mbuf_array[array_idx]);
                    entry->mbuf_array[array_idx] = NULL;
                    entry->packet_count--;
                    freed_count++;
                }
            }

            // 更新start_psn
            if (freed_count > 0) {
                entry->start_psn = (ack_psn + 1) & PSN_MASK;
                if (entry->packet_count == 0) {
                    entry->start_psn = PSN_INVALID;
                    entry->end_psn = PSN_INVALID;
                    entry->cur_psn = PSN_INVALID;
                }

                printf("Connection: ACK freed %u packets up to PSN 0x%06X, "
                       "MSN: %u\n",
                       freed_count, ack_psn, msn);
            }

            // 收到ACK报文时清除RNR异常状态标志
            entry->receiver_not_ready = false;
            entry->rnr_timer = 0;

            // 清理RNR定时器
            if (entry->rnr_timer_fd >= 0) {
                // 注意：这里不能直接关闭，因为定时器可能由定时器系统管理
                // 标记为无效，由定时器系统清理
                entry->rnr_timer_fd = -1;
                entry->rnr_retry_count = 0;
            }

            pthread_spin_unlock(&entry->lock);
            pthread_spin_unlock(&ht->bucket_locks[bucket_idx]);
            rte_pktmbuf_free(mbuf);
            return OK;
        }
        entry = entry->next;
    }

    pthread_spin_unlock(&ht->bucket_locks[bucket_idx]);
    rte_pktmbuf_free(mbuf);
    return ERR;
}

// 处理RNR报文
int process_rnr_v4(struct cache_hash_v4 *ht, const struct cache_key_v4 *key,
                   struct rte_mbuf *mbuf, uint32_t rnr_psn) {
    uint32_t hash = cache_hash_v4_func(key);
    uint32_t bucket_idx = hash % ht->num_buckets;

    pthread_spin_lock(&ht->bucket_locks[bucket_idx]);
    struct cache_entry_v4 *entry = ht->buckets[bucket_idx];
    while (entry) {
        if (memcmp(&entry->key, key, sizeof(struct cache_key_v4)) == 0) {
            pthread_spin_lock(&entry->lock);

            entry->receiver_not_ready = true; // 设置RNR状态：接收方未就绪
            entry->rnr_timer = rte_get_timer_cycles() +
                               (RNR_TIMEOUT * rte_get_timer_hz() / 1000);
            printf(
                "Connection: RNR received for PSN 0x%06X, receiver not ready\n",
                rnr_psn);

            pthread_spin_unlock(&entry->lock);
            pthread_spin_unlock(&ht->bucket_locks[bucket_idx]);
            rte_pktmbuf_free(mbuf);
            return OK;
        }
        entry = entry->next;
    }

    pthread_spin_unlock(&ht->bucket_locks[bucket_idx]);
    rte_pktmbuf_free(mbuf);
    return ERR;
}

// 处理NAK报文
int process_nak_v4(struct global_data *gd, const struct cache_key_v4 *key,
                   struct rte_mbuf *mbuf, uint32_t nak_psn, uint8_t nak_code,
                   enum gateway_role *role) {
    uint32_t hash = cache_hash_v4_func(key);
    struct cache_hash_v4 *ht = gd->cache_tbl_v4;
    uint32_t bucket_idx = hash % ht->num_buckets;

    pthread_spin_lock(&ht->bucket_locks[bucket_idx]);
    struct cache_entry_v4 *entry = ht->buckets[bucket_idx];
    while (entry) {
        if (memcmp(&entry->key, key, sizeof(struct cache_key_v4)) == 0) {
            pthread_spin_lock(&entry->lock);
            printf("Connection: NAK received with code 0x%02X for PSN 0x%06X\n",
                   nak_code, nak_psn);

            switch (nak_code) {
            case NAK_CODE_SEQ_ERR: // 序列错误
                if (*role == DST_GATEWAY) {
                    dst_gateway_nak_v4(gd, entry, nak_psn);
                } else {
                    src_gateway_nak_v4(gd, entry, nak_psn);
                }
                break;
            case NAK_CODE_RNR: // RNR NAK，接收方未就绪
                create_rnr_timer(gd, entry, nak_psn);
                entry->receiver_not_ready = true;
                entry->rnr_timer = rte_get_timer_cycles() +
                                   (RNR_TIMEOUT * rte_get_timer_hz() / 1000);
                printf("Connection marked as RNR due to NAK\n");
                break;
            default:
                printf("Unhandled NAK code: 0x%02X\n", nak_code);
                break;
            }

            entry->timestamp = rte_get_timer_cycles();
            pthread_spin_unlock(&entry->lock);
            pthread_spin_unlock(&ht->bucket_locks[bucket_idx]);
            rte_pktmbuf_free(mbuf);
            return OK;
        }
        entry = entry->next;
    }

    pthread_spin_unlock(&ht->bucket_locks[bucket_idx]);
    rte_pktmbuf_free(mbuf);
    return ERR;
}

/* 处理AETH报文 */
void process_aeth_packet(struct global_data *gd, struct cache_key_v4 *key,
                         struct rte_mbuf *mbuf, struct ib_bth *bth,
                         enum gateway_role *role) {
    struct ib_aeth *aeth = (struct ib_aeth *)(bth + 1);
    uint32_t psn = get_psn_from_bth(bth);
    uint32_t msn = get_msn_from_aeth(aeth);
    uint8_t aeth_type = get_aeth_type(aeth);
    uint8_t aeth_code = get_aeth_code(aeth);

    printf("AETH packet: type=0x%02X, code=0x%02X, PSN=0x%06X, MSN=%u\n",
           aeth_type, aeth_code, psn, msn);
    printf("Key: src_ip=0x%08X, dst_ip=0x%08X, src_qp=%u, dst_qp=%u\n",
           key->src_ip, key->dst_ip, key->src_qp, key->dst_qp);

    switch (aeth_type) {
    case AETH_TYPE_ACK: // ACK报文
        printf("Processing ACK packet\n");
        process_ack_v4(gd->cache_tbl_v4, key, mbuf, psn, msn);
        break;

    case AETH_TYPE_RNR: // RNR报文
        printf("Processing RNR packet\n");
        process_rnr_v4(gd->cache_tbl_v4, key, mbuf, psn);
        break;

    case AETH_TYPE_NAK: // NAK报文
        printf("Processing NAK packet (code: 0x%02X)\n", aeth_code);
        process_nak_v4(gd, key, mbuf, psn, aeth_code, role);
        break;

    default: // 未知AETH类型
        printf("Unknown AETH type: 0x%02X, code: 0x%02X\n", aeth_type,
               aeth_code);
        rte_pktmbuf_free(mbuf);
        break;
    }
}

/* 处理控制报文 */
void process_control_packet(struct global_data *gd, struct cache_key_v4 *key,
                            struct rte_mbuf *mbuf, struct ib_bth *bth,
                            enum gateway_role *role) {
    uint32_t payload_len = rte_pktmbuf_data_len(mbuf) -
                           (sizeof(struct rte_ether_hdr) +
                            ((bth->version & 0x0F) * 4) + // IP头长度
                            sizeof(struct rte_udp_hdr) + sizeof(struct ib_bth));

    printf("Control packet: opcode=0x%02X, payload_len=%u\n", bth->opcode,
           payload_len);

    if (payload_len >= sizeof(struct ib_aeth)) {
        process_aeth_packet(gd, key, mbuf, bth, role);
    } else {
        printf("Control packet without AETH, opcode: 0x%02X\n", bth->opcode);
        rte_pktmbuf_free(mbuf);
    }
}

/* 构造目的网关GBN重传报文 */
uint32_t build_gbn_retrans(struct cache_entry_v4 *entry,
                           struct rte_mbuf **tx_burst, uint16_t max_burst,
                           uint32_t nak_psn) {
    uint32_t start_psn = nak_psn;      // 使用nak_psn作为起始PSN
    uint32_t end_psn = entry->end_psn; // 结束PSN
    uint32_t current_psn = start_psn;
    uint32_t processed_count = 0;
    bool found_gap = false;          // 是否遇到了缓存间隙
    bool collected_post_gap = false; // 是否已经收集了间隙后的第一个报文

    printf("GBN retransmission: start_psn=0x%06X, end_psn=0x%06X\n", start_psn,
           end_psn);

    // 计算需要遍历的PSN数量（考虑PSN回绕）
    uint32_t total_psns_to_check;
    if (end_psn >= start_psn) {
        total_psns_to_check = end_psn - start_psn + 1;
    } else {
        // 处理PSN回绕的情况
        total_psns_to_check = (PSN_MAX_VALUE - start_psn + 1) + (end_psn + 1);
    }

    // 限制遍历的数量，避免无限循环
    uint32_t max_psns_to_check =
        RTE_MIN(total_psns_to_check, MAX_PSN_ARRAY / 4);

    printf("GBN traverse: total=%u, max=%u\n", total_psns_to_check,
           max_psns_to_check);

    // 遍历PSN范围
    for (uint32_t i = 0; i < max_psns_to_check && processed_count < max_burst;
         i++) {
        current_psn = (start_psn + i) & PSN_MASK;
        uint32_t index = current_psn % MAX_PSN_ARRAY;

        if (!entry->mbuf_array[index]) {
            found_gap = true; // 该PSN没有缓存报文，视为缓存间隙
            printf("  Gap at PSN 0x%06X (index %u)\n", current_psn, index);
            continue;
        }

        struct pkt_cache *pc = entry->mbuf_array[index];
        if (!pc || !pc->mbuf) {
            found_gap = true; // pkt_cache无效或mbuf为空，视为缓存间隙
            printf("  Invalid cache at PSN 0x%06X\n", current_psn);
            continue;
        }

        if (pc->psn != current_psn) {
            printf(
                "  PSN mismatch at index %u (cached:0x%06X, expected:0x%06X)\n",
                index, pc->psn, current_psn);
            found_gap = true; // PSN不匹配，视为缓存间隙
            continue;
        }

        // PSN匹配，克隆报文
        struct rte_mbuf *mbuf = pc->mbuf;
        if (!found_gap) {
            // 还在第一段连续缓存中
            struct rte_mbuf *tx_mbuf =
                rte_pktmbuf_copy(mbuf, g_data.mbuf_pool, 0, UINT32_MAX);
            if (tx_mbuf) {
                tx_burst[processed_count++] = tx_mbuf;
                entry->timestamp = rte_get_timer_cycles();
                printf("  Collected PSN 0x%06X (continuous)\n", current_psn);
            } else {
                printf("  Failed to clone PSN 0x%06X\n", current_psn);
            }
        } else if (!collected_post_gap) {
            // 已经遇到了间隙，这是间隙后的第一个缓存报文
            struct rte_mbuf *tx_mbuf =
                rte_pktmbuf_copy(mbuf, g_data.mbuf_pool, 0, UINT32_MAX);
            if (tx_mbuf) {
                tx_burst[processed_count++] = tx_mbuf;
                collected_post_gap = true;
                entry->timestamp = rte_get_timer_cycles();
                printf("  Collected post-gap PSN 0x%06X\n", current_psn);
                break; // 只收集间隙后的第一个报文
            } else {
                printf("  Failed to clone post-gap PSN 0x%06X\n", current_psn);
            }
        }
    }

    printf("GBN traverse completed: collected %u packets\n", processed_count);
    return processed_count;
}

/* GBN重传 */
void gbn_retrans_v4(struct global_data *gd, struct cache_entry_v4 *entry,
                    uint32_t nak_psn) {
    struct rte_mbuf *tx_burst[BURST_SIZE];
    uint32_t processed =
        build_gbn_retrans(entry, tx_burst, BURST_SIZE, nak_psn);

    if (processed > 0) {
        // 批量发送重传的报文
        uint16_t nb_tx =
            rte_eth_tx_burst(gd->port_id, gd->tx_queue_id, tx_burst, processed);

        // 释放未发送成功的报文
        for (uint16_t j = nb_tx; j < processed; j++) {
            rte_pktmbuf_free(tx_burst[j]);
        }

        printf("GBN retransmission: triggered for PSN 0x%06X, sent %u/%u "
               "packets\n",
               nak_psn, nb_tx, processed);

        // 更新连接时间戳
        entry->timestamp = rte_get_timer_cycles();
    } else {
        printf("GBN retransmission: no packets to retransmit for PSN 0x%06X\n",
               nak_psn);
    }
}

/* 目的网关NAK处理 */
void dst_gateway_nak_v4(struct global_data *gd, struct cache_entry_v4 *entry,
                        uint32_t nak_psn) {
    // psn有效性判断
    uint32_t index = nak_psn % MAX_PSN_ARRAY;

    printf("DST gateway NAK handling for PSN 0x%06X\n", nak_psn);

    if (!entry->mbuf_array[index]) {
        printf("PSN 0x%06X not cached, triggering SR retransmission request\n",
               nak_psn);
        // 未缓存该PSN报文，延时触发SR重传请求
        create_nak_delay_timer(gd, entry, nak_psn);
        return;
    }

    // 使用GBN重传策略
    printf("Cached PSN 0x%06X found, performing GBN retransmission\n", nak_psn);
    gbn_retrans_v4(gd, entry, nak_psn);
}

/* 延时定时器中触发：目的网关向源网关发送SR重传请求 */
void send_sr_request_v4(struct global_data *gd, struct cache_entry_v4 *entry,
                        uint32_t nak_psn) {
    // psn有效性判断
    uint32_t index = nak_psn % MAX_PSN_ARRAY;

    printf("DST gateway NAK handling for PSN 0x%06X\n", nak_psn);

    if (!entry->mbuf_array[index]) {
        // TODO: 向源网关触发SR重传请求
        return;
    }
}

/* 源网关NAK处理 */
void src_gateway_nak_v4(struct global_data *gd, struct cache_entry_v4 *entry,
                        uint32_t nak_psn) {
    uint32_t index = nak_psn % MAX_PSN_ARRAY;

    printf("SRC gateway NAK handling for PSN 0x%06X\n", nak_psn);

    if (entry->mbuf_array[index]) {
        // 已缓存该PSN报文，等待触发SR重传，丢弃NAK
        printf("  PSN 0x%06X already cached, waiting for SR retransmission "
               "trigger\n",
               nak_psn);
        return;
    }

    // 未缓存该PSN报文，向源主机发送NAK触发GBN重传
    printf("  PSN 0x%06X not cached, forwarding NAK to source host\n", nak_psn);

    // TODO: 实现向源主机发送NAK的逻辑
    // 这里需要构造一个NAK报文并发送
    printf("  Sending NAK to source host for PSN 0x%06X\n", nak_psn);
}

/* 验证报文完整性 */
bool validate_packet_integrity(struct rte_mbuf *mbuf) {
    // 检查mbuf有效性
    if (!mbuf || !rte_pktmbuf_mtod(mbuf, void *)) {
        printf("Invalid mbuf pointer\n");
        return false;
    }

    // 检查报文长度
    if (rte_pktmbuf_data_len(mbuf) < 64) { // 最小以太网帧长度
        printf("Packet too short (%u bytes)\n", rte_pktmbuf_data_len(mbuf));
        return false;
    }
    return true;
}

/* 打印报文摘要信息 */
void print_packet_summary(struct rte_mbuf *mbuf, struct rte_ipv4_hdr *ip_hdr,
                          struct rte_udp_hdr *udp_hdr, struct ib_bth *bth) {
    char src_ip[INET_ADDRSTRLEN];
    char dst_ip[INET_ADDRSTRLEN];

    // 转换IP地址为字符串
    inet_ntop(AF_INET, &ip_hdr->src_addr, src_ip, INET_ADDRSTRLEN);
    inet_ntop(AF_INET, &ip_hdr->dst_addr, dst_ip, INET_ADDRSTRLEN);

    printf("Packet: %s:%u -> %s:%u, ", src_ip,
           rte_be_to_cpu_16(udp_hdr->src_port), dst_ip,
           rte_be_to_cpu_16(udp_hdr->dst_port));

    printf("QP=%u, PSN=0x%06X, Opcode=0x%02X\n", get_dest_qp_from_bth(bth),
           get_psn_from_bth(bth), bth->opcode);

    // 打印更多详细信息
    if (is_data_packet(bth)) {
        printf("  Type: Data packet, ");
        uint32_t payload_len =
            rte_pktmbuf_data_len(mbuf) -
            (sizeof(struct rte_ether_hdr) + ((bth->version & 0x0F) * 4) +
             sizeof(struct rte_udp_hdr) + sizeof(struct ib_bth));
        printf("Payload: %u bytes\n", payload_len);
    } else if (is_control_packet(bth)) {
        printf("  Type: Control packet\n");
    } else {
        printf("  Type: Unknown (opcode: 0x%02X)\n", bth->opcode);
    }
}

/* 检查报文是否需要特殊处理 */
bool needs_special_processing(struct ib_bth *bth) {
    uint8_t opcode = bth->opcode;

    // 检查是否为需要特殊处理的opcode
    switch (opcode) {
    case ROCE_OPCODE_RC_ATOMIC_ACK:
    case ROCE_OPCODE_RC_RDMA_READ_REQUEST:
    case ROCE_OPCODE_RC_RDMA_READ_RESPONSE_ONLY:
        return true;
    default:
        return false;
    }
}

/* 处理特殊报文类型 */
void process_special_packet(struct global_data *gd, struct rte_mbuf *mbuf,
                            struct ib_bth *bth) {
    uint8_t opcode = bth->opcode;

    printf("Processing special packet with opcode 0x%02X\n", opcode);

    switch (opcode) {
    case ROCE_OPCODE_RC_ATOMIC_ACK:
        // 原子操作ACK，需要特殊处理
        printf("  Atomic ACK packet\n");
        // TODO: 实现原子操作ACK处理
        break;

    case ROCE_OPCODE_RC_RDMA_READ_REQUEST:
        // RDMA读请求
        printf("  RDMA Read Request packet\n");
        // TODO: 实现RDMA读请求处理
        break;

    case ROCE_OPCODE_RC_RDMA_READ_RESPONSE_ONLY:
        // RDMA读响应
        printf("  RDMA Read Response packet\n");
        // TODO: 实现RDMA读响应处理
        break;

    default:
        printf("  Unhandled special opcode: 0x%02X\n", opcode);
        break;
    }

    // 释放报文（暂时不处理）
    rte_pktmbuf_free(mbuf);
}

/* 统计报文处理 */
void update_packet_statistics(struct global_data *gd, bool is_data_packet,
                              bool processed_successfully) {
    // TODO: 实现报文统计功能
    // 可以统计不同类型的报文数量、处理成功率等

    static uint64_t data_packet_count = 0;
    static uint64_t control_packet_count = 0;
    static uint64_t success_count = 0;
    static uint64_t failure_count = 0;

    if (is_data_packet) {
        data_packet_count++;
    } else {
        control_packet_count++;
    }

    if (processed_successfully) {
        success_count++;
    } else {
        failure_count++;
    }

    // 定期打印统计信息
    static uint64_t last_print_time = 0;
    uint64_t current_time = rte_get_timer_cycles();
    uint64_t print_interval = rte_get_timer_hz() * 10; // 10秒

    if (current_time - last_print_time > print_interval) {
        printf("Packet statistics: Data=%lu, Control=%lu, Success=%lu, "
               "Failure=%lu\n",
               data_packet_count, control_packet_count, success_count,
               failure_count);
        last_print_time = current_time;
    }
}

/* 检查报文是否来自可信源 */
bool is_packet_from_trusted_source(struct rte_ipv4_hdr *ip_hdr) {
    // TODO: 实现可信源检查逻辑
    // 可以基于IP白名单、认证信息等

    // 示例：检查是否为内部网络地址
    uint32_t src_ip = rte_be_to_cpu_32(ip_hdr->src_addr);

    // 检查是否为私有IP地址范围
    if ((src_ip & 0xFF000000) == 0x0A000000 || // 10.0.0.0/8
        (src_ip & 0xFFF00000) == 0xAC100000 || // 172.16.0.0/12
        (src_ip & 0xFFFF0000) == 0xC0A80000) { // 192.168.0.0/16
        return true;
    }

    return false;
}
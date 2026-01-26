#include "network/network.h"
#include "global.h"
#include <arpa/inet.h>
#include <rte_errno.h>
#include <rte_ethdev.h>
#include <rte_flow.h>
#include <rte_mempool.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// RoCEv2特定配置
#define ROCEV2_UDP_PORT 4791
#define ROCEV2_JUMBO_FRAME_SIZE 9014 // RoCEv2支持巨帧，通常为9K
#define ROCEV2_RSS_HASH_KEY_LEN 40   // RSS哈希键长度
#define ROCEV2_QUEUE_COUNT 1         // RoCEv2通常使用单队列以保持顺序

// RoCEv2 RSS哈希键（40字节）
static const uint8_t rocev2_rss_hash_key[ROCEV2_RSS_HASH_KEY_LEN] = {
    0x6d, 0x5a, 0x6d, 0x5a, 0x6d, 0x5a, 0x6d, 0x5a, 0x6d, 0x5a,
    0x6d, 0x5a, 0x6d, 0x5a, 0x6d, 0x5a, 0x6d, 0x5a, 0x6d, 0x5a,
    0x6d, 0x5a, 0x6d, 0x5a, 0x6d, 0x5a, 0x6d, 0x5a, 0x6d, 0x5a,
    0x6d, 0x5a, 0x6d, 0x5a, 0x6d, 0x5a, 0x6d, 0x5a, 0x6d, 0x5a};

// 信号处理函数 - 用于优雅退出
void signal_handler(int sig) {
    printf("\n接收到信号 %d，正在清理资源...\n", sig);

    // 设置停止标志
    g_data.stop = 1;

    // 记录信号类型用于调试
    switch (sig) {
    case SIGHUP:
        printf("信号类型: SIGHUP (终端挂起)\n");
        break;
    case SIGINT:
        printf("信号类型: SIGINT (Ctrl+C)\n");
        break;
    case SIGTERM:
        printf("信号类型: SIGTERM (终止请求)\n");
        break;
    case SIGQUIT:
        printf("信号类型: SIGQUIT (Ctrl+\\)\n");
        break;
    default:
        printf("信号类型: %d\n", sig);
        break;
    }
}

// 注册信号处理器
void setup_signal_handlers() {
    struct sigaction sa;

    // 也可以使用下面函数直接基于信号注册回调函数
    // signal(SIGHUP, signal_handler);

    // 设置信号处理函数
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    // 注册信号
    // 终端挂起  1
    if (sigaction(SIGHUP, &sa, NULL) == -1) {
        perror("无法注册SIGHUP信号处理器");
    }

    // Ctrl+C 信号  2
    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("无法注册SIGINT信号处理器");
    }

    // 终止请求信号  15
    if (sigaction(SIGTERM, &sa, NULL) == -1) {
        perror("无法注册SIGTERM信号处理器");
    }

    // Ctrl + \ 信号 3
    if (sigaction(SIGQUIT, &sa, NULL) == -1) {
        perror("无法注册SIGQUIT信号处理器");
    }

    // 忽略SIGPIPE信号（避免网络连接断开导致程序退出）
    signal(SIGPIPE, SIG_IGN);

    printf("信号处理器已注册\n");
}

// 创建RoCEv2流规则（基于UDP端口4791过滤）
static int create_rocev2_flow_rule(uint16_t port) {
    struct rte_flow_error error;
    struct rte_flow_attr attr;
    struct rte_flow_item pattern[4];
    struct rte_flow_action action[2];
    struct rte_flow *flow;

    memset(&attr, 0, sizeof(attr));
    attr.ingress = 1;
    attr.priority = 0;

    // 匹配以太网类型为IPv4
    memset(pattern, 0, sizeof(pattern));
    pattern[0].type = RTE_FLOW_ITEM_TYPE_ETH;

    // 匹配IPv4协议
    struct rte_flow_item_ipv4 ipv4_spec;
    struct rte_flow_item_ipv4 ipv4_mask;
    memset(&ipv4_spec, 0, sizeof(ipv4_spec));
    memset(&ipv4_mask, 0, sizeof(ipv4_mask));
    ipv4_spec.hdr.next_proto_id = IPPROTO_UDP;
    ipv4_mask.hdr.next_proto_id = 0xFF;

    pattern[1].type = RTE_FLOW_ITEM_TYPE_IPV4;
    pattern[1].spec = &ipv4_spec;
    pattern[1].mask = &ipv4_mask;

    // 匹配UDP目的端口为4791
    struct rte_flow_item_udp udp_spec;
    struct rte_flow_item_udp udp_mask;
    memset(&udp_spec, 0, sizeof(udp_spec));
    memset(&udp_mask, 0, sizeof(udp_mask));
    udp_spec.hdr.dst_port = rte_cpu_to_be_16(ROCEV2_UDP_PORT);
    udp_mask.hdr.dst_port = 0xFFFF;

    pattern[2].type = RTE_FLOW_ITEM_TYPE_UDP;
    pattern[2].spec = &udp_spec;
    pattern[2].mask = &udp_mask;

    pattern[3].type = RTE_FLOW_ITEM_TYPE_END;

    // 动作为：发送到队列0
    memset(action, 0, sizeof(action));
    action[0].type = RTE_FLOW_ACTION_TYPE_QUEUE;
    struct rte_flow_action_queue queue = {.index = 0};
    action[0].conf = &queue;
    action[1].type = RTE_FLOW_ACTION_TYPE_END;

    // 创建流规则
    flow = rte_flow_create(port, &attr, pattern, action, &error);
    if (!flow) {
        fprintf(stderr, "无法创建RoCEv2流规则: %s\n", error.message);
        return -1;
    }

    printf("已创建RoCEv2流规则（UDP端口%d -> 队列0）\n", ROCEV2_UDP_PORT);
    return 0;
}

// 优化网卡配置为RoCEv2专用
int configure_nic_for_rocev2(uint16_t port, struct rte_mempool *mbuf_pool) {
    struct rte_eth_conf port_conf = {0};
    struct rte_eth_dev_info dev_info;
    int ret;

    // 获取网卡信息
    ret = rte_eth_dev_info_get(port, &dev_info);
    if (ret != 0) {
        fprintf(stderr, "无法获取网卡%d信息: %s\n", port, strerror(-ret));
        return ret;
    }

    printf("配置网卡%d为RoCEv2优化模式...\n", port);

    // 接收模式配置
    port_conf.rxmode.mtu = ROCEV2_JUMBO_FRAME_SIZE;
    port_conf.rxmode.max_rx_pkt_len = ROCEV2_JUMBO_FRAME_SIZE;
    port_conf.rxmode.split_hdr_size = 0;
    port_conf.rxmode.offloads = 0;

    // 针对RoCEv2开启特定的卸载功能
    if (dev_info.rx_offload_capa & RTE_ETH_RX_OFFLOAD_CHECKSUM) {
        port_conf.rxmode.offloads |= RTE_ETH_RX_OFFLOAD_CHECKSUM;
        printf("  启用接收校验和卸载\n");
    }

    if (dev_info.rx_offload_capa & RTE_ETH_RX_OFFLOAD_SCATTER) {
        port_conf.rxmode.offloads |= RTE_ETH_RX_OFFLOAD_SCATTER;
        printf("  启用分散/聚集接收\n");
    }

    // RSS配置 - 只针对UDP进行哈希
    port_conf.rx_adv_conf.rss_conf.rss_key = (uint8_t *)rocev2_rss_hash_key;
    port_conf.rx_adv_conf.rss_conf.rss_key_len = ROCEV2_RSS_HASH_KEY_LEN;
    port_conf.rx_adv_conf.rss_conf.rss_hf = RTE_ETH_RSS_IP | RTE_ETH_RSS_UDP;

    // 发送模式配置
    port_conf.txmode.mtu = ROCEV2_JUMBO_FRAME_SIZE;
    port_conf.txmode.offloads = 0;

    if (dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_MULTI_SEGS) {
        port_conf.txmode.offloads |= RTE_ETH_TX_OFFLOAD_MULTI_SEGS;
        printf("  启用多分段发送\n");
    }

    if (dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_IPV4_CKSUM) {
        port_conf.txmode.offloads |= RTE_ETH_TX_OFFLOAD_IPV4_CKSUM;
        printf("  启用IPv4校验和卸载\n");
    }

    if (dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_UDP_CKSUM) {
        port_conf.txmode.offloads |= RTE_ETH_TX_OFFLOAD_UDP_CKSUM;
        printf("  启用UDP校验和卸载\n");
    }

    if (dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE) {
        port_conf.txmode.offloads |= RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE;
        printf("  启用mbuf快速释放\n");
    }

    // 配置网卡
    ret = rte_eth_dev_configure(port, ROCEV2_QUEUE_COUNT, ROCEV2_QUEUE_COUNT,
                                &port_conf);
    if (ret != 0) {
        fprintf(stderr, "无法配置网卡%d: %s\n", port, strerror(-ret));
        return ret;
    }

    // 调整队列描述符数量
    uint16_t rx_desc_count = NIC_RX_RING_SIZE;
    uint16_t tx_desc_count = NIC_TX_RING_SIZE;
    ret =
        rte_eth_dev_adjust_nb_rx_tx_desc(port, &rx_desc_count, &tx_desc_count);
    if (ret != 0) {
        fprintf(stderr, "无法调整网卡%d队列描述符数量: %s\n", port,
                strerror(-ret));
    } else {
        printf("  接收队列大小: %u, 发送队列大小: %u\n", rx_desc_count,
               tx_desc_count);
    }

    // 配置接收队列
    struct rte_eth_rxconf rxconf = dev_info.default_rxconf;
    rxconf.offloads = port_conf.rxmode.offloads;
    rxconf.rx_thresh.pthresh = 8; // 预取阈值
    rxconf.rx_thresh.hthresh = 8; // 主机阈值
    rxconf.rx_thresh.wthresh = 4; // 回写阈值

    ret =
        rte_eth_rx_queue_setup(port, 0, rx_desc_count,
                               rte_eth_dev_socket_id(port), &rxconf, mbuf_pool);
    if (ret < 0) {
        fprintf(stderr, "无法设置网卡%d接收队列: %s\n", port, strerror(-ret));
        return ret;
    }

    // 配置发送队列
    struct rte_eth_txconf txconf = dev_info.default_txconf;
    txconf.offloads = port_conf.txmode.offloads;
    txconf.tx_thresh.pthresh = 32; // 预取阈值
    txconf.tx_thresh.hthresh = 0;  // 主机阈值
    txconf.tx_thresh.wthresh = 0;  // 回写阈值
    txconf.tx_rs_thresh = 32;      // RS位阈值

    ret = rte_eth_tx_queue_setup(port, 0, tx_desc_count,
                                 rte_eth_dev_socket_id(port), &txconf);
    if (ret < 0) {
        fprintf(stderr, "无法设置网卡%d发送队列: %s\n", port, strerror(-ret));
        return ret;
    }

    return 0;
}

// 网卡初始化（RoCEv2优化版）
int port_init(uint16_t port, struct rte_mempool *mbuf_pool) {
    int ret;

    printf("初始化网卡%d为RoCEv2网关...\n", port);

    // 优化网卡配置
    ret = configure_nic_for_rocev2(port, mbuf_pool);
    if (ret != 0) {
        return ret;
    }

    // 启动网卡
    ret = rte_eth_dev_start(port);
    if (ret < 0) {
        fprintf(stderr, "无法启动网卡%d: %s\n", port, strerror(-ret));
        return ret;
    }

    // 启用混杂模式（可选，建议在生产环境中关闭）
    rte_eth_promiscuous_enable(port);
    printf("  启用混杂模式\n");

    // 获取网卡MAC地址
    struct rte_ether_addr addr;
    ret = rte_eth_macaddr_get(port, &addr);
    if (ret != 0) {
        fprintf(stderr, "无法获取网卡%d MAC地址\n", port);
    } else {
        printf("  MAC地址: %02x:%02x:%02x:%02x:%02x:%02x\n", addr.addr_bytes[0],
               addr.addr_bytes[1], addr.addr_bytes[2], addr.addr_bytes[3],
               addr.addr_bytes[4], addr.addr_bytes[5]);
    }

    // 创建RoCEv2流规则（硬件过滤）
    ret = create_rocev2_flow_rule(port);
    if (ret != 0) {
        printf("  警告: 无法创建硬件流规则，使用软件过滤\n");
    }

    // 获取网卡链路状态
    struct rte_eth_link link;
    int link_wait_count = 0;
    const int max_link_wait = 10; // 最多等待10秒

    do {
        rte_eth_link_get(port, &link);
        if (!link.link_status) {
            if (link_wait_count == 0) {
                printf("等待网卡%d链路就绪...\n", port);
            }
            sleep(1);
            link_wait_count++;
        }
    } while (!link.link_status && link_wait_count < max_link_wait &&
             !g_data.stop);

    if (link.link_status) {
        printf("  链路已启动: %u Mbps, %s\n", link.link_speed,
               link.link_duplex == RTE_ETH_LINK_FULL_DUPLEX ? "全双工"
                                                            : "半双工");

        // 验证配置
        printf("  验证配置:\n");
        struct rte_eth_conf port_conf;
        rte_eth_dev_conf_get(port, &port_conf);

        if (port_conf.rxmode.max_rx_pkt_len >= ROCEV2_JUMBO_FRAME_SIZE) {
            printf("    ✓ 支持巨帧 (%u bytes)\n",
                   port_conf.rxmode.max_rx_pkt_len);
        } else {
            printf("    ⚠ 最大帧大小: %u bytes (RoCEv2建议使用巨帧)\n",
                   port_conf.rxmode.max_rx_pkt_len);
        }

        // 打印卸载功能状态
        printf("   卸载功能:\n");
        if (port_conf.rxmode.offloads & RTE_ETH_RX_OFFLOAD_CHECKSUM) {
            printf("    ✓ 接收校验和卸载\n");
        }
        if (port_conf.txmode.offloads & RTE_ETH_TX_OFFLOAD_UDP_CKSUM) {
            printf("    ✓ UDP发送校验和卸载\n");
        }

        // 打印统计信息
        struct rte_eth_stats stats;
        if (rte_eth_stats_get(port, &stats) == 0) {
            printf("   队列统计: RX=%lu, TX=%lu\n", stats.ipackets,
                   stats.opackets);
        }
    } else {
        printf("警告: 网卡%d链路未就绪\n", port);
        return -1;
    }

    printf("网卡%d初始化完成\n", port);
    return 0;
}

// 关闭网卡
void close_network_port(void) {
    if (g_data.port_id >= RTE_MAX_ETHPORTS) {
        printf("无效的网卡ID: %u\n", g_data.port_id);
        return;
    }

    printf("正在停止网卡%u...\n", g_data.port_id);

    // 获取并打印最终的统计信息
    struct rte_eth_stats stats;
    if (rte_eth_stats_get(g_data.port_id, &stats) == 0) {
        printf("网卡%u最终统计:\n", g_data.port_id);
        printf("  接收报文: %lu\n", stats.ipackets);
        printf("  发送报文: %lu\n", stats.opackets);
        printf("  接收字节: %lu\n", stats.ibytes);
        printf("  发送字节: %lu\n", stats.obytes);
        printf("  接收错误: %lu\n", stats.ierrors);
        printf("  发送错误: %lu\n", stats.oerrors);
        printf("  接收丢包: %lu\n", stats.imissed);
        printf("  RX NOMEM: %lu\n", stats.rx_nombuf);

        // 计算报文平均大小
        if (stats.ipackets > 0) {
            uint64_t avg_rx_size = stats.ibytes / stats.ipackets;
            printf("  平均接收大小: %lu bytes\n", avg_rx_size);
        }

        if (stats.opackets > 0) {
            uint64_t avg_tx_size = stats.obytes / stats.opackets;
            printf("  平均发送大小: %lu bytes\n", avg_tx_size);
        }
    }

    // 停止网卡
    int ret = rte_eth_dev_stop(g_data.port_id);
    if (ret != 0) {
        fprintf(stderr, "停止网卡%u失败: %s\n", g_data.port_id, strerror(-ret));
    } else {
        printf("网卡%u已停止\n", g_data.port_id);
    }

    // 关闭网卡
    ret = rte_eth_dev_close(g_data.port_id);
    if (ret != 0) {
        fprintf(stderr, "关闭网卡%u失败: %s\n", g_data.port_id, strerror(-ret));
    } else {
        printf("网卡%u已关闭\n", g_data.port_id);
    }
}

// 获取网卡统计信息
int get_port_statistics(uint16_t port, struct rte_eth_stats *stats) {
    if (port >= RTE_MAX_ETHPORTS) {
        return -1;
    }

    return rte_eth_stats_get(port, stats);
}

// 重置网卡统计信息
int reset_port_statistics(uint16_t port) {
    if (port >= RTE_MAX_ETHPORTS) {
        return -1;
    }

    return rte_eth_stats_reset(port);
}

// 打印详细的网卡信息（RoCEv2优化版）
void print_port_info(uint16_t port) {
    if (port >= RTE_MAX_ETHPORTS) {
        printf("无效的网卡ID: %u\n", port);
        return;
    }

    printf("\n=== 网卡%u详细信息 (RoCEv2优化) ===\n", port);

    // 获取网卡信息
    struct rte_eth_dev_info dev_info;
    if (rte_eth_dev_info_get(port, &dev_info) != 0) {
        printf("无法获取网卡信息\n");
        return;
    }

    printf("驱动程序: %s\n", dev_info.driver_name);
    printf("设备名称: %s\n", dev_info.device->name);

    // MAC地址
    struct rte_ether_addr addr;
    if (rte_eth_macaddr_get(port, &addr) == 0) {
        printf("MAC地址: %02x:%02x:%02x:%02x:%02x:%02x\n", addr.addr_bytes[0],
               addr.addr_bytes[1], addr.addr_bytes[2], addr.addr_bytes[3],
               addr.addr_bytes[4], addr.addr_bytes[5]);
    }

    // 链路状态
    struct rte_eth_link link;
    rte_eth_link_get(port, &link);
    printf("链路状态: %s\n", link.link_status ? "启动" : "断开");
    if (link.link_status) {
        printf("链路速度: %u Mbps\n", link.link_speed);
        printf("双工模式: %s\n", link.link_duplex == RTE_ETH_LINK_FULL_DUPLEX
                                     ? "全双工"
                                     : "半双工");
    }

    // RoCEv2特定配置
    struct rte_eth_conf port_conf;
    rte_eth_dev_conf_get(port, &port_conf);

    printf("\nRoCEv2配置:\n");
    printf("  最大接收包长度: %u bytes\n", port_conf.rxmode.max_rx_pkt_len);
    printf("  MTU: %u\n", port_conf.rxmode.mtu);
    printf("  RSS哈希函数: 0x%lx\n", port_conf.rx_adv_conf.rss_conf.rss_hf);

    printf("\n卸载功能:\n");
    printf("  接收卸载: 0x%lx\n", port_conf.rxmode.offloads);
    printf("  发送卸载: 0x%lx\n", port_conf.txmode.offloads);

    if (port_conf.rxmode.offloads & RTE_ETH_RX_OFFLOAD_CHECKSUM) {
        printf("  ✓ 接收校验和卸载\n");
    }
    if (port_conf.txmode.offloads & RTE_ETH_TX_OFFLOAD_UDP_CKSUM) {
        printf("  ✓ UDP发送校验和卸载\n");
    }
    if (port_conf.txmode.offloads & RTE_ETH_TX_OFFLOAD_IPV4_CKSUM) {
        printf("  ✓ IPv4发送校验和卸载\n");
    }

    // 能力信息
    printf("\n网卡能力:\n");
    printf("  支持的最大接收队列: %u\n", dev_info.max_rx_queues);
    printf("  支持的最大发送队列: %u\n", dev_info.max_tx_queues);
    printf("  支持的最大接收描述符: %u\n", dev_info.rx_desc_lim.nb_max);
    printf("  支持的最大发送描述符: %u\n", dev_info.tx_desc_lim.nb_max);

    // 统计信息
    struct rte_eth_stats stats;
    if (rte_eth_stats_get(port, &stats) == 0) {
        printf("\n统计信息:\n");
        printf("  接收报文: %lu\n", stats.ipackets);
        printf("  发送报文: %lu\n", stats.opackets);
        printf("  接收字节: %lu\n", stats.ibytes);
        printf("  发送字节: %lu\n", stats.obytes);
        printf("  接收错误: %lu\n", stats.ierrors);
        printf("  发送错误: %lu\n", stats.oerrors);
        printf("  接收丢包: %lu\n", stats.imissed);
        printf("  RX NOMEM: %lu\n", stats.rx_nombuf);

        if (stats.ipackets > 0 && stats.opackets > 0) {
            uint64_t avg_rx_size = stats.ibytes / stats.ipackets;
            uint64_t avg_tx_size = stats.obytes / stats.opackets;
            printf("  平均接收大小: %lu bytes\n", avg_rx_size);
            printf("  平均发送大小: %lu bytes\n", avg_tx_size);
        }
    }

    printf("====================================\n\n");
}

// 检查网卡是否支持RoCEv2所需的特定功能
bool check_rocev2_capabilities(uint16_t port) {
    if (port >= RTE_MAX_ETHPORTS) {
        return false;
    }

    struct rte_eth_dev_info dev_info;
    if (rte_eth_dev_info_get(port, &dev_info) != 0) {
        return false;
    }

    printf("检查网卡%u的RoCEv2能力:\n", port);

    bool all_supported = true;

    // 检查UDP校验和卸载
    if (dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_UDP_CKSUM) {
        printf("  ✓ 支持UDP校验和卸载\n");
    } else {
        printf("  ✗ 不支持UDP校验和卸载\n");
        all_supported = false;
    }

    // 检查巨帧支持
    if (dev_info.max_rx_pkt_len >= ROCEV2_JUMBO_FRAME_SIZE) {
        printf("  ✓ 支持巨帧 (%u bytes)\n", dev_info.max_rx_pkt_len);
    } else {
        printf("  ⚠ 最大帧大小: %u bytes (RoCEv2建议使用巨帧)\n",
               dev_info.max_rx_pkt_len);
    }

    // 检查多队列支持
    if (dev_info.max_rx_queues >= ROCEV2_QUEUE_COUNT &&
        dev_info.max_tx_queues >= ROCEV2_QUEUE_COUNT) {
        printf("  ✓ 支持所需队列数\n");
    } else {
        printf("  ✗ 队列数不足 (RX:%u, TX:%u)\n", dev_info.max_rx_queues,
               dev_info.max_tx_queues);
        all_supported = false;
    }

    // 检查RSS支持
    if (dev_info.flow_type_rss_offloads & RTE_ETH_RSS_UDP) {
        printf("  ✓ 支持UDP RSS\n");
    } else {
        printf("  ⚠ 不支持UDP RSS\n");
    }

    return all_supported;
}

// 设置网卡MTU（支持巨帧）
int set_port_mtu(uint16_t port, uint16_t mtu) {
    if (port >= RTE_MAX_ETHPORTS) {
        return -1;
    }

    // RoCEv2通常使用巨帧，检查MTU是否在有效范围内
    uint16_t min_mtu = RTE_ETHER_MIN_LEN;
    uint16_t max_mtu = ROCEV2_JUMBO_FRAME_SIZE;

    if (mtu < min_mtu || mtu > max_mtu) {
        fprintf(stderr, "MTU %u 不在有效范围内 [%u, %u]\n", mtu, min_mtu,
                max_mtu);
        return -1;
    }

    // 停止网卡以修改MTU
    int ret = rte_eth_dev_stop(port);
    if (ret != 0) {
        fprintf(stderr, "停止网卡%u失败: %s\n", port, strerror(-ret));
        return ret;
    }

    // 重新配置网卡MTU
    struct rte_eth_conf port_conf = {0};
    port_conf.rxmode.mtu = mtu;
    port_conf.rxmode.max_rx_pkt_len =
        mtu + RTE_ETHER_HDR_LEN + RTE_ETHER_CRC_LEN;

    ret = rte_eth_dev_configure(port, ROCEV2_QUEUE_COUNT, ROCEV2_QUEUE_COUNT,
                                &port_conf);
    if (ret != 0) {
        fprintf(stderr, "重新配置网卡%u失败: %s\n", port, strerror(-ret));
        return ret;
    }

    // 重新启动网卡
    ret = rte_eth_dev_start(port);
    if (ret != 0) {
        fprintf(stderr, "重新启动网卡%u失败: %s\n", port, strerror(-ret));
        return ret;
    }

    printf("网卡%u MTU已设置为 %u (最大包长度: %u)\n", port, mtu,
           port_conf.rxmode.max_rx_pkt_len);
    return 0;
}

// 启用/禁用网卡混杂模式
int set_port_promiscuous(uint16_t port, bool enable) {
    if (port >= RTE_MAX_ETHPORTS) {
        return -1;
    }

    if (enable) {
        rte_eth_promiscuous_enable(port);
        printf("网卡%u 混杂模式已启用\n", port);
    } else {
        rte_eth_promiscuous_disable(port);
        printf("网卡%u 混杂模式已禁用\n", port);
    }

    return 0;
}

// 设置网卡RSS配置（RoCEv2优化）
int set_port_rss_config(uint16_t port, uint64_t rss_hf) {
    if (port >= RTE_MAX_ETHPORTS) {
        return -1;
    }

    // 确保只包含UDP相关的RSS哈希函数
    rss_hf &= (RTE_ETH_RSS_IP | RTE_ETH_RSS_UDP);

    struct rte_eth_conf port_conf = {0};
    port_conf.rxmode.mtu = ROCEV2_JUMBO_FRAME_SIZE;
    port_conf.rxmode.max_rx_pkt_len = ROCEV2_JUMBO_FRAME_SIZE;
    port_conf.rx_adv_conf.rss_conf.rss_key = (uint8_t *)rocev2_rss_hash_key;
    port_conf.rx_adv_conf.rss_conf.rss_key_len = ROCEV2_RSS_HASH_KEY_LEN;
    port_conf.rx_adv_conf.rss_conf.rss_hf = rss_hf;

    // 需要停止网卡以修改RSS配置
    int ret = rte_eth_dev_stop(port);
    if (ret != 0) {
        fprintf(stderr, "停止网卡%u失败: %s\n", port, strerror(-ret));
        return ret;
    }

    ret = rte_eth_dev_configure(port, ROCEV2_QUEUE_COUNT, ROCEV2_QUEUE_COUNT,
                                &port_conf);
    if (ret != 0) {
        fprintf(stderr, "重新配置网卡%u失败: %s\n", port, strerror(-ret));
        return ret;
    }

    ret = rte_eth_dev_start(port);
    if (ret != 0) {
        fprintf(stderr, "重新启动网卡%u失败: %s\n", port, strerror(-ret));
        return ret;
    }

    printf("网卡%u RSS配置已更新: 0x%lx (仅UDP)\n", port, rss_hf);
    return 0;
}

// 获取网卡支持的RSS哈希函数
uint64_t get_port_supported_rss(uint16_t port) {
    if (port >= RTE_MAX_ETHPORTS) {
        return 0;
    }

    struct rte_eth_dev_info dev_info;
    if (rte_eth_dev_info_get(port, &dev_info) != 0) {
        return 0;
    }

    return dev_info.flow_type_rss_offloads;
}

// 发送RoCEv2测试报文
struct rte_mbuf *create_rocev2_test_packet(struct rte_mempool *mbuf_pool,
                                           const uint8_t *src_mac,
                                           const uint8_t *dst_mac,
                                           uint32_t src_ip, uint32_t dst_ip,
                                           uint32_t dest_qp, uint32_t psn,
                                           uint8_t opcode) {
    struct rte_mbuf *mbuf = rte_pktmbuf_alloc(mbuf_pool);
    if (!mbuf) {
        return NULL;
    }

    // RoCEv2报文结构
    uint16_t total_len =
        sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) +
        sizeof(struct rte_udp_hdr) + sizeof(struct ib_bth) + 32; // 模拟载荷

    if (rte_pktmbuf_append(mbuf, total_len) == NULL) {
        rte_pktmbuf_free(mbuf);
        return NULL;
    }

    uint8_t *data = rte_pktmbuf_mtod(mbuf, uint8_t *);

    // 以太网头
    struct rte_ether_hdr *eth_hdr = (struct rte_ether_hdr *)data;
    rte_memcpy(eth_hdr->s_addr.addr_bytes, src_mac, RTE_ETHER_ADDR_LEN);
    rte_memcpy(eth_hdr->d_addr.addr_bytes, dst_mac, RTE_ETHER_ADDR_LEN);
    eth_hdr->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

    // IP头
    struct rte_ipv4_hdr *ip_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);
    ip_hdr->version_ihl = 0x45;
    ip_hdr->type_of_service = 0;
    ip_hdr->total_length =
        rte_cpu_to_be_16(total_len - sizeof(struct rte_ether_hdr));
    ip_hdr->packet_id = 0;
    ip_hdr->fragment_offset = 0;
    ip_hdr->time_to_live = 64;
    ip_hdr->next_proto_id = IPPROTO_UDP;
    ip_hdr->src_addr = src_ip;
    ip_hdr->dst_addr = dst_ip;

    // 计算IP校验和
    ip_hdr->hdr_checksum = 0;
    ip_hdr->hdr_checksum = rte_ipv4_cksum(ip_hdr);

    // UDP头
    struct rte_udp_hdr *udp_hdr = (struct rte_udp_hdr *)(ip_hdr + 1);
    udp_hdr->src_port = rte_cpu_to_be_16(12345); // 任意源端口
    udp_hdr->dst_port = rte_cpu_to_be_16(ROCEV2_UDP_PORT);
    udp_hdr->dgram_len = rte_cpu_to_be_16(sizeof(struct rte_udp_hdr) +
                                          sizeof(struct ib_bth) + 32);
    udp_hdr->dgram_cksum = 0; // 由网卡硬件计算

    // BTH头
    struct ib_bth *bth = (struct ib_bth *)(udp_hdr + 1);
    bth->opcode = opcode;
    bth->solicited_event = 0;
    bth->mig_req = 0;
    bth->pad_count = 0;
    bth->version = 0; // RoCEv2
    bth->pkey = rte_cpu_to_be_16(0xFFFF);
    bth->reserved1 = 0;

    // 24位目的QP
    bth->dest_qp[0] = (dest_qp >> 16) & 0xFF;
    bth->dest_qp[1] = (dest_qp >> 8) & 0xFF;
    bth->dest_qp[2] = dest_qp & 0xFF;

    bth->ack_req = 1; // 需要ACK
    bth->reserved2 = 0;

    // 24位PSN
    bth->psn[0] = (psn >> 16) & 0xFF;
    bth->psn[1] = (psn >> 8) & 0xFF;
    bth->psn[2] = psn & 0xFF;

    // 模拟载荷
    uint8_t *payload = (uint8_t *)(bth + 1);
    for (int i = 0; i < 32; i++) {
        payload[i] = i;
    }

    return mbuf;
}

// RoCEv2网络诊断功能
void rocev2_network_diagnostics(uint16_t port) {
    printf("\n=== RoCEv2网络诊断 (端口%u) ===\n", port);

    // 1. 检查网卡是否存在
    if (!rte_eth_dev_is_valid_port(port)) {
        printf("错误: 网卡%u不存在\n", port);
        return;
    }

    // 2. 检查链路状态
    bool link_up = check_port_link_status(port);
    printf("链路状态: %s\n", link_up ? "启动" : "断开");

    if (!link_up) {
        printf("严重错误: 网卡链路断开，RoCEv2通信无法进行\n");
        return;
    }

    // 3. 检查RoCEv2能力
    bool rocev2_capable = check_rocev2_capabilities(port);
    printf("RoCEv2能力支持: %s\n", rocev2_capable ? "完全支持" : "部分支持");

    // 4. 获取统计信息
    struct rte_eth_stats stats;
    if (get_port_statistics(port, &stats) == 0) {
        printf("\n错误统计:\n");
        printf("  接收错误: %lu\n", stats.ierrors);
        printf("  发送错误: %lu\n", stats.oerrors);
        printf("  接收丢包: %lu\n", stats.imissed);
        printf("  RX NOMEM: %lu\n", stats.rx_nombuf);

        if (stats.ierrors > 0 || stats.oerrors > 0) {
            printf("警告: 检测到网络错误，可能影响RoCEv2可靠性\n");
        }

        if (stats.rx_nombuf > 0) {
            printf("警告: 接收缓冲区不足，考虑增加mbuf池大小\n");
        }

        // 计算吞吐量（如果运行时间足够长）
        static uint64_t last_ipackets = 0;
        static uint64_t last_opackets = 0;
        static time_t last_check = 0;

        time_t now = time(NULL);
        if (last_check > 0 && (now - last_check) >= 5) {
            double rx_rate =
                (stats.ipackets - last_ipackets) / (double)(now - last_check);
            double tx_rate =
                (stats.opackets - last_opackets) / (double)(now - last_check);

            printf("\n吞吐量 (最近%ld秒):\n", now - last_check);
            printf("  接收: %.2f Kpps\n", rx_rate / 1000);
            printf("  发送: %.2f Kpps\n", tx_rate / 1000);
        }

        last_ipackets = stats.ipackets;
        last_opackets = stats.opackets;
        last_check = now;
    }

    // 5. 检查MTU配置
    struct rte_eth_conf port_conf;
    rte_eth_dev_conf_get(port, &port_conf);

    if (port_conf.rxmode.max_rx_pkt_len >= ROCEV2_JUMBO_FRAME_SIZE) {
        printf("✓ MTU配置: 支持巨帧 (%u bytes)\n",
               port_conf.rxmode.max_rx_pkt_len);
    } else {
        printf("⚠ MTU配置: 最大帧大小 %u bytes (RoCEv2建议使用巨帧)\n",
               port_conf.rxmode.max_rx_pkt_len);
    }

    printf("==============================\n\n");
}

// 打印所有可用网卡（RoCEv2优化信息）
void print_available_ports(void) {
    uint16_t port_count = rte_eth_dev_count_avail();

    if (port_count == 0) {
        printf("没有可用的网卡\n");
        return;
    }

    printf("可用的网卡 (%u个) - RoCEv2兼容性检查:\n", port_count);

    for (uint16_t port = 0; port < port_count; port++) {
        struct rte_eth_dev_info dev_info;
        if (rte_eth_dev_info_get(port, &dev_info) != 0) {
            printf("  网卡%u: 无法获取信息\n", port);
            continue;
        }

        struct rte_ether_addr addr;
        rte_eth_macaddr_get(port, &addr);

        struct rte_eth_link link;
        rte_eth_link_get(port, &link);

        printf("  网卡%u: %s\n", port,
               dev_info.device->name ? dev_info.device->name : "未知");
        printf("      驱动: %s\n", dev_info.driver_name);
        printf("      MAC: %02x:%02x:%02x:%02x:%02x:%02x\n", addr.addr_bytes[0],
               addr.addr_bytes[1], addr.addr_bytes[2], addr.addr_bytes[3],
               addr.addr_bytes[4], addr.addr_bytes[5]);
        printf("      链路: %s", link.link_status ? "启动" : "断开");
        if (link.link_status) {
            printf(" (%u Mbps, %s)\n", link.link_speed,
                   link.link_duplex == RTE_ETH_LINK_FULL_DUPLEX ? "全双工"
                                                                : "半双工");
        } else {
            printf("\n");
        }

        // RoCEv2特定能力检查
        printf("      RoCEv2能力:");
        bool udp_csum = dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_UDP_CKSUM;
        bool jumbo = dev_info.max_rx_pkt_len >= ROCEV2_JUMBO_FRAME_SIZE;

        if (udp_csum && jumbo) {
            printf(" ✓ 完全支持\n");
        } else if (udp_csum) {
            printf(" ⚠ 部分支持 (无巨帧)\n");
        } else if (jumbo) {
            printf(" ⚠ 部分支持 (无UDP校验和卸载)\n");
        } else {
            printf(" ✗ 不支持\n");
        }
    }
}

// 验证网卡配置（RoCEv2专用）
int validate_rocev2_port_configuration(uint16_t port) {
    if (port >= RTE_MAX_ETHPORTS) {
        return -1;
    }

    printf("验证网卡%u的RoCEv2配置...\n", port);

    // 检查网卡是否存在
    if (!rte_eth_dev_is_valid_port(port)) {
        fprintf(stderr, "网卡%u无效\n", port);
        return -1;
    }

    // 检查链路状态
    if (!check_port_link_status(port)) {
        fprintf(stderr, "网卡%u链路断开\n", port);
        return -1;
    }

    // 检查RoCEv2能力
    if (!check_rocev2_capabilities(port)) {
        printf("警告: 网卡%u不完全支持RoCEv2所需能力\n", port);
    }

    // 检查配置
    struct rte_eth_conf port_conf;
    rte_eth_dev_conf_get(port, &port_conf);

    printf("配置验证:\n");
    printf("  MTU: %u\n", port_conf.rxmode.mtu);
    printf("  最大包长度: %u\n", port_conf.rxmode.max_rx_pkt_len);
    printf("  RSS哈希函数: 0x%lx\n", port_conf.rx_adv_conf.rss_conf.rss_hf);

    if (port_conf.rxmode.max_rx_pkt_len < ROCEV2_JUMBO_FRAME_SIZE) {
        printf("警告: 最大包长度可能不足，建议配置为至少%d\n",
               ROCEV2_JUMBO_FRAME_SIZE);
    }

    if (!(port_conf.rx_adv_conf.rss_conf.rss_hf & RTE_ETH_RSS_UDP)) {
        printf("警告: RSS未配置为UDP哈希，可能影响负载均衡\n");
    }

    printf("网卡%u配置验证完成\n", port);
    return 0;
}
#include "network/network.h"
#include "config.h"
#include "rdma_defs.h"
#include "utils/debug.h"
#include <rte_ethdev.h>
#include <rte_flow.h>

static int configure_nic(uint16_t port, uint16_t rx_queue_id,
                         uint16_t tx_queue_id) {
    struct rte_eth_dev_info dev_info;
    struct rte_eth_conf port_conf = {0};

    int ret = rte_eth_dev_count_avail();
    if (ret == 0) {
        dbg_err("未发现可用的DPDK网卡\n");
        return ret;
    }

    ret = rte_eth_dev_info_get(port, &dev_info);
    if (ret != 0) {
        dbg_err("无法获取网卡%d信息\n", port);
        return ret;
    }

    // 接收模式配置
    port_conf.rxmode.mtu = SAFE_ROCEV2_MTU;
    port_conf.rxmode.offloads = 0;
    if (dev_info.rx_offload_capa & RTE_ETH_RX_OFFLOAD_CHECKSUM) {
        // port_conf.rxmode.offloads |= RTE_ETH_RX_OFFLOAD_CHECKSUM;
        //【修复点】：使用按位与(&)，网卡支持什么特性就开启什么特性，不强行越界开启
        port_conf.rxmode.offloads |=
            (dev_info.rx_offload_capa & RTE_ETH_RX_OFFLOAD_CHECKSUM);
    } // 接收 校验和卸载 功能启用

    // 发送模式配置
    port_conf.txmode.offloads = 0;
    if (dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_IPV4_CKSUM) {
        port_conf.txmode.offloads |= RTE_ETH_TX_OFFLOAD_IPV4_CKSUM;
    } // 发送 IPv4校验和卸载 功能启用
    if (dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_UDP_CKSUM) {
        port_conf.txmode.offloads |= RTE_ETH_TX_OFFLOAD_UDP_CKSUM;
    } // 发送 UDP校验和卸载 功能启用
    if (dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE) {
        // port_conf.txmode.offloads |= RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE;
        //【修复点】：同理，取交集
        port_conf.txmode.offloads |=
            (dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE);
    } // 发送 mbuf快速释放 功能启用

    // 网卡配置
    ret = rte_eth_dev_configure(port, QUEUE_COUNT, QUEUE_COUNT, &port_conf);
    if (ret != 0) {
        dbg_err("网卡配置失败\n");
        return ret;
    }
    // 显式调用API修改MTU
    // 直接修改port_conf.rxmode.mtu有时在rte_eth_dev_configure
    // 阶段会被某些网卡驱动忽略
    ret = rte_eth_dev_set_mtu(port, SAFE_ROCEV2_MTU);
    if (ret != 0) {
        dbg_err("接收模式MTU设置失败\n");
        return ret;
    }

    // 调整硬件环硬件描述符数量
    uint16_t rx_desc_count = NIC_RX_RING_SIZE;
    uint16_t tx_desc_count = NIC_TX_RING_SIZE;
    ret =
        rte_eth_dev_adjust_nb_rx_tx_desc(port, &rx_desc_count, &tx_desc_count);
    if (ret != 0) {
        dbg_err("网卡%d队列描述符数量调整失败\n", port);
        return ret;
    }

    // 配置接收队列
    struct rte_eth_rxconf rxconf = dev_info.default_rxconf;
    rxconf.offloads = port_conf.rxmode.offloads;
    ret = rte_eth_rx_queue_setup(port, rx_queue_id, NIC_RX_RING_SIZE,
                                 rte_eth_dev_socket_id(port), &rxconf,
                                 g_data.mbuf_pool);
    if (ret != 0) {
        dbg_err("接收队列设置失败\n");
        return ret;
    }

    // 配置发送队列
    struct rte_eth_txconf txconf = dev_info.default_txconf;
    txconf.offloads = port_conf.txmode.offloads;
    ret = rte_eth_tx_queue_setup(port, tx_queue_id, NIC_TX_RING_SIZE,
                                 rte_eth_dev_socket_id(port), &txconf);
    if (ret != 0) {
        dbg_err("发送队列设置失败\n");
        return ret;
    }

    return 0;
}

// 创建RoCEv2流规则
static int create_rocev2_flow_rule(uint16_t port_id, uint16_t rx_queue) {
    struct rte_flow_error error;

    // 属性：只匹配接收方向 (Ingress)
    struct rte_flow_attr attr = {.ingress = 1, .priority = 0};

    // 定义匹配模式 (Pattern)
    struct rte_flow_item pattern[4];

    // 1. 匹配 Ethernet (通配)
    pattern[0].type = RTE_FLOW_ITEM_TYPE_ETH;
    pattern[0].spec = NULL;
    pattern[0].mask = NULL;

    // 2. 匹配 IPv4 (严格匹配下一个协议为 UDP)
    struct rte_flow_item_ipv4 ipv4_spec = {.hdr.next_proto_id = IPPROTO_UDP};
    struct rte_flow_item_ipv4 ipv4_mask = {.hdr.next_proto_id = 0xFF};
    pattern[1].type = RTE_FLOW_ITEM_TYPE_IPV4;
    pattern[1].spec = &ipv4_spec;
    pattern[1].mask = &ipv4_mask;

    // 3. 匹配 UDP (严格匹配目的端口为 4791)
    struct rte_flow_item_udp udp_spec = {.hdr.dst_port =
                                             rte_cpu_to_be_16(ROCEV2_UDP_PORT)};
    struct rte_flow_item_udp udp_mask = {.hdr.dst_port = 0xFFFF};
    pattern[2].type = RTE_FLOW_ITEM_TYPE_UDP;
    pattern[2].spec = &udp_spec;
    pattern[2].mask = &udp_mask;

    // 4. 结束符
    pattern[3].type = RTE_FLOW_ITEM_TYPE_END;

    // 定义动作 (Action): 发送到指定的 RX Queue
    struct rte_flow_action action[2];
    struct rte_flow_action_queue queue_conf = {.index = rx_queue};

    action[0].type = RTE_FLOW_ACTION_TYPE_QUEUE;
    action[0].conf = &queue_conf;

    action[1].type = RTE_FLOW_ACTION_TYPE_END;

    // 【关键步骤】：先验证网卡硬件是否支持该规则
    int ret = rte_flow_validate(port_id, &attr, pattern, action, &error);
    if (ret != 0) {
        dbg_err("网卡%u硬件不支持该流规则校验: %s\n", port_id, error.message);
        return ret;
    }

    // 验证通过，正式下发创建
    struct rte_flow *flow =
        rte_flow_create(port_id, &attr, pattern, action, &error);
    if (!flow) {
        dbg_err("无法创建RoCEv2流规则: %s\n", error.message);
        return ERROR;
    }

    return 0;
}

int init_port(uint16_t port, uint16_t rx_queue_id, uint16_t tx_queue_id) {

    int ret = configure_nic(port, rx_queue_id, tx_queue_id);
    if (ret != 0)
        return ret;

    ret = rte_eth_dev_start(port);
    if (ret != 0) {
        dbg_err("网卡启动失败\n");
        return ret;
    }

    ret = rte_eth_promiscuous_enable(port);
    if (ret != 0) {
        dbg_err("混杂模式启动失败\n");
        return ret;
    }

    ret = create_rocev2_flow_rule(port, rx_queue_id);
    if (ret != 0) {
        dbg_err("RoCEv2硬件过滤流表下发失败，将接收所有镜像流量\n");
        // 前期测试报错可以先不return ret; 容忍它失败
    }

    return SUCCESS;
}

void close_port(uint16_t port) {
    // 获取并打印最终的统计信息
    struct rte_eth_stats stats;
    if (rte_eth_stats_get(port, &stats) == 0) {
        printf("网卡%u最终统计:\n", port);
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
            printf("  平均接收大小: %lubytes\n", avg_rx_size);
        }

        if (stats.opackets > 0) {
            uint64_t avg_tx_size = stats.obytes / stats.opackets;
            printf("  平均发送大小: %lubytes\n", avg_tx_size);
        }
    }

    // 停止网卡
    int ret = rte_eth_dev_stop(port);
    if (ret != 0) {
        dbg_err("停止网卡%u失败", port);
    }

    // 关闭网卡
    ret = rte_eth_dev_close(port);
    if (ret != 0) {
        dbg_err("网卡%u关闭失败\n", port);
    }
}
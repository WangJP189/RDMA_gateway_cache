#include <stdio.h>
#include <stdlib.h>
#include "pkt_shared.h"

// 最小实现的重传/释放/解析辅助函数，用于本地编译与功能测试。
// 真实实现应在用户态构建完整以太/IPv4/UDP/BTH/AETH 报文并通过 AF_PACKET 发送。

void retransmit_rdma_packet(const struct connection_key *key,
                            const unsigned char *data, int len,
                            uint32_t dest_qp, uint32_t psn)
{
    // 打印日志以便测试流程（真实实现需发送报文）
    (void)key; // 目前仅用于编译
    printf("[STUB] retransmit_rdma_packet: QP=%u PSN=%u len=%d\n", dest_qp, psn, len);
}

void free_cached_packets(struct cached_packet *head)
{
    struct cached_packet *cur = head;
    while (cur) {
        struct cached_packet *n = cur->next;
        free(cur->app_data);
        free(cur);
        cur = n;
    }
}

uint8_t get_aeth_syndrome(const struct aeth *a)
{
    if (!a) return 0;
    return a->syndrome;
}

uint32_t get_aeth_epsn(const struct aeth *a)
{
    if (!a) return 0;
    return (a->credit & 0x00FFFFFF);
}

/*
本程序演示如何使用原始UDP套接字发送RoCEv2报文（目前仅发送功能）
目前本程序仅做到相同设备内的回环发送，未实现完整的RDMA语义（如读写、注册内存等）
若验证是否成功发送RoCEv2报文，可使用以下命令监听回环设备进行抓包
sudo tcpdump -i lo -n udp port 4791 -vv

*/


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>  // 新增：解决close函数未声明的问题
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/udp.h>

// RoCEv2 BTH头结构（Base Transport Header）
struct roce_bth {
    uint8_t opcode;       // 操作码：0x00=SEND
    uint8_t flags;        // 标志位：0x00
    uint16_t qp_num;      // QP号（自定义，如0x1234）
    uint32_t psn;         // 序列号（自定义，如0x56789abc）
};

// RoCEv2 GRH头结构（Global Routing Header）
struct roce_grh {
    uint8_t ver_tc_flow[4];  // 版本(4bit)+流量类别(8bit)+流标签(20bit)：0x80000000
    uint8_t payload_len[2];  // 载荷长度（BTH+数据，网络字节序）
    uint8_t next_header;     // 下一层协议：0x11=UDP
    uint8_t hop_limit;       // 跳数限制：0x01
    uint8_t sgid[16];        // 源GID（自定义，如fe80::1）
    uint8_t dgid[16];        // 目标GID（自定义，如fe80::1，自环）
};

// 修正：删除多余的*phdr参数，参数列表恢复正确
uint16_t udp_checksum(struct iphdr *ip, struct udphdr *udp, uint8_t *payload, int payload_len) {
    uint32_t sum = 0;
    sum += (ip->saddr >> 16) & 0xffff;
    sum += ip->saddr & 0xffff;
    sum += (ip->daddr >> 16) & 0xffff;
    sum += ip->daddr & 0xffff;
    sum += htons(ip->protocol);
    sum += htons(ntohs(udp->len));
    sum += (udp->source >> 16) & 0xffff;
    sum += udp->source & 0xffff;
    sum += (udp->dest >> 16) & 0xffff;
    sum += udp->dest & 0xffff;
    sum += (udp->len >> 16) & 0xffff;
    sum += udp->len & 0xffff;
    for (int i=0; i<payload_len; i+=2) {
        sum += (payload[i] << 8) | (i+1 < payload_len ? payload[i+1] : 0);
    }
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return ~sum & 0xffff;
}

int main() {
    int sockfd;
    struct sockaddr_in dest_addr;
    // 构造RoCEv2报文（GRH + BTH + 数据）
    uint8_t roce_packet[1024];
    struct roce_grh *grh = (struct roce_grh *)roce_packet;
    struct roce_bth *bth = (struct roce_bth *)(roce_packet + sizeof(struct roce_grh));
    char *data = (char *)(roce_packet + sizeof(struct roce_grh) + sizeof(struct roce_bth));

    // 1. 创建UDP套接字（SOCK_DGRAM，系统自动处理IP和UDP头）
    sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sockfd < 0) {
        perror("socket create failed");
        return 1;
    }

    // 2. 配置目标地址（自环：你的eth1 IP + RoCEv2默认端口4791）
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(4791);  // RoCEv2标准端口（固定）
    dest_addr.sin_addr.s_addr = inet_addr("192.168.239.130");  // 你的eth1 IP，无需修改

    // 3. 填充GRH头（自定义GID，无需依赖RDMA设备）
    memset(grh, 0, sizeof(struct roce_grh));
    grh->ver_tc_flow[0] = 0x80;  // 版本=2（前4bit：0b1000），其余bit为0
    // 载荷长度 = BTH长度(8字节) + 数据长度（网络字节序，小端需转大端）
    uint16_t payload_len = sizeof(struct roce_bth) + strlen("RoCEv2 Raw Packet!111") + 1;
    grh->payload_len[0] = (payload_len >> 8) & 0xff;  // 高8位
    grh->payload_len[1] = payload_len & 0xff;         // 低8位
    grh->next_header = 0x11;     // 下一层协议：0x11=UDP
    grh->hop_limit = 0x01;       // 跳数限制：自环测试设为1
    // 自定义GID（源GID=目标GID，自环），格式：fe80::020c:29ff:fe10:01ff
    uint8_t gid[] = {0xfe,0x80,0x00,0x00,0x00,0x00,0x00,0x00,
                     0x02,0x0c,0x29,0xff,0xfe,0x10,0x01,0xff};
    memcpy(grh->sgid, gid, 16);  // 源GID
    memcpy(grh->dgid, gid, 16);  // 目标GID（自环，与源GID相同）

    // 4. 填充BTH头（RoCEv2核心控制字段）
    bth->opcode = 0x00;                  // 操作码：0x00=SEND（可靠连接发送）
    bth->flags = 0x00;                   // 标志位：无特殊需求设为0
    bth->qp_num = htons(0x1234);         // QP号（自定义，需转网络字节序）
    bth->psn = htonl(0x56789abc);        // 序列号（自定义，需转网络字节序）

    // 5. 填充数据载荷（自定义测试内容）
    strcpy(data, "RoCEv2 Raw Packet!111");
    // 计算总报文长度：GRH长度 + BTH长度 + 数据长度（含结束符）
    int total_len = sizeof(struct roce_grh) + sizeof(struct roce_bth) + strlen(data) + 1;

    // 6. 发送RoCEv2报文（通过UDP发送，系统自动添加IP和UDP头）
    int send_len = sendto(sockfd, roce_packet, total_len, 0, 
        (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (send_len < 0) {
        perror("sendto failed");
        close(sockfd);  // 关闭套接字，避免资源泄漏
        return 1;
    }

    // 发送成功，打印信息
    printf("✅ RoCEv2原始报文发送成功！\n");
    printf("📦 报文总长度：%d 字节\n", send_len);
    printf("📂 结构拆分：GRH(%lu字节) + BTH(%lu字节) + 数据(%lu字节)\n",
           sizeof(struct roce_grh), sizeof(struct roce_bth), strlen(data)+1);
    printf("💡 可通过Wireshark（过滤udp port 4791）查看报文详情\n");

    // 关闭套接字，释放资源
    close(sockfd);
    return 0;
}
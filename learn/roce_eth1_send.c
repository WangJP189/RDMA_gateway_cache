/*
本程序目标是通过eth1网卡发送RoCEv2报文到eth3网卡的4791端口。
但是目前只是用udp封装了RoCEv2报文，并没有真正实现RDMA功能。只能做到模拟RoCEv2报文的发送和接收。

编译命令：
gcc -o roce_eth1_send roce_eth1_send.c

运行命令（先在一个终端运行：将接收程序，再在另一个终端运行发送程序）：
sudo ./roce_eth1_send

*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

struct roce_bth { uint8_t opcode, flags; uint16_t qp_num; uint32_t psn; };
struct roce_grh { 
    uint8_t ver_tc_flow[4], payload_len[2], next_header, hop_limit; 
    uint8_t sgid[16], dgid[16]; 
};

int main() {
    int sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sockfd < 0) { perror("socket failed"); return 1; }

    // 绑定eth1的IP（确保从eth1发送）
    struct sockaddr_in local_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(0), // 随机端口
        .sin_addr.s_addr = inet_addr("192.168.239.130") // 你的eth1 IP
    };
    if (bind(sockfd, (struct sockaddr *)&local_addr, sizeof(local_addr)) < 0) {
        perror("bind eth1 failed"); close(sockfd); return 1;
    }

    // 目标地址：eth3的IP和4791端口
    struct sockaddr_in dest_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(4791),
        .sin_addr.s_addr = inet_addr("192.168.239.133") // 你的eth3 IP
    };

    // 构造RoCEv2报文
    uint8_t send_buf[1024];
    struct roce_grh *grh = (struct roce_grh *)send_buf;
    struct roce_bth *bth = (struct roce_bth *)(send_buf + sizeof(struct roce_grh));
    char *data = (char *)(send_buf + sizeof(struct roce_grh) + sizeof(struct roce_bth));

    // 填充GRH（GID自定义，匹配格式即可）
    uint8_t sgid[] = {0xfe,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x02,0x0c,0x29,0xff,0xfe,0x10,0x01,0xff};
    uint8_t dgid[] = {0xfe,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x02,0x0c,0x29,0xff,0xfe,0x10,0x01,0x13};
    memcpy(grh->sgid, sgid, 16); memcpy(grh->dgid, dgid, 16);
    grh->ver_tc_flow[0] = 0x80; grh->next_header = 0x11; grh->hop_limit = 0x01;

    // 填充BTH
    bth->opcode = 0x00; bth->qp_num = htons(0x1234); bth->psn = htonl(0x56789abc);

    // 填充数据
    strcpy(data, "eth1→eth3 RoCEv2测试成功！");
    uint16_t payload_len = sizeof(struct roce_bth) + strlen(data) + 1;
    grh->payload_len[0] = (payload_len >> 8) & 0xff; grh->payload_len[1] = payload_len & 0xff;

    // 发送
    int send_len = sendto(sockfd, send_buf, sizeof(struct roce_grh) + payload_len, 0,
                         (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (send_len < 0) { perror("send failed"); close(sockfd); return 1; }

    printf("✅ 发送端（eth1）已发送！\n");
    printf("192.168.239.130 → 192.168.239.133:4791\n");
    printf("报文长度：%d字节\n", send_len);

    close(sockfd);
    return 0;
}
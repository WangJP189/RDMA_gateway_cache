/*
本程序目标是接收从eth1发送过来的RoCEv2报文
但是目前只是用udp封装了RoCEv2报文，并没有真正实现RDMA功能。只能做到模拟RoCEv2报文的发送和接收。

编译命令：
gcc -o roce_eth3_recv roce_eth3_recv.c

运行命令（先在一个终端运行接收程序，再在另一个终端运行发送程序）：
sudo ./roce_eth3_recv
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

    // 绑定eth3的IP和4791端口
    struct sockaddr_in local_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(4791),
        .sin_addr.s_addr = inet_addr("192.168.239.133") // 你的eth3 IP
    };
    if (bind(sockfd, (struct sockaddr *)&local_addr, sizeof(local_addr)) < 0) {
        perror("bind eth3 failed"); close(sockfd); return 1;
    }
    printf("✅ 接收端（eth3）启动：192.168.239.133:4791 等待报文...\n");

    // 接收报文
    uint8_t recv_buf[1024];
    struct sockaddr_in src_addr;
    socklen_t src_len = sizeof(src_addr);
    int recv_len = recvfrom(sockfd, recv_buf, sizeof(recv_buf), 0, &src_addr, &src_len);
    if (recv_len < 0) { perror("recv failed"); close(sockfd); return 1; }

    // 解析并打印
    struct roce_grh *grh = (struct roce_grh *)recv_buf;
    struct roce_bth *bth = (struct roce_bth *)(recv_buf + sizeof(struct roce_grh));
    char *data = (char *)(recv_buf + sizeof(struct roce_grh) + sizeof(struct roce_bth));
    printf("\n📥 收到eth1的RoCEv2报文！\n");
    printf("发送端IP：%s\n", inet_ntoa(src_addr.sin_addr)); // 应显示192.168.239.130
    printf("BTH：Opcode=0x%02x, QP=0x%04x, PSN=0x%08x\n",
           bth->opcode, ntohs(bth->qp_num), ntohl(bth->psn));
    printf("数据：%s\n", data);

    close(sockfd);
    return 0;
}
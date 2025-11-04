//本程序演示如何使用原始UDP套接字发送和接收RoCEv2报文
//目前本程序仅做到相同设备内的回环发送和接收，未实现完整的RDMA语义（如读写、注册内存等）


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <linux/ip.h>
#include <linux/udp.h>

// 复用之前的GRH/BTH结构体
struct roce_bth { uint8_t opcode, flags; uint16_t qp_num; uint32_t psn; };
struct roce_grh { uint8_t ver_tc_flow[4], payload_len[2], next_header, hop_limit; uint8_t sgid[16], dgid[16]; };

int main() {
    int sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sockfd < 0) { perror("socket failed"); return 1; }

    // 关键：绑定UDP/4791端口，用于接收报文
    struct sockaddr_in local_addr;
    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_port = htons(4792);  // 绑定RoCEv2端口
    local_addr.sin_addr.s_addr = htonl(INADDR_ANY);  // 监听所有本地IP
    if (bind(sockfd, (struct sockaddr *)&local_addr, sizeof(local_addr)) < 0) {
        perror("bind failed"); close(sockfd); return 1;
    }
    printf("已绑定UDP/4791端口，等待接收RoCEv2报文...\n");

    // 1. 先发送报文
    struct sockaddr_in dest_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(4792),
        .sin_addr.s_addr = inet_addr("192.168.239.130")
    };
    uint8_t send_buf[1024];
    struct roce_grh *send_grh = (struct roce_grh *)send_buf;
    struct roce_bth *send_bth = (struct roce_bth *)(send_buf + sizeof(struct roce_grh));
    char *send_data = (char *)(send_buf + sizeof(struct roce_grh) + sizeof(struct roce_bth));
    
    // 填充发送报文（同之前逻辑）
    uint8_t gid[] = {0xfe,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x02,0x0c,0x29,0xff,0xfe,0x10,0x01,0xff};
    memcpy(send_grh->sgid, gid, 16); memcpy(send_grh->dgid, gid, 16);
    send_grh->ver_tc_flow[0] = 0x80; send_grh->next_header = 0x11; send_grh->hop_limit = 0x01;
    uint16_t payload_len = sizeof(struct roce_bth) + strlen("RoCEv2 Send&Recv!") + 1;
    send_grh->payload_len[0] = (payload_len >> 8) & 0xff; send_grh->payload_len[1] = payload_len & 0xff;
    send_bth->opcode = 0x00; send_bth->qp_num = htons(0x1234); send_bth->psn = htonl(0x56789abc);
    strcpy(send_data, "RoCEv2 Send&Recv!");
    int send_len = sendto(sockfd, send_buf, sizeof(struct roce_grh) + payload_len, 0, 
        (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    printf("已发送RoCEv2报文，长度：%d字节\n", send_len);

    // 2. 接收报文
    uint8_t recv_buf[1024];
    struct sockaddr_in src_addr;
    socklen_t src_len = sizeof(src_addr);
    int recv_len = recvfrom(sockfd, recv_buf, sizeof(recv_buf), 0, 
        (struct sockaddr *)&src_addr, &src_len);
    if (recv_len < 0) { perror("recv failed"); close(sockfd); return 1; }

    // 解析接收的报文
    struct roce_grh *recv_grh = (struct roce_grh *)recv_buf;
    struct roce_bth *recv_bth = (struct roce_bth *)(recv_buf + sizeof(struct roce_grh));
    char *recv_data = (char *)(recv_buf + sizeof(struct roce_grh) + sizeof(struct roce_bth));
    printf("\n✅ 接收RoCEv2报文成功！\n");
    printf("📤 发送方IP：%s\n", inet_ntoa(src_addr.sin_addr));
    printf("📦 接收长度：%d字节\n", recv_len);
    printf("🔧 BTH信息：Opcode=0x%02x, QP号=0x%04x, PSN=0x%08x\n",
           recv_bth->opcode, ntohs(recv_bth->qp_num), ntohl(recv_bth->psn));
    printf("📄 数据载荷：%s\n", recv_data);

    close(sockfd);
    return 0;
}
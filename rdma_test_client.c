/*
在虚拟机135（eth1为192.168.239.135）上运行此客户端代码：
gcc rdma_test_client.c -o rdma_test_client -lrdmacm -libverbs

*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <rdma/rdma_cma.h>
#include <infiniband/verbs.h>

#define SERVER_IP "192.168.239.130"
#define PORT "18515"
#define BUFFER_SIZE 65536

int main() {
    struct rdma_event_channel *ec;
    struct rdma_cm_id *id;
    struct rdma_conn_param conn_param = {0};
    struct ibv_mr *mr;
    char *buffer;
    int ret;

    ec = rdma_create_event_channel();
    if (!ec) {
        perror("rdma_create_event_channel");
        return 1;
    }

    if (rdma_create_id(ec, &id, NULL, RDMA_PS_TCP)) {
        perror("rdma_create_id");
        return 1;
    }

    // 解析服务器地址
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(18515);
    inet_pton(AF_INET, SERVER_IP, &addr.sin_addr);

    if (rdma_resolve_addr(id, NULL, (struct sockaddr*)&addr, 2000)) {
        perror("rdma_resolve_addr");
        return 1;
    }

    printf("正在连接到服务器 %s:%s...\n", SERVER_IP, PORT);

    // 事件循环
    struct rdma_cm_event *event;
    while (rdma_get_cm_event(ec, &event) == 0) {
        if (event->event == RDMA_CM_EVENT_ADDR_RESOLVED) {
            rdma_resolve_route(id, 2000);
        } else if (event->event == RDMA_CM_EVENT_ROUTE_RESOLVED) {
            // 设置QP
            struct ibv_qp_init_attr qp_attr = {0};
            qp_attr.qp_type = IBV_QPT_RC;
            qp_attr.cap.max_send_wr = 10;
            qp_attr.cap.max_recv_wr = 10;
            qp_attr.cap.max_send_sge = 1;
            qp_attr.cap.max_recv_sge = 1;
            
            id->recv_cq = ibv_create_cq(id->verbs, 10, NULL, NULL, 0);
            id->send_cq = ibv_create_cq(id->verbs, 10, NULL, NULL, 0);
            qp_attr.recv_cq = id->recv_cq;
            qp_attr.send_cq = id->send_cq;
            
            if (rdma_create_qp(id, id->pd, &qp_attr)) {
                perror("rdma_create_qp");
                continue;
            }
            
            // 分配并准备接收缓冲区
            buffer = malloc(BUFFER_SIZE);
            memset(buffer, 0, BUFFER_SIZE);
            mr = ibv_reg_mr(id->pd, buffer, BUFFER_SIZE, 
                           IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE);
            
            // 投递接收请求
            struct ibv_recv_wr wr, *bad_wr;
            struct ibv_sge sge;
            
            sge.addr = (uintptr_t)buffer;
            sge.length = BUFFER_SIZE;
            sge.lkey = mr->lkey;
            
            wr.wr_id = (uintptr_t)buffer;
            wr.next = NULL;
            wr.sg_list = &sge;
            wr.num_sge = 1;
            
            ibv_post_recv(id->qp, &wr, &bad_wr);
            
            // 发起连接
            conn_param.initiator_depth = 1;
            conn_param.retry_count = 3;
            rdma_connect(id, &conn_param);
            
        } else if (event->event == RDMA_CM_EVENT_ESTABLISHED) {
            printf("连接已建立，等待数据...\n");
            
            // 等待接收完成
            struct ibv_wc wc;
            while (ibv_poll_cq(id->recv_cq, 1, &wc) == 0) {
                usleep(1000);
            }
            
            if (wc.status == IBV_WC_SUCCESS && wc.opcode == IBV_WC_RECV) {
                printf("收到数据: %s\n", buffer);
            }
            
            break;
        }
        
        rdma_ack_cm_event(event);
    }

    return 0;
}
/*
在虚拟机130（eth1为192.168.239.130）上运行此服务器代码：
gcc rdma_test_server.c -o rdma_test_server -lrdmacm -libverbs

*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <rdma/rdma_cma.h>
#include <infiniband/verbs.h>

#define PORT "4791"
#define BUFFER_SIZE 65536

int main() {
    struct rdma_event_channel *ec;
    struct rdma_cm_id *listener, *id;
    struct rdma_conn_param conn_param = {0};
    struct ibv_mr *mr;
    char *buffer;
    int ret;

    // 创建事件通道
    ec = rdma_create_event_channel();
    if (!ec) {
        perror("rdma_create_event_channel");
        return 1;
    }

    // 创建监听ID
    if (rdma_create_id(ec, &listener, NULL, RDMA_PS_TCP)) {
        perror("rdma_create_id");
        return 1;
    }

    // 解析并绑定地址
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(18515);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (rdma_bind_addr(listener, (struct sockaddr*)&addr)) {
        perror("rdma_bind_addr");
        return 1;
    }

    // 开始监听
    if (rdma_listen(listener, 10)) {
        perror("rdma_listen");
        return 1;
    }

    printf("RDMA测试服务器监听端口 18515...\n");

    // 处理连接请求
    struct rdma_cm_event *event;
    while (rdma_get_cm_event(ec, &event) == 0) {
        if (event->event == RDMA_CM_EVENT_CONNECT_REQUEST) {
            id = event->id;
            
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
            
            // 分配缓冲区
            buffer = malloc(BUFFER_SIZE);
            strcpy(buffer, "测试数据从服务器发送到客户端");
            
            // 注册MR
            mr = ibv_reg_mr(id->pd, buffer, BUFFER_SIZE, 
                           IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE);
            
            // 接受连接
            conn_param.responder_resources = 1;
            conn_param.initiator_depth = 1;
            rdma_accept(id, &conn_param);
            
            printf("连接已接受，准备发送数据...\n");
            
            // 发送数据
            struct ibv_sge sge;
            struct ibv_send_wr wr, *bad_wr;
            
            sge.addr = (uintptr_t)buffer;
            sge.length = strlen(buffer) + 1;
            sge.lkey = mr->lkey;
            
            memset(&wr, 0, sizeof(wr));
            wr.wr_id = (uintptr_t)buffer;
            wr.sg_list = &sge;
            wr.num_sge = 1;
            wr.opcode = IBV_WR_SEND;
            wr.send_flags = IBV_SEND_SIGNALED;
            
            if (ibv_post_send(id->qp, &wr, &bad_wr)) {
                perror("ibv_post_send");
            } else {
                printf("数据已发送: %s\n", buffer);
            }
        }
        
        rdma_ack_cm_event(event);
    }

    return 0;
}
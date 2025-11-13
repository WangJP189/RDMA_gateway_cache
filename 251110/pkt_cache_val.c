#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <rdma/rdma_cma.h>
#include <infiniband/verbs.h>
#include <netinet/in.h>

// 使用头文件而非直接包含源文件（避免main函数冲突）
#include "pkt_cache.h"

// 全局标志：控制线程退出
static volatile int running = 1;

// 信号处理函数：捕获Ctrl+C退出
void signal_handler(int sig) {
    running = 0;
    printf("\n收到退出信号，正在清理...\n");
}

// 定期打印缓存状态的线程函数
void *status_printer(void *arg) {
    while (running) {
        sleep(5);  // 每5秒打印一次状态
        print_all_connections_status();
        cleanup_expired_connections();  // 同时清理过期连接
    }
    return NULL;
}

// RDMA连接监听线程：捕获ib_send_bw的通信并缓存
void *rdma_listener(void *arg) {
    struct rdma_event_channel *ec;
    struct rdma_cm_id *listener, *id;
    struct rdma_conn_param conn_param = {0};
    struct sockaddr_in sin;
    struct ibv_qp_init_attr qp_attr = {0};
    struct ibv_comp_channel *comp_chan;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    int ret;

    // 初始化事件通道
    ec = rdma_create_event_channel();
    if (!ec) {
        perror("rdma_create_event_channel failed");
        return NULL;
    }

    // 创建监听ID
    if (rdma_create_id(ec, &listener, NULL, RDMA_PS_TCP)) {
        perror("rdma_create_id failed");
        rdma_destroy_event_channel(ec);
        return NULL;
    }

    // 绑定到默认端口（ib_send_bw默认使用18515）
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(18515);
    sin.sin_addr.s_addr = INADDR_ANY;

    if (rdma_bind_addr(listener, (struct sockaddr *)&sin)) {
        perror("rdma_bind_addr failed");
        rdma_destroy_id(listener);
        rdma_destroy_event_channel(ec);
        return NULL;
    }

    // 在 rdma_listener 函数中，初始化QP属性前添加
    if (!listener->verbs) {
        perror("listener->verbs is NULL (设备上下文无效)");
        rdma_destroy_id(listener);
        rdma_destroy_event_channel(ec);
        return NULL;
    }
    printf("成功获取设备上下文: %p\n", listener->verbs);  // 确认非空

    // 初始化QP属性
    qp_attr.qp_type = IBV_QPT_RC;
    comp_chan = ibv_create_comp_channel(listener->verbs);
    if (!comp_chan) {
        perror("ibv_create_comp_channel failed");
        // 打印更详细的错误信息
        fprintf(stderr, "错误原因: 设备上下文无效或rxe设备未激活\n");
        return NULL;
    }
    ibv_req_notify_cq(cq, 0);

    qp_attr.send_cq = cq;
    qp_attr.recv_cq = cq;
    qp_attr.cap.max_send_wr = 128;
    qp_attr.cap.max_recv_wr = 128;
    qp_attr.cap.max_send_sge = 1;
    qp_attr.cap.max_recv_sge = 1;

    // 开始监听连接
    if (rdma_listen(listener, 10)) {  // 最大10个等待连接
        perror("rdma_listen failed");
        rdma_destroy_id(listener);
        rdma_destroy_event_channel(ec);
        return NULL;
    }

    printf("RDMA缓存监听已启动，等待ib_send_bw连接...\n");

    // 处理连接事件
    struct rdma_cm_event *event;
    while (running) {
        if (rdma_get_cm_event(ec, &event)) {
            perror("rdma_get_cm_event failed");
            break;
        }

        if (event->event == RDMA_CM_EVENT_CONNECT_REQUEST) {
            id = event->id;
            
            // 修复：使用id->pd而非id->verbs（rdma_create_qp第二个参数应为ibv_pd*）
            if (rdma_create_qp(id, id->pd, &qp_attr)) {
                perror("rdma_create_qp failed");
                rdma_reject(id, NULL, 0);
                rdma_ack_cm_event(event);
                continue;
            }
            qp = id->qp;

            // 准备接收缓冲区
            char *recv_buf = malloc(65536);  // 最大支持64KB报文
            struct ibv_recv_wr wr, *bad_wr;
            struct ibv_sge sge;

            sge.addr = (uint64_t)recv_buf;
            sge.length = 65536;
            sge.lkey = ibv_reg_mr(id->pd, recv_buf, 65536, 
                                 IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | 
                                 IBV_ACCESS_REMOTE_WRITE)->lkey;

            wr.wr_id = 0;
            wr.next = NULL;
            wr.sg_list = &sge;
            wr.num_sge = 1;
            ibv_post_recv(qp, &wr, &bad_wr);

            // 接受连接
            conn_param.responder_resources = 1;
            conn_param.initiator_depth = 1;
            conn_param.retry_count = 3;
            if (rdma_accept(id, &conn_param)) {
                perror("rdma_accept failed");
                rdma_destroy_qp(id);
                rdma_reject(id, NULL, 0);
                rdma_ack_cm_event(event);
                continue;
            }

            printf("已建立RDMA连接，开始缓存数据包...\n");

            // 处理数据接收
            while (running) {
                // 修复：包含ibv_cq_event的定义（需要正确引入verbs.h）
                struct ibv_cq *cq_ptr;
                void *cq_ctx;
                struct ibv_wc wc;

                // 新版 API：通过参数返回 CQ 指针和上下文，无需 ibv_cq_event 结构体
                ret = ibv_get_cq_event(comp_chan, &cq_ptr, &cq_ctx);

                if (ret) break;

                ibv_ack_cq_events(cq, 1);
                ibv_req_notify_cq(cq, 0);

                while (ibv_poll_cq(cq, 1, &wc)) {
                    if (wc.status != IBV_WC_SUCCESS) {
                        printf("WC错误: %s\n", ibv_wc_status_str(wc.status));
                        break;
                    }

                    if (wc.opcode == IBV_WC_RECV) {
                        // 获取本地和远程地址信息
                        struct sockaddr_in *local_addr = 
                            (struct sockaddr_in *)rdma_get_local_addr(id);
                        struct sockaddr_in *remote_addr = 
                            (struct sockaddr_in *)rdma_get_peer_addr(id);

                        // 缓存接收的数据包
                        add_to_connection_cache(
                            inet_ntoa(remote_addr->sin_addr),  // 源IP（发送端）
                            inet_ntoa(local_addr->sin_addr),   // 目的IP（接收端）
                            ntohs(remote_addr->sin_port),      // 源端口
                            ntohs(local_addr->sin_port),       // 目的端口
                            qp->qp_num,                        // 本地QP号
                            wc.qp_num,                         // 远程QP号
                            0,                                 // 服务类型
                            0xffff,                            // PKey
                            wc.wr_id,                          // PSN（这里用WR_ID模拟，实际应从报文中提取）
                            (unsigned char *)recv_buf, 
                            wc.byte_len
                        );

                        // 重新投递接收请求
                        ibv_post_recv(qp, &wr, &bad_wr);
                    }
                }
            }

            free(recv_buf);
            rdma_destroy_qp(id);
        } else if (event->event == RDMA_CM_EVENT_DISCONNECTED) {
            printf("连接已断开\n");
            rdma_destroy_id(event->id);
        }

        rdma_ack_cm_event(event);
    }

    // 清理资源
    rdma_destroy_id(listener);
    rdma_destroy_event_channel(ec);
    ibv_destroy_cq(cq);
    ibv_destroy_comp_channel(comp_chan);
    return NULL;
}

int main() {
    // 注册信号处理
    signal(SIGINT, signal_handler);

    // 初始化缓存管理器
    g_cache_mgr = init_cache_manager(
        1024,    // 哈希表大小
        10,     // 最大连接数
        100,   // 每连接最大报文数
        10,     // 每连接最大字节数(MB)
        60       // 连接超时时间(秒)
    );
    if (!g_cache_mgr) {
        fprintf(stderr, "初始化缓存管理器失败：init_cache_manager 返回 NULL\n");
        return 1;
    }
    printf("初始化缓存管理器成功：%p\n", g_cache_mgr);  // 打印地址，确认非空

    // 创建状态打印线程
    pthread_t printer_thread;
    if (pthread_create(&printer_thread, NULL, status_printer, NULL) != 0) {
        perror("创建状态打印线程失败");
        return 1;
    }

    // 创建RDMA监听线程
    pthread_t listener_thread;
    if (pthread_create(&listener_thread, NULL, rdma_listener, NULL) != 0) {
        perror("创建RDMA监听线程失败");
        pthread_join(printer_thread, NULL);
        return 1;
    }

    // 等待线程结束
    pthread_join(listener_thread, NULL);
    pthread_join(printer_thread, NULL);

    printf("程序已退出\n");
    return 0;
}
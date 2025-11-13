/*
编译命令：
gcc -g pkt_cache_val.c pkt_cache.o -o pkt_cache_val -lpthread -lrdmacm -libverbs

运行命令：
sudo ./pkt_cache_val

使用debug运行命令：
sudo gdb ./pkt_cache_val

(gdb) run
(gdb) backtrace
(gdb) exit

*/



#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <rdma/rdma_cma.h>
#include <infiniband/verbs.h>
#include <netinet/in.h>
#include <ifaddrs.h>

// 使用头文件而非直接包含源文件（避免main函数冲突）
#include "pkt_cache.h"

// 全局标志：控制线程退出
static volatile int running = 1;

// 信号处理函数：捕获Ctrl+C退出
void signal_handler(int sig) {
    running = 0;
    printf("\n收到退出信号，正在清理...\n");
    exit(0);
    
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

static struct ibv_context* get_ibv_context_by_name(const char *device_name) {
    struct ibv_device **dev_list;
    struct ibv_context *ctx = NULL;
    int num_devices;

    dev_list = ibv_get_device_list(&num_devices);
    if (!dev_list) {
        perror("ibv_get_device_list failed");
        return NULL;
    }

    // 查找目标设备
    for (int i = 0; i < num_devices; i++) {
        if (strcmp(ibv_get_device_name(dev_list[i]), device_name) == 0) {
            // 打开设备上下文
            ctx = ibv_open_device(dev_list[i]);
            if (!ctx) {
                perror("ibv_open_device failed");
            }
            break;
        }
    }

    ibv_free_device_list(dev_list);  // 释放设备列表
    return ctx;
}

// RDMA连接监听线程：捕获ib_send_bw的通信并缓存
void *rdma_listener(void *arg) {
    char *device = (char *)arg;
    struct rdma_event_channel *ec;
    struct rdma_cm_id *listener, *id;
    struct rdma_conn_param conn_param = {0};
    struct sockaddr_in sin;
    struct ibv_qp_init_attr qp_attr = {0};
    struct ibv_comp_channel *comp_chan;
    struct ibv_cq *cq = NULL;
    struct ibv_qp *qp;
    int ret;

    // 初始化事件通道
    ec = rdma_create_event_channel();
    if (!ec) {
        perror("rdma_create_event_channel failed");
        return NULL;
    }

    // 创建监听ID
    if (rdma_create_id(ec, &listener, NULL, RDMA_PS_UDP)) {
        perror("rdma_create_id failed");
        rdma_destroy_event_channel(ec);
        return NULL;
    }

    // 关键修改1：如果指定了设备名，获取并关联设备上下文
    struct ibv_context *verbs = NULL;
    if (device && strcmp(device, "") != 0) {
        verbs = get_ibv_context_by_name(device);
        if (!verbs) {
            fprintf(stderr, "无法获取设备 %s 的上下文\n", device);
            rdma_destroy_id(listener);
            rdma_destroy_event_channel(ec);
            return NULL;
        }
        // 手动关联设备上下文到监听ID
        listener->verbs = verbs;
    }

    // 绑定到指定端口和地址
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(18515);
    sin.sin_addr.s_addr = INADDR_ANY;  // 绑定到所有地址（设备已通过verbs指定）

    // // 如果指定了设备，尝试绑定到设备的IP（可选，不影响设备上下文）
    // if (device && strcmp(device, "") != 0) {
    //     struct ifaddrs *ifaddr, *ifa;
    //     if (getifaddrs(&ifaddr) == 0) {
    //         for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
    //             if (!ifa->ifa_addr) continue;
    //             if (ifa->ifa_addr->sa_family == AF_INET && strcmp(ifa->ifa_name, device) == 0) {
    //                 struct sockaddr_in *sa = (struct sockaddr_in *)ifa->ifa_addr;
    //                 sin.sin_addr = sa->sin_addr;  // 绑定到设备的IP
    //                 break;
    //             }
    //         }
    //         freeifaddrs(ifaddr);
    //     } else {
    //         perror("getifaddrs failed");
    //     }
    // }

    // 绑定地址（此时listener->verbs已通过设备名关联）
    if (rdma_bind_addr(listener, (struct sockaddr *)&sin)) {
        perror("rdma_bind_addr failed");
        if (verbs) ibv_close_device(verbs);  // 释放设备上下文
        rdma_destroy_id(listener);
        rdma_destroy_event_channel(ec);
        return NULL;
    }

    // 验证设备上下文（此时应非空）
    if (!listener->verbs) {
        fprintf(stderr, "listener->verbs is NULL (设备上下文无效)：可能设备名错误或未加载驱动\n");
        if (verbs) ibv_close_device(verbs);
        rdma_destroy_id(listener);
        rdma_destroy_event_channel(ec);
        return NULL;
    }
    printf("成功获取设备上下文: %p (设备名: %s)\n", listener->verbs, device ? device : "默认设备");

    // 初始化QP属性
    qp_attr.qp_type = IBV_QPT_RC;
    
    // 创建完成通道
    comp_chan = ibv_create_comp_channel(listener->verbs);
    if (!comp_chan) {
        perror("ibv_create_comp_channel failed");
        rdma_destroy_id(listener);
        rdma_destroy_event_channel(ec);
        return NULL;
    }

    // 创建完成队列（CQ）
    cq = ibv_create_cq(listener->verbs, 128, NULL, comp_chan, 0);
    if (!cq) {
        perror("ibv_create_cq failed");
        ibv_destroy_comp_channel(comp_chan);
        rdma_destroy_id(listener);
        rdma_destroy_event_channel(ec);
        return NULL;
    }

    // 注册CQ通知（必须在CQ创建后调用）
    if (ibv_req_notify_cq(cq, 0)) {
        perror("ibv_req_notify_cq failed");
        ibv_destroy_cq(cq);
        ibv_destroy_comp_channel(comp_chan);
        rdma_destroy_id(listener);
        rdma_destroy_event_channel(ec);
        return NULL;
    }

    // 设置QP的CQ属性
    qp_attr.send_cq = cq;
    qp_attr.recv_cq = cq;
    qp_attr.cap.max_send_wr = 128;
    qp_attr.cap.max_recv_wr = 128;
    qp_attr.cap.max_send_sge = 1;
    qp_attr.cap.max_recv_sge = 1;

    // 开始监听连接（最大10个等待连接）
    if (rdma_listen(listener, 10)) {
        perror("rdma_listen failed");
        ibv_destroy_cq(cq);
        ibv_destroy_comp_channel(comp_chan);
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
        printf("收到RDMA事件：%s\n", rdma_event_str(event->event));  // 调试用
        
        if (event->event == RDMA_CM_EVENT_CONNECT_REQUEST) {
            id = event->id;
            
            // 创建QP（使用正确的保护域）
            if (rdma_create_qp(id, id->pd, &qp_attr)) {
                perror("rdma_create_qp failed");
                rdma_reject(id, NULL, 0);
                rdma_ack_cm_event(event);
                continue;
            }
            qp = id->qp;

            // 准备接收缓冲区（64KB）
            char *recv_buf = malloc(65536);
            if (!recv_buf) {
                perror("malloc recv_buf failed");
                rdma_destroy_qp(id);
                rdma_reject(id, NULL, 0);
                rdma_ack_cm_event(event);
                continue;
            }

            // 注册内存区域（MR）
            struct ibv_mr *mr = ibv_reg_mr(
                id->pd,
                recv_buf,
                65536,
                IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE
            );
            if (!mr) {
                perror("ibv_reg_mr failed");
                free(recv_buf);
                rdma_destroy_qp(id);
                rdma_reject(id, NULL, 0);
                rdma_ack_cm_event(event);
                continue;
            }

            // 初始化接收WR
            struct ibv_recv_wr wr, *bad_wr;
            struct ibv_sge sge;
            sge.addr = (uint64_t)recv_buf;
            sge.length = 65536;
            sge.lkey = mr->lkey;

            wr.wr_id = (uint64_t)recv_buf;  // 用缓冲区地址作为WR_ID标识
            wr.next = NULL;
            wr.sg_list = &sge;
            wr.num_sge = 1;

            // 投递接收请求
            if (ibv_post_recv(qp, &wr, &bad_wr)) {
                perror("ibv_post_recv failed");
                ibv_dereg_mr(mr);
                free(recv_buf);
                rdma_destroy_qp(id);
                rdma_reject(id, NULL, 0);
                rdma_ack_cm_event(event);
                continue;
            }

            // 接受连接
            conn_param.responder_resources = 1;
            conn_param.initiator_depth = 1;
            conn_param.retry_count = 3;
            if (rdma_accept(id, &conn_param)) {
                perror("rdma_accept failed");
                ibv_dereg_mr(mr);
                free(recv_buf);
                rdma_destroy_qp(id);
                rdma_reject(id, NULL, 0);
                rdma_ack_cm_event(event);
                continue;
            }

            printf("已建立RDMA连接，开始缓存数据包...\n");

            // 处理数据接收循环
            while (running) {
                struct ibv_cq *cq_ptr;
                void *cq_ctx;
                struct ibv_wc wc;

                // 获取CQ事件
                ret = ibv_get_cq_event(comp_chan, &cq_ptr, &cq_ctx);
                if (ret) {
                    perror("ibv_get_cq_event failed");
                    break;
                }

                // 确认CQ事件
                ibv_ack_cq_events(cq, 1);
                if (ibv_req_notify_cq(cq, 0)) {
                    perror("ibv_req_notify_cq failed");
                    break;
                }

                // 轮询完成队列
                while (ibv_poll_cq(cq, 1, &wc) > 0) {
                    if (wc.status != IBV_WC_SUCCESS) {
                        printf("WC错误: %s\n", ibv_wc_status_str(wc.status));
                        break;
                    }

                    if (wc.opcode == IBV_WC_RECV) {
                        // 获取地址信息
                        struct sockaddr_in *local_addr = (struct sockaddr_in *)rdma_get_local_addr(id);
                        struct sockaddr_in *remote_addr = (struct sockaddr_in *)rdma_get_peer_addr(id);

                        // 缓存数据包
                        add_to_connection_cache(
                            inet_ntoa(remote_addr->sin_addr),
                            inet_ntoa(local_addr->sin_addr),
                            ntohs(remote_addr->sin_port),
                            ntohs(local_addr->sin_port),
                            qp->qp_num,
                            wc.qp_num,
                            0,
                            0xffff,
                            wc.wr_id,  // 注意：实际PSN需从报文中解析，这里仅为示例
                            (unsigned char *)recv_buf,
                            wc.byte_len
                        );

                        // 重新投递接收请求
                        if (ibv_post_recv(qp, &wr, &bad_wr)) {
                            perror("ibv_post_recv failed (loop)");
                            break;
                        }
                    }
                }
            }

            // 清理当前连接资源
            ibv_dereg_mr(mr);
            free(recv_buf);
            rdma_destroy_qp(id);

        } else if (event->event == RDMA_CM_EVENT_DISCONNECTED) {
            printf("连接已断开\n");
            rdma_destroy_id(event->id);
        }

        rdma_ack_cm_event(event);
    }

    // 清理全局资源
    if (verbs) ibv_close_device(verbs);
    rdma_destroy_id(listener);
    rdma_destroy_event_channel(ec);
    if (cq) ibv_destroy_cq(cq);
    if (comp_chan) ibv_destroy_comp_channel(comp_chan);
    
    printf("RDMA监听线程已退出\n");
    return NULL;
}

int main(int argc, char **argv) {
    // 注册信号处理
    signal(SIGINT, signal_handler);

    // 从命令行参数读取 RDMA 设备名（可选），例如：sudo ./pkt_cache_val rxe130
    char *rdma_device = NULL;
    if (argc > 1) rdma_device = argv[1];

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
    if (pthread_create(&listener_thread, NULL, rdma_listener, rdma_device) != 0) {
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
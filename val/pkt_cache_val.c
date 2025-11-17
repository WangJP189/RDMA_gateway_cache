/*
编译命令：
gcc -g pkt_cache_val.c ../251110/pkt_cache.o -o pkt_cache_val -lpthread -lrdmacm -libverbs

运行命令：
sudo ./pkt_cache_val rxe130  # 在130虚拟机上
sudo ./pkt_cache_val rxe135  # 在135虚拟机上
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <rdma/rdma_cma.h>
#include <infiniband/verbs.h>
#include <netinet/in.h>
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <errno.h>

// 使用头文件而非直接包含源文件（避免main函数冲突）
#include "../251110/pkt_cache.h"

// 全局标志：控制线程退出
static volatile int running = 1;

// 要监听的端口（根据wireshark抓包结果设置）
#define LISTEN_PORT1 49441
#define LISTEN_PORT2 4791

// 在文件开头定义线程参数结构体
typedef struct {
    int port;               // 监听端口
    const char *device_name; // RDMA设备名
} ThreadArgs;

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

// 获取设备上下文
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
        printf("检查设备: %s\n", ibv_get_device_name(dev_list[i]));
        if (strcmp(ibv_get_device_name(dev_list[i]), device_name) == 0) {
            // 打开设备上下文
            ctx = ibv_open_device(dev_list[i]);
            if (!ctx) {
                perror("ibv_open_device failed");
            } else {
                printf("成功打开设备: %s\n", device_name);
            }
            break;
        }
    }

    ibv_free_device_list(dev_list);
    return ctx;
}

// 连接信息结构体
struct connection_context {
    struct rdma_cm_id *id;
    struct ibv_mr *mr;
    char *buffer;
    uint32_t local_qp;
    uint32_t remote_qp;
    struct sockaddr_in remote_addr;
};

// 解析数据包并提取PSN（简化版本）
uint32_t extract_psn_from_packet(const char *data, int length) {
    // 在实际的RDMA数据包中，PSN位于BTH（Base Transport Header）中
    // 这里我们使用一个简单的模拟方法：使用数据包的前4个字节作为PSN
    if (length >= 4) {
        return *(uint32_t*)data;
    }
    return 0;
}

// 处理数据接收
void handle_rdma_traffic(struct connection_context *ctx) {
    struct ibv_wc wc;
    int ret;
    uint32_t packet_count = 0;
    
    printf("🔍 开始处理RDMA流量...\n");
    
    // 持续轮询完成队列
    while (running) {
        ret = ibv_poll_cq(ctx->id->recv_cq, 1, &wc);
        if (ret < 0) {
            perror("ibv_poll_cq failed");
            break;
        } else if (ret > 0) {
            if (wc.status == IBV_WC_SUCCESS) {
                if (wc.opcode & IBV_WC_RECV) {
                    packet_count++;
                    
                    // 获取地址信息
                    struct sockaddr_in *local_addr = (struct sockaddr_in *)rdma_get_local_addr(ctx->id);
                    
                    char local_ip[INET_ADDRSTRLEN];
                    char remote_ip[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &local_addr->sin_addr, local_ip, sizeof(local_ip));
                    inet_ntop(AF_INET, &ctx->remote_addr.sin_addr, remote_ip, sizeof(remote_ip));
                    
                    // 提取PSN（模拟）
                    uint32_t psn = extract_psn_from_packet(ctx->buffer, wc.byte_len);
                    if (psn == 0) {
                        psn = packet_count; // 如果提取失败，使用包计数作为PSN
                    }
                    
                    printf("✅ [包%d] 收到RDMA数据包! 长度: %d bytes, QP: %d, PSN: %u\n", 
                           packet_count, wc.byte_len, wc.qp_num, psn);
                    
                    printf("📡 连接信息: %s:%d -> %s:%d\n", 
                           remote_ip, ntohs(ctx->remote_addr.sin_port),
                           local_ip, ntohs(local_addr->sin_port));
                    
                    // 缓存接收到的数据包
                    int cache_ret = add_to_connection_cache(
                        remote_ip,
                        local_ip,
                        ntohs(ctx->remote_addr.sin_port),
                        ntohs(local_addr->sin_port),
                        ctx->remote_qp,  // 远程QP
                        ctx->local_qp,   // 本地QP
                        0,      // 服务类型
                        0xffff, // pkey
                        psn,    // PSN
                        (unsigned char *)ctx->buffer,
                        wc.byte_len
                    );
                    
                    if (cache_ret == 0) {
                        printf("✅ 成功缓存数据包到接收缓存\n");
                        // 打印缓存的数据内容（前50字节）
                        int print_len = wc.byte_len < 50 ? wc.byte_len : 50;
                        printf("📦 数据内容(前%d字节): ", print_len);
                        for (int i = 0; i < print_len; i++) {
                            printf("%02x ", (unsigned char)ctx->buffer[i]);
                            if ((i + 1) % 16 == 0) printf("\n                     ");
                        }
                        printf("\n");
                    } else {
                        printf("❌ 缓存数据包失败 (返回码: %d)\n", cache_ret);
                    }
                    
                    // 重新投递接收请求
                    struct ibv_recv_wr wr, *bad_wr;
                    struct ibv_sge sge;
                    
                    sge.addr = (uintptr_t)ctx->buffer;
                    sge.length = 65536;
                    sge.lkey = ctx->mr->lkey;
                    
                    wr.wr_id = (uintptr_t)ctx->buffer;
                    wr.next = NULL;
                    wr.sg_list = &sge;
                    wr.num_sge = 1;
                    
                    if (ibv_post_recv(ctx->id->qp, &wr, &bad_wr)) {
                        perror("重新投递接收请求失败");
                        break;
                    }
                }
            } else {
                printf("WC错误: %s\n", ibv_wc_status_str(wc.status));
            }
        } else {
            // 没有完成项，短暂休眠
            usleep(10000); // 10ms
        }
    }
    
    printf("🔚 数据处理线程结束，共处理 %d 个数据包\n", packet_count);
}

// 创建并配置QP
int setup_qp(struct rdma_cm_id *id) {
    struct ibv_qp_init_attr qp_attr;
    memset(&qp_attr, 0, sizeof(qp_attr));
    
    // 创建完成队列
    id->recv_cq = ibv_create_cq(id->verbs, 128, NULL, NULL, 0);
    id->send_cq = ibv_create_cq(id->verbs, 128, NULL, NULL, 0);
    if (!id->recv_cq || !id->send_cq) {
        perror("创建CQ失败");
        return -1;
    }
    
    qp_attr.qp_type = IBV_QPT_RC;
    qp_attr.cap.max_send_wr = 128;
    qp_attr.cap.max_recv_wr = 128;
    qp_attr.cap.max_send_sge = 1;
    qp_attr.cap.max_recv_sge = 1;
    qp_attr.cap.max_inline_data = 64;
    qp_attr.recv_cq = id->recv_cq;
    qp_attr.send_cq = id->send_cq;
    
    if (rdma_create_qp(id, id->pd, &qp_attr)) {
        perror("rdma_create_qp失败");
        return -1;
    }
    
    return 0;
}

// RDMA连接监听函数 - 单个端口监听
void *rdma_port_listener(void *arg) {
    int *port = (int *)arg;
    char *device = (char *)*(port + 1); // 获取设备名指针
    struct rdma_event_channel *ec = NULL;
    struct rdma_cm_id *listener = NULL;
    struct ibv_context *verbs = NULL;
    int ret;

    printf("🚀 启动RDMA监听线程，设备: %s, 端口: %d\n", device ? device : "默认", *port);

    // 初始化事件通道
    ec = rdma_create_event_channel();
    if (!ec) {
        perror("❌ rdma_create_event_channel failed");
        goto cleanup;
    }

    // 创建CM ID - 使用RC模式
    if (rdma_create_id(ec, &listener, NULL, RDMA_PS_TCP)) {
        perror("❌ rdma_create_id failed");
        goto cleanup;
    }

    // 获取设备上下文
    if (device && strcmp(device, "") != 0) {
        verbs = get_ibv_context_by_name(device);
        if (!verbs) {
            fprintf(stderr, "❌ 无法获取设备 %s 的上下文\n", device);
            goto cleanup;
        }
        listener->verbs = verbs;
        printf("✅ 成功关联设备上下文: %s\n", device);
    }

    // 绑定到指定端口
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(*port);  // 使用指定端口
    addr.sin_addr.s_addr = INADDR_ANY;

    printf("📡 绑定到端口 %d...\n", *port);
    if (rdma_bind_addr(listener, (struct sockaddr *)&addr)) {
        perror("❌ rdma_bind_addr failed");
        goto cleanup;
    }

    // 开始监听
    if (rdma_listen(listener, 10)) {
        perror("❌ rdma_listen failed");
        goto cleanup;
    }

    printf("✅ RDMA缓存监听已启动，等待连接...\n");
    printf("📍 监听地址: 0.0.0.0:%d\n", *port);
    printf("📍 设备: %s\n", device ? device : "默认");

    // 事件处理循环
    while (running) {
        struct rdma_cm_event *event;
        
        // 设置超时以避免永久阻塞
        struct timeval tv = {2, 0}; // 2秒超时
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(ec->fd, &read_fds);
        
        ret = select(ec->fd + 1, &read_fds, NULL, NULL, &tv);
        if (ret < 0) {
            if (running && errno != EINTR) {
                perror("select failed");
            }
            continue;
        } else if (ret == 0) {
            // 超时，继续循环
            continue;
        }
        
        // 有事件到达
        if (rdma_get_cm_event(ec, &event)) {
            if (running && errno != EINTR) {
                perror("rdma_get_cm_event failed");
            }
            continue;
        }

        printf("📩 收到RDMA事件: %s (端口: %d)\n", rdma_event_str(event->event), *port);

        if (event->event == RDMA_CM_EVENT_CONNECT_REQUEST) {
            struct rdma_cm_id *client_id = event->id;
            
            printf("✅ 收到连接请求，准备建立连接...\n");
            
            // 保存远程地址信息
            struct connection_context *conn_ctx = malloc(sizeof(struct connection_context));
            if (!conn_ctx) {
                perror("❌ 分配连接上下文失败");
                rdma_reject(client_id, NULL, 0);
                rdma_ack_cm_event(event);
                continue;
            }
            
            memset(conn_ctx, 0, sizeof(struct connection_context));
            conn_ctx->id = client_id;
            memcpy(&conn_ctx->remote_addr, rdma_get_peer_addr(client_id), sizeof(struct sockaddr_in));
            
            // 设置QP
            if (setup_qp(client_id) != 0) {
                fprintf(stderr, "❌ 设置QP失败\n");
                free(conn_ctx);
                rdma_reject(client_id, NULL, 0);
                rdma_ack_cm_event(event);
                continue;
            }
            
            // 分配接收缓冲区
            conn_ctx->buffer = malloc(65536);
            if (!conn_ctx->buffer) {
                perror("❌ 分配缓冲区失败");
                free(conn_ctx);
                rdma_destroy_qp(client_id);
                rdma_reject(client_id, NULL, 0);
                rdma_ack_cm_event(event);
                continue;
            }
            
            // 注册内存区域
            conn_ctx->mr = ibv_reg_mr(client_id->pd, conn_ctx->buffer, 65536,
                                     IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ);
            if (!conn_ctx->mr) {
                perror("❌ 注册MR失败");
                free(conn_ctx->buffer);
                free(conn_ctx);
                rdma_destroy_qp(client_id);
                rdma_reject(client_id, NULL, 0);
                rdma_ack_cm_event(event);
                continue;
            }
            
            // 设置QP号
            conn_ctx->local_qp = client_id->qp->qp_num;
            
            // 从连接请求中获取远程QP号（需要解析私有数据）
            if (event->param.conn.private_data && event->param.conn.private_data_len >= 4) {
                // 简化：假设私有数据的前4个字节包含远程QP号
                conn_ctx->remote_qp = *(uint32_t*)event->param.conn.private_data;
            } else {
                conn_ctx->remote_qp = 0; // 未知
            }
            
            printf("🔗 QP信息: 本地QP=%u, 远程QP=%u\n", conn_ctx->local_qp, conn_ctx->remote_qp);
            
            // 投递初始接收请求
            struct ibv_recv_wr wr, *bad_wr;
            struct ibv_sge sge;
            
            sge.addr = (uintptr_t)conn_ctx->buffer;
            sge.length = 65536;
            sge.lkey = conn_ctx->mr->lkey;
            
            wr.wr_id = (uintptr_t)conn_ctx;
            wr.next = NULL;
            wr.sg_list = &sge;
            wr.num_sge = 1;
            
            if (ibv_post_recv(client_id->qp, &wr, &bad_wr)) {
                perror("❌ 投递接收请求失败");
                ibv_dereg_mr(conn_ctx->mr);
                free(conn_ctx->buffer);
                free(conn_ctx);
                rdma_destroy_qp(client_id);
                rdma_reject(client_id, NULL, 0);
                rdma_ack_cm_event(event);
                continue;
            }
            
            // 接受连接
            struct rdma_conn_param conn_param = {0};
            conn_param.responder_resources = 1;
            conn_param.initiator_depth = 1;
            conn_param.retry_count = 3;
            
            // 发送私有数据（包含我们的QP号）
            uint32_t private_data = conn_ctx->local_qp;
            conn_param.private_data = &private_data;
            conn_param.private_data_len = sizeof(private_data);
            
            if (rdma_accept(client_id, &conn_param)) {
                perror("❌ rdma_accept失败");
                ibv_dereg_mr(conn_ctx->mr);
                free(conn_ctx->buffer);
                free(conn_ctx);
                rdma_destroy_qp(client_id);
                rdma_reject(client_id, NULL, 0);
                rdma_ack_cm_event(event);
                continue;
            }
            
            printf("✅ RDMA连接已建立!\n");
            
            // 启动数据处理线程
            pthread_t data_thread;
            if (pthread_create(&data_thread, NULL, (void *(*)(void *))handle_rdma_traffic, conn_ctx) != 0) {
                perror("❌ 创建数据处理线程失败");
                ibv_dereg_mr(conn_ctx->mr);
                free(conn_ctx->buffer);
                free(conn_ctx);
            } else {
                printf("✅ 启动数据处理线程\n");
                pthread_detach(data_thread);
            }
            
        } else if (event->event == RDMA_CM_EVENT_ESTABLISHED) {
            printf("✅ RDMA连接已完全建立 (端口: %d)\n", *port);
            
        } else if (event->event == RDMA_CM_EVENT_DISCONNECTED) {
            printf("🔌 RDMA连接已断开 (端口: %d)\n", *port);
            if (event->id->qp) {
                rdma_destroy_qp(event->id);
            }
            rdma_destroy_id(event->id);
            
        } else if (event->event == RDMA_CM_EVENT_REJECTED) {
            printf("❌ 连接被拒绝 (端口: %d)\n", *port);
            
        } else if (event->event == RDMA_CM_EVENT_CONNECT_ERROR) {
            printf("❌ 连接错误 (端口: %d)\n", *port);
        } else if (event->event == RDMA_CM_EVENT_ADDR_RESOLVED) {
            printf("🌐 地址解析完成 (端口: %d)\n", *port);
        } else if (event->event == RDMA_CM_EVENT_ROUTE_RESOLVED) {
            printf("🗺️  路由解析完成 (端口: %d)\n", *port);
        }

        rdma_ack_cm_event(event);
    }

cleanup:
    printf("🧹 清理RDMA监听资源 (端口: %d)...\n", *port);
    if (listener) {
        if (listener->qp) rdma_destroy_qp(listener);
        rdma_destroy_id(listener);
    }
    if (ec) {
        rdma_destroy_event_channel(ec);
    }
    if (verbs) {
        ibv_close_device(verbs);
    }
    printf("✅ RDMA监听线程已退出 (端口: %d)\n", *port);
    return NULL;
}

int main(int argc, char **argv) {
    printf("🚀 ===== 启动RDMA缓存验证程序 =====\n");
    
    // 注册信号处理
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // 从命令行参数读取 RDMA 设备名
    char *rdma_device = NULL;
    if (argc > 1) {
        rdma_device = argv[1];
        printf("📡 使用设备: %s\n", rdma_device);
    } else {
        fprintf(stderr, "❌ 请指定RDMA设备名，例如: %s rxe130\n", argv[0]);
        return 1;
    }

    // 初始化缓存管理器
    printf("\n📦 初始化缓存管理器...\n");
    g_cache_mgr = init_cache_manager(
        1024,    // 哈希表大小
        100,     // 最大连接数
        1000,    // 每连接最大报文数
        100,     // 每连接最大字节数(MB)
        300      // 连接超时时间(秒)
    );
    if (!g_cache_mgr) {
        fprintf(stderr, "❌ 初始化缓存管理器失败\n");
        return 1;
    }
    printf("✅ 缓存管理器初始化成功\n");

    // 创建状态打印线程
    pthread_t printer_thread;
    if (pthread_create(&printer_thread, NULL, status_printer, NULL) != 0) {
        perror("❌ 创建状态打印线程失败");
        return 1;
    }
    printf("✅ 状态打印线程已启动\n");

    // 要监听的端口
    int ports[] = {LISTEN_PORT1, LISTEN_PORT2};
    pthread_t listener_threads[2];
    
    // 为每个端口创建独立的监听线程
    for (int i = 0; i < 2; i++) {
        // 为每个线程分配参数结构体
        ThreadArgs *args = malloc(sizeof(ThreadArgs));
        if (!args) {
            perror("malloc ThreadArgs failed");
            running = 0;
            pthread_join(printer_thread, NULL);
            return 1;
        }
        args->port = ports[i];               // 正确赋值端口
        args->device_name = rdma_device;     // 正确传递字符串指针

        // 创建线程，传递参数结构体指针
        if (pthread_create(&listener_threads[i], NULL, rdma_port_listener, args) != 0) {
            perror("创建RDMA监听线程失败");
            free(args);  // 失败时释放内存
            running = 0;
            pthread_join(printer_thread, NULL);
            return 1;
        }
        printf("✅ RDMA监听线程已启动 (端口: %d)\n", ports[i]);
    }

    printf("\n🎯 等待RDMA连接...\n");
    printf("📍 缓存程序正在监听端口: %d, %d\n", LISTEN_PORT1, LISTEN_PORT2);
    printf("📍 使用 Ctrl+C 退出程序\n\n");

    // 等待所有监听线程结束
    for (int i = 0; i < 2; i++) {
        pthread_join(listener_threads[i], NULL);
        printf("RDMA监听线程 (端口: %d) 已结束\n", ports[i]);
    }
    
    running = 0;
    pthread_join(printer_thread, NULL);
    printf("状态打印线程已结束\n");

    printf("\n🏁 程序已退出\n");
    return 0;
}
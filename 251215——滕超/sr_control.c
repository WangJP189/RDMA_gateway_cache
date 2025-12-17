#include "sr_control.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <errno.h>

int g_tcp_sockfd = -1;
int g_tcp_connected = 0;
static pthread_t g_server_tid;

// 简单的接收处理函数（仅用于演示接收到的数据）
void handle_connection(int connfd) {
    char buffer[1024];
    while (1) {
        memset(buffer, 0, sizeof(buffer));
        ssize_t n = recv(connfd, buffer, sizeof(buffer) - 1, 0);
        if (n <= 0) {
            printf("\n[TCP] 对端断开连接或出错\n");
            close(connfd);
            g_tcp_connected = 0;
            g_tcp_sockfd = -1;
            break;
        }
        printf("\n[TCP Recv] 收到 %zd 字节: %s\n", n, buffer);
    }
}

// 服务端线程主函数
void* tcp_server_worker(void* arg) {
    int port = *(int*)arg;
    free(arg);

    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0) {
        perror("[TCP] 创建监听socket失败");
        return NULL;
    }

    // 允许地址重用，方便调试时快速重启
    int opt = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons(port);

    if (bind(listenfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("[TCP] Bind失败");
        close(listenfd);
        return NULL;
    }

    if (listen(listenfd, 5) < 0) {
        perror("[TCP] Listen失败");
        close(listenfd);
        return NULL;
    }

    printf("[TCP] 服务端启动，监听端口 %d ...\n", port);

    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    
    // 阻塞等待连接（简单模型：只接受一个连接）
    int connfd = accept(listenfd, (struct sockaddr*)&client_addr, &client_len);
    if (connfd < 0) {
        perror("[TCP] Accept失败");
        close(listenfd);
        return NULL;
    }

    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
    printf("\n[TCP] 接受来自 %s 的连接!\n", client_ip);

    g_tcp_sockfd = connfd;
    g_tcp_connected = 1; // 标记为已连接

    // 进入接收循环
    handle_connection(connfd);

    close(listenfd); // 关闭监听，只维持现有连接（简化演示）
    return NULL;
}

void start_tcp_server_thread(int port) {
    if (g_tcp_connected) {
        printf("[TCP] 连接已存在，请先断开\n");
        return;
    }

    int* port_ptr = malloc(sizeof(int));
    *port_ptr = port;
    if (pthread_create(&g_server_tid, NULL, tcp_server_worker, port_ptr) != 0) {
        perror("[TCP] 创建服务端线程失败");
    }
}

int connect_to_gateway(const char* target_ip, int port) {
    if (g_tcp_connected) {
        printf("[TCP] 已经建立了连接\n");
        return -1;
    }

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("[TCP] Socket创建失败");
        return -1;
    }

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);

    if (inet_pton(AF_INET, target_ip, &serv_addr.sin_addr) <= 0) {
        printf("[TCP] 无效的 IP 地址\n");
        close(sockfd);
        return -1;
    }

    printf("[TCP] 正在连接 %s:%d ...\n", target_ip, port);
    if (connect(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("[TCP] 连接失败");
        close(sockfd);
        return -1;
    }

    printf("[TCP] 连接成功!\n");
    g_tcp_sockfd = sockfd;
    g_tcp_connected = 2; // Client模式

    // 启动一个简单的接收线程（以便全双工通信，或者在主循环只做发送）
    // 为了简化，这里客户端暂不启动接收线程，只负责发送
    return 0;
}

int send_tcp_message(const char* msg) {
    if (!g_tcp_connected || g_tcp_sockfd < 0) {
        printf("[TCP] 未连接\n");
        return -1;
    }

    // TCP使用 send，sendto 的 dest_addr 参数在 TCP连接套接字中是被忽略的
    ssize_t sent = send(g_tcp_sockfd, msg, strlen(msg), 0);
    if (sent < 0) {
        perror("[TCP] 发送失败");
        return -1;
    }
    printf("[TCP] 发送成功: %zd bytes\n", sent);
    return 0;
}

void cleanup_tcp() {
    if (g_tcp_sockfd >= 0) {
        close(g_tcp_sockfd);
        g_tcp_sockfd = -1;
    }
    g_tcp_connected = 0;
}

// 添加到 sr_control.c 末尾
void send_batch_tcp_message(int count, const char* base_msg) {
    if (!g_tcp_connected || g_tcp_sockfd < 0) {
        printf("[TCP] 未连接\n");
        return;
    }

    char buffer[128];
    printf("[TCP] 开始批量发送 %d 条报文...\n", count);

    for (int i = 0; i < count; i++) {
        // 格式化消息，例如 "Hello-0|", "Hello-1|"
        // 加上 '|' 是为了方便你在Server端看清楚边界
        snprintf(buffer, sizeof(buffer), "[%s-%d]", base_msg, i);
        
        // 连续发送，不等待
        ssize_t sent = send(g_tcp_sockfd, buffer, strlen(buffer), 0);
        
        if (sent < 0) {
            perror("[TCP] 发送失败");
            break;
        }
    }
    printf("[TCP] 批量发送完成\n");
}
#include "sr_control.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

// 全局变量
extern char g_sr_target_ip[32];
extern int g_sr_target_port;

int g_sr_server_listen_fd = -1;
int g_sr_client_fd = -1;

// 测试用
int g_server_conn_fd = -1;

// ============================================================
//  基础 TCP 逻辑 (Server Setup, Accept, Client Connect)
// ============================================================

int tcp_server_setup(const char *ip, int port) {
    if (g_sr_server_listen_fd != -1) {
        printf("[Dst-Server] 监听端口已存在\n");
        return -1;
    }

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("Socket error");
        return -1;
    }

    int opt = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    if (inet_pton(AF_INET, ip, &serv_addr.sin_addr) <= 0)
        serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons(port);

    if (bind(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("[Dst-Server] Bind failed");
        close(sockfd);
        return -1;
    }
    if (listen(sockfd, 5) < 0) {
        perror("[Dst-Server] Listen failed");
        close(sockfd);
        return -1;
    }

    g_sr_server_listen_fd = sockfd;
    printf("[Dst-Server] 启动监听 %s:%d\n", ip, port);
    return 0;
}

int tcp_client_connect(const char *target_ip, int port) {
    if (g_sr_client_fd != -1)
        return -1;

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0)
        return -1;

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    inet_pton(AF_INET, target_ip, &serv_addr.sin_addr);

    printf("[Src-Client] 连接 %s:%d ...\n", target_ip, port);
    if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("Connect failed");
        close(sockfd);
        return -1;
    }

    g_sr_client_fd = sockfd;
    printf("[Src-Client] 连接成功!\n");
    return 0;
}

// 测试用
int tcp_server_accept(void) {
    if (g_sr_server_listen_fd == -1)
        return -1;

    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    printf("[Dst-Server] 等待连接...\n");
    int connfd = accept(g_sr_server_listen_fd, (struct sockaddr *)&client_addr,
                        &client_len);
    if (connfd < 0) {
        perror("Accept failed");
        return -1;
    }

    if (g_server_conn_fd != -1)
        close(g_server_conn_fd);
    g_server_conn_fd = connfd;

    printf("[Dst-Server] 连接建立! FD=%d\n", connfd);
    return 0;
}

// ============================================================
//  SR重传请求接口
// ============================================================

// 1. 配置对端信息
void set_sr_target_info(const char *ip, int port) {
    if (ip) {
        strncpy(g_sr_target_ip, ip, sizeof(g_sr_target_ip) - 1);
    }
    g_sr_target_port = port;
    printf("[SR-Config] 对端地址已更新: %s:%d\n", g_sr_target_ip,
           g_sr_target_port);
}

// 2. 获取客户端FD (不存在则连接，存在则复用)
int get_sr_client_fd(void) {
    // 如果已有连接，直接复用
    if (g_sr_client_fd != -1) {
        return g_sr_client_fd;
    }

    // 如果没有连接，执行 Socket -> Connect 流程
    if (g_sr_target_port == 0 || strlen(g_sr_target_ip) == 0) {
        printf("[SR-Client] 错误：未配置对端IP或端口，请先配置！\n");
        return -1;
    }

    printf("[SR-Client] 尝试建立新连接 -> %s:%d ...\n", g_sr_target_ip,
           g_sr_target_port);

    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("[SR-Client] Socket创建失败");
        return -1;
    }

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(g_sr_target_port);
    if (inet_pton(AF_INET, g_sr_target_ip, &serv_addr.sin_addr) <= 0) {
        printf("[SR-Client] 无效的IP地址\n");
        close(sockfd);
        return -1;
    }

    // 阻塞连接
    if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) ==
        0) {
        printf("[SR-Client] 连接成功 (FD=%d)\n", sockfd);
        g_sr_client_fd = sockfd; // 保存FD
        return sockfd;
    } else {
        perror("[SR-Client] 连接失败");
        close(sockfd);
        return -1;
    }
}

// 测试用
int send_sr_request(const char *msg) {
    // 获取FD (自动处理连接)
    int fd = get_sr_client_fd();
    if (fd == -1) {
        return -1; // 获取连接失败
    }

    // 发送数据
    ssize_t sent = send(fd, msg, strlen(msg), 0);

    // 异常检测
    if (sent < 0) {
        perror("[SR-Client] 发送异常");
        // 发送失败，认为是连接断开，执行清理，确保下次重连
        close_sr_client_fd();
        return -1;
    }

    printf("[SR-Client] 发送SR请求成功: %s (%zd bytes)\n", msg, sent);
    return 0;
}

// ============================================================
//  资源清理
// ============================================================
void cleanup_tcp(void) {
    if (g_sr_server_listen_fd != -1) {
        close(g_sr_server_listen_fd);
        g_sr_server_listen_fd = -1;
    }
    if (g_server_conn_fd != -1) {
        close(g_server_conn_fd);
        g_server_conn_fd = -1;
    }
    if (g_sr_client_fd != -1) {
        close(g_sr_client_fd);
        g_sr_client_fd = -1;
    }

    printf("[TCP] 资源已释放\n");
}
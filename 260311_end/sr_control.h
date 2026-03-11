#ifndef SR_CONTROL_H
#define SR_CONTROL_H

#include <stdint.h>
#include <stdio.h>

// ==========================================
// 全局变量
// ==========================================

extern char g_sr_target_ip[32];
extern int g_sr_target_port;

extern int g_sr_server_listen_fd;
extern int g_sr_client_fd;

// 测试用
extern int g_server_conn_fd;

// ==========================================
// 接口封装
// ==========================================

// 原有测试接口
int tcp_server_setup(const char *ip, int port);
int tcp_client_connect(const char *target_ip, int port);

// 测试用
int tcp_server_accept(void);

// SR重传请求客户端接口
void set_sr_target_info(const char *ip, int port); // 1. 配置

int get_sr_client_fd(void); // 2. 获取FD (含自动连接)

void close_sr_client_fd(void); // 3. 关闭并重置

// 测试用
int send_sr_request(const char *msg);

// 清理资源
void cleanup_tcp(void);

#endif
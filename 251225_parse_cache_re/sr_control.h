#ifndef SR_CONTROL_H
#define SR_CONTROL_H

#include <stdio.h>
#include <stdint.h>
#include <pthread.h>

// 全局TCP套接字句柄（简化演示，实际可封装在结构体中）
extern int g_tcp_sockfd;
extern int g_tcp_connected; // 0:未连接, 1:作为Server已连接, 2:作为Client已连接

// 启动TCP服务端（监听等待连接）
void start_tcp_server_thread(int port);

// 启动TCP客户端（主动连接对端）
int connect_to_gateway(const char* target_ip, int port);

// 发送测试消息
int send_tcp_message(const char* msg);

// 关闭TCP资源
void cleanup_tcp();

void send_batch_tcp_message(int count, const char* base_msg);
#endif
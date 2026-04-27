#ifndef SYSTEM_H
#define SYSTEM_H

#include <pthread.h>

// 系统资源清理函数
void cleanup_resources(void);

// 优雅退出机制初始化
int init_graceful_shutdown(void);

// DPDK初始化
int init_dpdk(int argc, char *argv[]);

// 网关资源初始化
int init_gw_resources(void);

// 工作线程启动
int start_worker_threads(pthread_attr_t *attr);

#endif // SYSTEM_H
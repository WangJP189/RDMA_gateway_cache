#ifndef NETWORK_H
#define NETWORK_H

#include "global.h"

// 网卡初始化
int port_init(uint16_t port, struct rte_mempool *mbuf_pool);
void close_network_port(void);

// 信号处理
void signal_handler(int sig);
void setup_signal_handlers(void);

#endif // NETWORK_H
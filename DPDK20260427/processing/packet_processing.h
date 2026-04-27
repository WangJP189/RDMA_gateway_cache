#ifndef PACKET_PROCESSING_H
#define PACKET_PROCESSING_H

#include "global.h"

// 报文处理函数
void process_packet(struct rte_mbuf *mbuf);

#endif // PACKET_PROCESSING_H
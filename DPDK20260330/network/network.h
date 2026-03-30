#ifndef NETWORK_H
#define NETWORK_H

#include "global.h"

int init_port(uint16_t port, uint16_t rx_queue_id, uint16_t tx_queue_id);

void close_port(uint16_t port);

#endif // NETWORK_H
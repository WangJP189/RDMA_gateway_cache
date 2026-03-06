#ifndef THREAD_FUNCTIONS_H
#define THREAD_FUNCTIONS_H

#include "global.h"

// 线程函数
void *rx_thread_func(void *arg);
void *parse_thread_func(void *arg);
void *aging_thread_func(void *arg);
void *sr_retrans_thread_func(void *arg);

// 辅助函数
uint32_t traverse_psn_with_wrap(struct cache_entry_v4 *entry,
                                struct rte_mbuf **tx_burst, uint16_t max_burst,
                                uint64_t current_time);
void wait_for_threads_exit(void);
int check_thread_health(void);

#endif // THREAD_FUNCTIONS_H
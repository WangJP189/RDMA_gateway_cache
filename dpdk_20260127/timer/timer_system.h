#ifndef TIMER_SYSTEM_H
#define TIMER_SYSTEM_H

#include "global.h"

// 定时器系统
int init_delay_processing_system(struct global_data *gd);
void cleanup_delay_processing_system(void);
int create_rnr_timer(struct global_data *gd, struct cache_entry_v4 *entry,
                     uint32_t nak_psn);
int create_nak_delay_timer(struct global_data *gd, struct cache_entry_v4 *entry,
                           uint32_t nak_psn);
void update_nak_delay_timer(struct cache_entry_v4 *entry);

// 定时器线程
void *timer_epoll_thread_func(void *arg);
void *delay_proc_thread_func(void *arg);
void process_delayed_rnr_retry(struct global_data *gd,
                               struct delay_task_info *task_info);
void process_delayed_nak_retry(struct global_data *gd,
                               struct delay_task_info *task_info);

#endif // TIMER_SYSTEM_H
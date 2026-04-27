#ifndef THREAD_FUNCTIONS_H
#define THREAD_FUNCTIONS_H

// 线程函数

void *rx_thread_func(void *arg);

void *parse_thread_func(void *arg);

// ==========================================
// 【新增4】任务线程
// ==========================================
void *task_thread_func(void *arg);

// ==========================================
// 【新增3】老化线程（自由调度）
// ==========================================
void *aging_thread_func(void *arg);

// ==========================================
// 【新增5】TCP线程（自由调度）
// ==========================================
void *retrans_thread_func(void *arg);

#endif // THREAD_FUNCTIONS_H
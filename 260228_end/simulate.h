#ifndef SIMULATE_H
#define SIMULATE_H

#include <stdint.h>
#include <time.h>
#include <signal.h>

// 性能统计数据结构
typedef struct {
    uint64_t total_packets;       // 总处理报文数
    uint64_t total_bytes;         // 总处理字节数
    struct timespec start_time;   // 统计开始时间
    struct timespec end_time;     // 统计结束时间
    uint64_t drop_packets;        // 丢包数
    uint64_t retransmit_packets;  // 重传报文数
    double avg_throughput;        // 平均吞吐量 (MB/s)
    double avg_latency;           // 平均时延 (ms)
} PerfStats;

// 新增：声明全局性能统计变量（extern表示变量在其他文件定义）
extern PerfStats g_perf_stats;
// 新增：声明全局性能监控开关
extern volatile sig_atomic_t g_perf_monitoring;

// 初始化性能统计
void init_perf_stats(PerfStats *stats);

// 开始性能统计
void start_perf_monitor(PerfStats *stats);

// 停止性能统计并计算结果
void stop_perf_monitor(PerfStats *stats);

// 记录处理的报文
void record_packet(PerfStats *stats, uint32_t pkt_size);

// 记录丢包
void record_drop_packet(PerfStats *stats);

// 记录重传报文
void record_retransmit_packet(PerfStats *stats);

// 打印性能统计结果
void print_perf_stats(const PerfStats *stats);

// 重置性能统计
void reset_perf_stats(PerfStats *stats);

#endif // SIMULATE_H
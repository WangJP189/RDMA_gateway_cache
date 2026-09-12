#include "simulate.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

// 初始化性能统计结构体
void init_perf_stats(PerfStats *stats) {
    if (stats == NULL)
        return;
    memset(stats, 0, sizeof(PerfStats));
}

// 开始性能监控（记录开始时间）
void start_perf_monitor(PerfStats *stats) {
    if (stats == NULL)
        return;
    clock_gettime(CLOCK_MONOTONIC, &stats->start_time);
    stats->total_packets = 0;
    stats->total_bytes = 0;
    stats->drop_packets = 0;
    stats->retransmit_packets = 0;
}

// 停止性能监控并计算吞吐量/时延
void stop_perf_monitor(PerfStats *stats) {
    if (stats == NULL)
        return;

    clock_gettime(CLOCK_MONOTONIC, &stats->end_time);

    // 计算耗时（秒）
    double elapsed =
        (stats->end_time.tv_sec - stats->start_time.tv_sec) +
        (stats->end_time.tv_nsec - stats->start_time.tv_nsec) / 1e9;

    if (elapsed <= 0) {
        stats->avg_throughput = 0;
        stats->avg_latency = 0;
        return;
    }

    // 计算吞吐量 (MB/s)：1MB = 1024*1024 bytes
    stats->avg_throughput = (stats->total_bytes / (1024.0 * 1024.0)) / elapsed;

    // 计算平均时延 (ms)：总耗时 / 总报文数 * 1000
    if (stats->total_packets > 0) {
        stats->avg_latency = (elapsed * 1000.0) / stats->total_packets;
    } else {
        stats->avg_latency = 0;
    }
}

// 记录单个报文处理
void record_packet(PerfStats *stats, uint32_t pkt_size) {
    if (stats == NULL)
        return;
    stats->total_packets++;
    stats->total_bytes += pkt_size;
}

// 记录丢包
void record_drop_packet(PerfStats *stats) {
    if (stats == NULL)
        return;
    stats->drop_packets++;
}

// 记录重传报文
void record_retransmit_packet(PerfStats *stats) {
    if (stats == NULL)
        return;
    stats->retransmit_packets++;
}

// 打印性能统计结果
void print_perf_stats(const PerfStats *stats) {
    if (stats == NULL)
        return;

    printf("\n===== 性能测试统计结果 =====\n");
    printf("总处理报文数: %lu\n", stats->total_packets);
    printf("总处理字节数: %lu bytes (%.2f MB)\n", stats->total_bytes,
           stats->total_bytes / (1024.0 * 1024.0));
    printf("丢包数: %lu (丢包率: %.2f%%)\n", stats->drop_packets,
           stats->total_packets > 0
               ? (stats->drop_packets * 100.0) / stats->total_packets
               : 0.0);
    printf("重传报文数: %lu (重传率: %.2f%%)\n", stats->retransmit_packets,
           stats->total_packets > 0
               ? (stats->retransmit_packets * 100.0) / stats->total_packets
               : 0.0);
    printf("平均吞吐量: %.2f MB/s\n", stats->avg_throughput);
    printf("平均处理时延: %.4f ms\n", stats->avg_latency);
    printf("==============================\n");
}

// 重置性能统计
void reset_perf_stats(PerfStats *stats) {
    if (stats == NULL)
        return;
    memset(stats, 0, sizeof(PerfStats));
}
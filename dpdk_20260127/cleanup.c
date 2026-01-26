#include "global.h"
#include "network/network.h"
#include "tables/connection_table.h"
#include "threads/thread_functions.h"
#include "timer/timer_system.h"
#include <rte_eal.h>
#include <stdio.h>


// 资源清理函数
void cleanup_resources(void) {
    printf("开始清理系统资源...\n");

    // 1. 设置停止标志，确保所有线程退出
    g_data.stop = 1;

    // 2. 等待所有线程结束
    wait_for_threads_exit();

    // 3. 清理延时处理系统
    cleanup_delay_processing_system();

    // 4. 关闭网卡
    close_network_port();

    // 5. 销毁连接表
    destroy_cache_table();

    // 6. 释放环形队列
    if (g_data.tx_ring) {
        rte_ring_free(g_data.tx_ring);
        g_data.tx_ring = NULL;
        printf("TX ring freed\n");
    }

    if (g_data.rx_ring) {
        rte_ring_free(g_data.rx_ring);
        g_data.rx_ring = NULL;
        printf("RX ring freed\n");
    }

    if (g_data.delay_task_ring) {
        rte_ring_free(g_data.delay_task_ring);
        g_data.delay_task_ring = NULL;
        printf("Delay task ring freed\n");
    }

    // 7. 释放内存池
    if (g_data.mbuf_pool) {
        rte_mempool_free(g_data.mbuf_pool);
        g_data.mbuf_pool = NULL;
        printf("Mbuf pool freed\n");
    }

    // 8. DPDK环境清理
    printf("Cleaning up DPDK EAL...\n");
    rte_eal_cleanup();

    printf("所有资源清理完成！\n");
}

// 程序退出前的最后清理
static void atexit_handler(void) {
    printf("\n程序退出，执行最终清理...\n");

    // 确保所有资源都被清理
    if (!g_data.stop) {
        g_data.stop = 1;
        sleep(1);
    }

    // 调用统一的资源清理函数
    cleanup_resources();
}
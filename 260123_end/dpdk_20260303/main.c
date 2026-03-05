#include "cleanup.c"
#include "config.h"
#include "global.h"
#include "network/network.h"
#include "tables/connection_table.h"
#include "threads/thread_functions.h"
#include "timer/timer_system.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

struct global_data g_data;

// 初始化系统
static int init_system(int argc, char *argv[]) {
    int ret;
    pthread_attr_t attr;
    cpu_set_t cpuset;

    // 初始化全局数据为NULL
    memset(&g_data, 0, sizeof(g_data));

    // 注册退出处理函数
    if (atexit(atexit_handler) != 0) {
        fprintf(stderr, "无法注册退出处理函数\n");
        return ERR;
    }

    // 设置信号处理器
    setup_signal_handlers();

    // 设置程序标题
    printf("\n========================================\n");
    printf("     RoCEv2 Packet Processor v2.0\n");
    printf("========================================\n\n");

    // 初始化DPDK环境
    ret = rte_eal_init(argc, argv);
    if (ret < 0) {
        fprintf(stderr, "EAL init failed\n");
        return ERR;
    }

    argc -= ret;
    argv += ret;

    return OK;
}

// 创建内存池和队列
static int create_memory_pools_and_queues(void) {
    // 创建内存池
    g_data.mbuf_pool =
        rte_pktmbuf_pool_create("mbuf_pool", NUM_MBUFS, MBUF_CACHE_SIZE, 0,
                                MBUF_NODE_SIZE, rte_socket_id());
    if (!g_data.mbuf_pool) {
        fprintf(stderr, "Cannot create mbuf pool\n");
        return ERR;
    }
    printf("✓ 创建内存池: %u mbufs, 缓存: %u, 节点大小: %u\n", NUM_MBUFS,
           MBUF_CACHE_SIZE, MBUF_NODE_SIZE);

    // 创建无锁环形队列
    g_data.rx_ring = rte_ring_create("rx_ring", USR_RING_SIZE, rte_socket_id(),
                                     RING_F_SP_ENQ | RING_F_SC_DEQ);
    if (!g_data.rx_ring) {
        fprintf(stderr, "Cannot create rx ring\n");
        return ERR;
    }

    g_data.tx_ring = rte_ring_create("tx_ring", USR_RING_SIZE, rte_socket_id(),
                                     RING_F_SP_ENQ | RING_F_SC_DEQ);
    if (!g_data.tx_ring) {
        fprintf(stderr, "Cannot create tx ring\n");
        return ERR;
    }

    printf("✓ 创建环形队列: RX大小 %u, TX大小 %u\n", USR_RING_SIZE,
           USR_RING_SIZE);

    return OK;
}

// 创建连接表
static int create_connection_table(void) {
    g_data.cache_tbl_v4 = cache_tbl_v4_create(NUM_BUCKETS);
    if (!g_data.cache_tbl_v4) {
        fprintf(stderr, "Cannot create connection table\n");
        return ERR;
    }

    return OK;
}

// 初始化网络接口
static int init_network_interface(void) {
    g_data.port_id = 0;
    g_data.rx_queue_id = 0;
    g_data.tx_queue_id = 0;

    if (port_init(g_data.port_id, g_data.mbuf_pool) != 0) {
        fprintf(stderr, "Cannot init port %u\n", g_data.port_id);
        return ERR;
    }

    return OK;
}

// 初始化线程属性
static int init_thread_attributes(pthread_attr_t *attr) {
    if (pthread_attr_init(attr) != 0) {
        fprintf(stderr, "Failed to init thread attr\n");
        return ERR;
    }
    pthread_attr_setdetachstate(attr, PTHREAD_CREATE_JOINABLE);
    return OK;
}

// 启动工作线程
static int start_worker_threads(pthread_attr_t *attr) {
    cpu_set_t cpuset;

    printf("\n启动线程 (CPU亲和性):\n");
    printf("  RX线程: CPU 0\n");
    printf("  解析线程: CPU 1\n");
    printf("  老化线程: CPU 2\n");
    printf("  重传线程: CPU 3\n\n");

    // 启动收包线程（绑定到CPU0）
    CPU_ZERO(&cpuset);
    CPU_SET(0, &cpuset);
    pthread_attr_setaffinity_np(attr, sizeof(cpu_set_t), &cpuset);
    if (pthread_create(&g_data.rx_thread, attr, rx_thread_func, &g_data) != 0) {
        fprintf(stderr, "Failed to create RX thread\n");
        return ERR;
    }

    // 启动解析线程（绑定到CPU1）
    CPU_ZERO(&cpuset);
    CPU_SET(1, &cpuset);
    pthread_attr_setaffinity_np(attr, sizeof(cpu_set_t), &cpuset);
    if (pthread_create(&g_data.parse_thread, attr, parse_thread_func,
                       &g_data) != 0) {
        fprintf(stderr, "Failed to create Parse thread\n");
        return ERR;
    }

    // 启动老化线程（绑定到CPU2）
    CPU_ZERO(&cpuset);
    CPU_SET(2, &cpuset);
    pthread_attr_setaffinity_np(attr, sizeof(cpu_set_t), &cpuset);
    if (pthread_create(&g_data.aging_thread, attr, aging_thread_func,
                       &g_data) != 0) {
        fprintf(stderr, "Failed to create Aging thread\n");
        return ERR;
    }

    // 启动重传线程（绑定到CPU3）
    CPU_ZERO(&cpuset);
    CPU_SET(3, &cpuset);
    pthread_attr_setaffinity_np(attr, sizeof(cpu_set_t), &cpuset);
    if (pthread_create(&g_data.sr_retrans_thread, attr, sr_retrans_thread_func,
                       &g_data) != 0) {
        fprintf(stderr, "Failed to create Retransmit thread\n");
        return ERR;
    }

    return OK;
}

// 启动定时器系统
static int start_timer_system(void) {
    // 初始化延时处理系统
    if (init_delay_processing_system(&g_data) != 0) {
        fprintf(stderr, "Failed to init delay processing system\n");
        return ERR;
    }

    // 启动定时器epoll线程
    if (pthread_create(&g_data.timer_epoll_thread, NULL,
                       timer_epoll_thread_func, &g_data) != 0) {
        fprintf(stderr, "Failed to create timer epoll thread\n");
        return ERR;
    }

    // 启动统一延时处理线程
    if (pthread_create(&g_data.delay_proc_thread, NULL, delay_proc_thread_func,
                       &g_data) != 0) {
        fprintf(stderr, "Failed to create delay processing thread\n");
        return ERR;
    }

    return OK;
}

// 主循环
static void main_loop(void) {
    uint64_t last_health_check_time = rte_get_timer_cycles();
    uint64_t last_stat_time = rte_get_timer_cycles();

    printf("✓ 所有线程已启动\n");
    printf("连接表: %u 个桶, PSN数组大小: %d\n",
           g_data.cache_tbl_v4->num_buckets, MAX_PSN_ARRAY);
    printf("全局老化间隔: %d ms, 连接老化间隔: %d ms, 重传间隔: %d ms\n\n",
           AGING_INTERVAL, SESSION_AGING_INTERVAL, RETRANSMIT_INTERVAL);
    printf("按 Ctrl+C 停止程序\n");
    printf("========================================\n\n");

    while (!g_data.stop) {
        sleep(5);

        uint64_t current_time = rte_get_timer_cycles();

        // 每30秒进行一次健康检查
        if (current_time - last_health_check_time >
            (THREAD_CHECK_INTERVAL * rte_get_timer_hz() / 1000)) {
            if (!check_thread_health()) {
                printf("线程健康检查失败，考虑重启程序\n");
            } else {
                printf("所有线程运行正常\n");
            }
            last_health_check_time = current_time;
        }
    }
}

// 错误处理
static void handle_init_error(int stage, pthread_attr_t *attr) {
    fprintf(stderr, "\n程序启动失败(阶段: %u)，正在清理...\n", stage);

    if (attr)
        pthread_attr_destroy(attr);
    cleanup_resources();
}

int main(int argc, char *argv[]) {
    int ret;
    pthread_attr_t attr;

    // 阶段1: 初始化系统
    ret = init_system(argc, argv);
    if (ret != OK) {
        handle_init_error(1, NULL);
        return ERR;
    }

    // 阶段2: 创建内存池和队列
    ret = create_memory_pools_and_queues();
    if (ret != OK) {
        handle_init_error(2, NULL);
        return ERR;
    }

    // 阶段3: 创建连接表
    ret = create_connection_table();
    if (ret != OK) {
        handle_init_error(3, NULL);
        return ERR;
    }

    // 阶段4: 初始化网络接口
    ret = init_network_interface();
    if (ret != OK) {
        handle_init_error(4, NULL);
        return ERR;
    }

    // 阶段5: 初始化线程属性
    ret = init_thread_attributes(&attr);
    if (ret != OK) {
        handle_init_error(5, NULL);
        return ERR;
    }

    // 阶段6: 初始化全局数据
    uint64_t current_time = rte_get_timer_cycles();
    g_data.rx_thread_last_active = current_time;
    g_data.parse_thread_last_active = current_time;
    g_data.aging_thread_last_active = current_time;
    g_data.sr_retrans_thread_last_active = current_time;
    g_data.delay_proc_thread_last_active = current_time;
    g_data.stop = 0;

    // 阶段7: 启动工作线程
    ret = start_worker_threads(&attr);
    if (ret != OK) {
        handle_init_error(7, &attr);
        return ERR;
    }

    // 阶段8: 启动定时器系统
    ret = start_timer_system();
    if (ret != OK) {
        handle_init_error(8, &attr);
        return ERR;
    }

    pthread_attr_destroy(&attr);

    // 阶段9: 进入主循环
    main_loop();

    // 阶段10: 正常清理
    printf("\n正在停止系统...\n");
    cleanup_resources();

    return OK;
}
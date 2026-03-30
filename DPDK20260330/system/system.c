#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "system/system.h"
#include "config.h"
#include "global.h"
#include "network/network.h"
#include "tables/conn_table.h"
#include "tables/flow_table.h"
#include "threads/thread_functions.h"
#include "utils/debug.h"
#include "utils/utils.h"
#include <rte_errno.h>
#include <signal.h>

enum gateway_role self_role = DST_GATEWAY;

// 等待线程退出
static void wait_for_threads_exit(void) {
    if (g_data.rx_thread) {
        pthread_join(g_data.rx_thread, NULL);
        printf("[SYSTEM] 接收线程已退出\n");
        g_data.rx_thread = 0;
    }

    if (g_data.parse_thread) {
        pthread_join(g_data.parse_thread, NULL);
        printf("[SYSTEM] 解析线程已退出\n");
        g_data.parse_thread = 0;
    }

    // ==========================================
    // 【新增4】任务线程退出
    // ==========================================
    if (g_data.task_thread) {
        pthread_join(g_data.task_thread, NULL);
        printf("[SYSTEM] 任务线程已退出\n");
        g_data.aging_thread = 0;
    }

    // ==========================================
    // 【新增3】老化线程退出
    // ==========================================
    if (g_data.aging_thread) {
        pthread_join(g_data.aging_thread, NULL);
        printf("[SYSTEM] 老化线程已退出\n");
        g_data.aging_thread = 0;
    }

    // ==========================================
    // 【新增5】重传线程退出
    // ==========================================
    if (g_data.retrans_thread) {
        pthread_join(g_data.retrans_thread, NULL);
        printf("[SYSTEM] 重传线程已退出\n");
        g_data.retrans_thread = 0;
    }
}

// 销毁所有表资源
static void destroy_gw_resource(void) {
    destory_conn_table();
    printf("[SYSTEM] 连接表资源已释放\n");

    destory_flow_table();
    printf("[SYSTEM] 流表资源已释放\n");
}

// 资源清理函数
void cleanup_resources(void) {
    printf("[SYSTEM] 开始清理系统资源...\n");

    // 1. 设置停止标志
    g_data.stop = 1;

    // 2. 等待线程退出
    wait_for_threads_exit();
    printf("[SYSTEM] 所有线程退出\n");

    // 3. 网卡关闭
    close_port(g_data.port_id);
    printf("[SYSTEM] 网卡%u关闭\n", g_data.port_id);

    // 4. 销毁连接表、流表
    destroy_gw_resource();
    printf("[SYSTEM] 所有网关资源销毁\n");

    // ==========================================
    // 【新增2】：释放定时器子系统内存
    // ==========================================
    // 在EAL环境清理前，表资源清理后，调用finalize释放init时申请的内部结构
    rte_timer_subsystem_finalize();
    printf("[SYSTEM] 定时器子系统释放\n");

    // 5. 释放软件环
    if (g_data.rx_ring) {
        rte_ring_free(g_data.rx_ring);
        g_data.rx_ring = NULL;
        printf("[SYSTEM] 软件接收环释放\n");
    }
    if (g_data.tx_ring) {
        rte_ring_free(g_data.tx_ring);
        g_data.tx_ring = NULL;
        printf("[SYSTEM] 软件发送环释放\n");
    }
    if (g_data.task_ring) {
        rte_ring_free(g_data.task_ring);
        g_data.task_ring = NULL;
        printf("[SYSTEM] 软件任务环释放\n");
    }

    // 6. 释放内存池
    if (g_data.mbuf_pool) {
        rte_mempool_free(g_data.mbuf_pool);
        g_data.mbuf_pool = NULL;
        printf("[SYSTEM] 内存池释放\n");
    }

    // 7. EAL环境清理
    rte_eal_cleanup();
    printf("[SYSTEM] EAL环境清理\n");

    printf("[SYSTEM] 系统资源清理完成！\n");
}

// 信号处理函数
static void signal_handler(int sig) {
    printf("\n[SYSTEM]接收到信号%d ", sig);

    g_data.stop = 1; // 设置停止标志

    // 记录信号类型用于调试
    switch (sig) {
    case SIGHUP:
        printf("SIGHUP(终端挂起)");
        break;
    case SIGINT:
        printf("SIGINT(Ctrl+C)");
        break;
    case SIGQUIT:
        printf("IGQUIT(Ctrl+\\)");
        break;
    case SIGSEGV:
        printf("SIGSEGV (内存访问违规)");
        break;
    case SIGTERM:
        printf("SIGTERM (终止请求)");
        break;
    default:
        printf("信号类型:%d\n", sig);
        break;
    }
    printf("\n");
}

// 注册信号处理器
static int setup_signal_handlers() {
    struct sigaction sa;

    // 也可以使用下面函数直接基于信号注册回调函数
    // signal(SIGHUP, signal_handler);

    // 设置信号处理函数
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    // 推荐加上 SA_RESTART，防止慢速系统调用被信号中断而产生EINTR错误
    // sa.sa_flags = 0;
    sa.sa_flags = SA_RESTART;

    int catch_signals[] = {SIGHUP, SIGINT, SIGQUIT, SIGTERM};
    size_t num_signals = sizeof(catch_signals) / sizeof(catch_signals[0]);

    for (size_t i = 0; i < num_signals; ++i) {
        int sig = catch_signals[i];
        if (sigaction(sig, &sa, NULL) == -1) {
            // 使用 strsignal 自动打印对应的信号名称
            dbg_err("无法注册信号 %d (%s) 处理器\n", sig, strsignal(sig));
            return ERR_INIT_SHUTDOWN;
        }
    }

    signal(SIGPIPE, SIG_IGN); // 忽略SIGPIPE信号（避免网络连接断开导致程序退出）

    return SUCCESS;
}

// 程序退出前的最后清理
static void atexit_clean(void) {
    printf("\n[SYSTEM] 程序退出，执行最终清理...\n");

    if (!g_data.stop) {
        g_data.stop = 1;
        // sleep(1);
    }

    cleanup_resources();
}

// 优雅退出机制初始化
int init_graceful_shutdown(void) {
    // 1. 优雅退出机制注册
    if (atexit(atexit_clean) != 0) {
        dbg_err("退出处理函数注册失败\n");
        return ERR_INIT_SHUTDOWN;
    } // 注册退出处理函数

    int ret = setup_signal_handlers(); // 注册信号处理器
    if (ret == ERR_INIT_SHUTDOWN) {
        dbg_err("信号处理器注册失败\n");
        return ret;
    }

    return SUCCESS;
}

// DPDK初始化
int init_dpdk(int argc, char *argv[]) {
    // EAL初始化
    printf("[SYSTEM] 开始EAL初始化...\n");
    int ret = rte_eal_init(argc, argv);
    if (ret < 0) {
        dbg_err("EAL初始化失败\n");
        return ERR_INIT_DPDK;
    }
    argc -= ret;
    argv += ret; // 剥离EAL参数，留给后续可能有用的应用层参数
    printf("[SYSTEM] EAL初始化成功\n");

    // mempool创建
    printf("[SYSTEM] 开始内存池创建...\n");
    g_data.mbuf_pool =
        rte_pktmbuf_pool_create("mbuf_pool", NUM_MBUFS, MBUF_CACHE_SIZE, 0,
                                MBUF_BUF_SIZE, rte_socket_id());
    if (!g_data.mbuf_pool) {
        dbg_err("内存池创建失败: %s\n", rte_strerror(rte_errno));
        return ERR_INIT_DPDK;
    }
    printf("[SYSTEM] 内存池创建成功\n");

    // NIC配置
    printf("[SYSTEM] 开始网卡%u初始化...\n", g_data.port_id);
    ret = init_port(g_data.port_id, g_data.rx_queue_id, g_data.tx_queue_id);
    if (ret != 0) {
        dbg_err("网卡初始化失败\n");
        return ERR_INIT_DPDK;
    }
    printf("[SYSTEM] 网卡%u启动成功\n", g_data.port_id);

    // 软件环创建
    printf("[SYSTEM] 开始软件环创建...\n");
    g_data.rx_ring = rte_ring_create("rx_ring", USR_RING_SIZE, rte_socket_id(),
                                     RING_F_SP_ENQ | RING_F_SC_DEQ);
    if (!g_data.rx_ring) {
        dbg_err("软件接收环创建失败: %s\n", rte_strerror(rte_errno));
        return ERR_INIT_DPDK;
    }
    g_data.tx_ring = rte_ring_create("tx_ring", USR_RING_SIZE, rte_socket_id(),
                                     RING_F_SP_ENQ | RING_F_SC_DEQ);
    if (!g_data.tx_ring) {
        dbg_err("软件发送环创建失败: %s\n", rte_strerror(rte_errno));
        return ERR_INIT_DPDK;
    }
    // ==========================================
    // 【新增2】：创建延时任务环 (存自定义大小元素)
    // ==========================================
    // /* 【定时器ctx版】
    // elem_size必须是4的倍数
    unsigned int elem_size = sizeof(struct retrans_task);
    g_data.task_ring =
        rte_ring_create_elem("task_ring", elem_size, USR_RING_SIZE,
                             rte_socket_id(), RING_F_SP_ENQ | RING_F_SC_DEQ);
    if (!g_data.task_ring) {
        dbg_err("软件任务环创建失败: %s\n", rte_strerror(rte_errno));
        return ERR_INIT_DPDK;
    }
    // */
    /*【定时器malloc版】
    g_data.task_ring =
        rte_ring_create("task_ring", USR_RING_SIZE, rte_socket_id(),
                        RING_F_SP_ENQ | RING_F_SC_DEQ);
    if (!g_data.tx_ring) {
        dbg_err("软件任务环创建失败: %s\n", rte_strerror(rte_errno));
        return ERR_INIT_DPDK;
    }
    */
    printf("[SYSTEM] 软件环创建成功\n");

    // ==========================================
    //【新增2】定时器子系统初始化
    // ==========================================
    printf("[SYSTEM] 开始定时器子系统初始化...\n");
    ret = rte_timer_subsystem_init();
    if (ret < 0) {
        dbg_err("定时器子系统初始化失败\n");
        return ERR_INIT_DPDK;
    }
    printf("[SYSTEM] 定时器子系统初始化成功\n");

    return SUCCESS;
}

// 配置一条双向流规则
static int configure_bidirectional_flow_rule(const char *src_ip_str,
                                             const char *dst_ip_str,
                                             uint32_t src_qp, uint32_t dst_qp,
                                             uint16_t pkey) {
    uint32_t src_ip = ip_str_to_host_v4(src_ip_str);
    uint32_t dst_ip = ip_str_to_host_v4(dst_ip_str);

    if (src_ip == 0 || dst_ip == 0) {
        dbg_err("IP地址格式无效\n");
        return ERR_INIT_GW_RESOURCE;
    }

    struct flow_key_v4 fwd_key = {.src_ip = src_ip,
                                  .dst_ip = dst_ip,
                                  .dst_qp = dst_qp,
                                  .pkey = pkey,
                                  .resv = 0};

    int ret = add_flow_rule(&fwd_key, src_qp, (enum gateway_role)SRC_GATEWAY);
    if (ret == ERR_FLOW_RULE_ADD) {
        return ret;
    }

    struct flow_key_v4 rev_key = {.src_ip = dst_ip,
                                  .dst_ip = src_ip,
                                  .dst_qp = src_qp,
                                  .pkey = pkey,
                                  .resv = 0};

    ret = add_flow_rule(&rev_key, dst_qp, (enum gateway_role)DST_GATEWAY);
    if (ret == ERR_FLOW_RULE_ADD) {
        return ret;
    }

    printf("[FLOW] 双向规则添加成功: %s(QP:%u) <--> %s(QP:%u)\n", src_ip_str,
           src_qp, dst_ip_str, dst_qp);

    return SUCCESS;
}

// 命令和流表插入
static int cli_bidirectional_flow_rule(void) {
    int num;

    printf("请输入待配置的流规则条数：");
    if (scanf("%d", &num) != 1) {
        printf("[SYSTEM] 输入无效，请输入一个数字\n");

        //清空标准输入缓冲区里的错误字符，防止死循环
        int c;
        while ((c = getchar()) != '\n' && c != EOF) {
        }

        return ERR_INIT_GW_RESOURCE;
    }

    char src_ip[16], dst_ip[16];
    uint32_t src_qp, dst_qp;
    uint16_t pkey;

    for (int i = 0; i < num; i++) {
        printf("请输入第%d条双向流规则信息\n", i + 1);
        printf("Source IP: ");
        if (scanf("%15s", src_ip) != 1)
            return ERR_INIT_GW_RESOURCE;
        printf("Destination IP: ");
        if (scanf("%15s", dst_ip) != 1)
            return ERR_INIT_GW_RESOURCE;
        printf("Source QP: ");
        if (scanf("%u", &src_qp) != 1)
            return ERR_INIT_GW_RESOURCE;
        printf("Destination QP: ");
        if (scanf("%u", &dst_qp) != 1)
            return ERR_INIT_GW_RESOURCE;
        printf("请输入PKey (Hex, e.g., ffff): ");
        if (scanf("%hx", &pkey) != 1)
            pkey = 0xffff;

        int ret = configure_bidirectional_flow_rule(src_ip, dst_ip, src_qp,
                                                    dst_qp, pkey);
        if (ret == ERR_FLOW_RULE_ADD)
            return ERR_INIT_GW_RESOURCE;
    }

    int ret = rte_hash_count(g_data.flow_hash);
    return ret;
}

// 网关资源初始化
int init_gw_resources(void) {
    int ret = init_flow_table();
    if (ret == ERR_INIT_GW_RESOURCE) {
        dbg_err("流表初始化失败！\n");
        return ret;
    }
    printf("[SYSTEM] 流表初始化成功\n");

    printf("[SYSTEM] 进行双向流表配置......\n");
    ret = cli_bidirectional_flow_rule();
    if (ret == ERR_INIT_GW_RESOURCE) {
        dbg_err("流表配置失败！\n");
        return ret;
    }
    printf("[SYSTEM] 流表配置成功，总计：%d\n", ret);

    printf("请配置网关角色(0:SRC 1:DST)：");
    if (scanf("%d", &ret) != 1) {
        printf("[SYSTEM] 输入无效，请输入一个数字\n");

        //清空标准输入缓冲区里的错误字符，防止死循环
        int c;
        while ((c = getchar()) != '\n' && c != EOF) {
        }

        return ERR_INIT_GW_RESOURCE;
    } else {
        self_role = (enum gateway_role)ret;
    }

    ret = init_conn_table();
    if (ret == ERR_INIT_GW_RESOURCE) {
        dbg_err("连接表初始化失败！\n");
        return ret;
    }
    printf("[SYSTEM] 连接表初始化成功\n");

    printf("[SYSTEM] 所有网关资源初始化成功\n");

    return SUCCESS;
}

// 工作线程启动
int start_worker_threads(pthread_attr_t *attr) {
    // 线程属性初始化
    printf("[SYSTEM] 开始线程属性初始化...\n");

    int ret = pthread_attr_init(attr);
    if (ret != 0) {
        dbg_err("线程属性初始化失败\n");
        return ERR_INIT_THREADS;
    }
    pthread_attr_setdetachstate(attr, PTHREAD_CREATE_JOINABLE);

    printf("[SYSTEM] 线程属性初始化成功\n");

    // 初始化全局数据
    uint64_t current_time = rte_get_timer_cycles();
    g_data.rx_thread_last_active = current_time;
    g_data.parse_thread_last_active = current_time;
    g_data.task_thread_last_active = current_time;    // 【新增4】
    g_data.aging_thread_last_active = current_time;   // 【新增3】
    g_data.retrans_thread_last_active = current_time; // 【新增5】

    cpu_set_t cpuset;

    // 线程启动
    printf("[SYSTEM] 开始线程启动...\n");

    // 启动收包线程（绑定到CPU1）
    CPU_ZERO(&cpuset);
    CPU_SET(1, &cpuset);
    pthread_attr_setaffinity_np(attr, sizeof(cpu_set_t), &cpuset);
    if (pthread_create(&g_data.rx_thread, attr, rx_thread_func, &g_data) != 0) {
        dbg_err("接收线程启动失败\n");
        return ERR_INIT_THREADS;
    }
    printf("[SYSTEM] 接收线程启动成功(绑定到CPU1)\n");

    // 启动收包线程（绑定到CPU2）
    CPU_ZERO(&cpuset);
    CPU_SET(2, &cpuset);
    pthread_attr_setaffinity_np(attr, sizeof(cpu_set_t), &cpuset);
    if (pthread_create(&g_data.parse_thread, attr, parse_thread_func,
                       &g_data) != 0) {
        dbg_err("解析线程启动失败\n");
        return ERR_INIT_THREADS;
    }
    printf("[SYSTEM] 解析线程启动成功(绑定到CPU2)\n");

    // ==========================================
    // 【新增4】任务调度线程（绑定到CPU3）
    // ==========================================
    CPU_ZERO(&cpuset);
    CPU_SET(3, &cpuset);
    pthread_attr_setaffinity_np(attr, sizeof(cpu_set_t), &cpuset);
    if (pthread_create(&g_data.aging_thread, attr, task_thread_func, &g_data) !=
        0) {
        dbg_err("任务线程启动失败\n");
        return ERR_INIT_THREADS;
    }
    printf("[SYSTEM] 任务线程启动成功(绑定到CPU3)\n");

    // 清除绑核属性，恢复为默认属性
    pthread_attr_destroy(attr);
    pthread_attr_init(attr);
    pthread_attr_setdetachstate(attr, PTHREAD_CREATE_JOINABLE);

    // ==========================================
    // 【新增3】启动老化线程（自由调度）
    // ==========================================
    if (pthread_create(&g_data.aging_thread, attr, aging_thread_func,
                       &g_data) != 0) {
        dbg_err("老化线程启动失败\n");
        return ERR_INIT_THREADS;
    }
    printf("[SYSTEM] 老化线程启动成功(自由调度)\n");

    // ==========================================
    // 【新增5】启动重传线程（自由调度）
    // ==========================================
    if (pthread_create(&g_data.retrans_thread, attr, retrans_thread_func,
                       &g_data) != 0) {
        dbg_err("重传线程启动失败\n");
        return ERR_INIT_THREADS;
    }
    printf("[SYSTEM] 重传线程启动成功(自由调度)\n");

    printf("[SYSTEM] 所有线程启动成功\n");

    return SUCCESS;
}
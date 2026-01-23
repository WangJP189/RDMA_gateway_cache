#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <signal.h>
#include <unistd.h>

#include <errno.h>
#include <sys/select.h>

#include "pkt_cache.h"
#include "pkt_recv.h"

// ZPY
#include <net/if.h> // if_nametoindex()-将接口名转换为索引号
// ZPY

// 全局控制变量
struct connection_bucket *g_conn_buckets = NULL; // 哈希桶全局指针
volatile sig_atomic_t g_running = 1;             // 全局线程控制标记
pthread_t g_aging_tid = 0;                       // 老化线程ID
uint32_t g_total_cleaned_conn = 0;               // 累计清理连接数
pthread_t g_receiver_thread = 0;                 // 全局接收线程ID

// ZPY
pthread_t g_retransmit_thread = 0; // 全局重传线程ID
int g_retransmit_sockfd = -1;      // 全局AF_PACKET传输的socket_fd
int g_tcp_server_sockfd = -1;      // 全局服务端TCP监听socket_fd
int g_sr_client_sockfd = -1;       // 全局客户端socket_fd
char g_sr_target_ip[32] = {0};     // 全局SR服务端目标IP
int g_sr_target_port = 0;          // 全局SR服务端目标端口
char g_dst_gbn_interface_name[IF_NAMESIZE] = {0}; // 全局目的网关GBN重传接口名称
char g_src_nack_interface_name[IF_NAMESIZE] = {0}; // 全局源网关发送NACK接口名称
char g_src_sr_interface_name[IF_NAMESIZE] = {0}; // 全局源网关发送SR数据接口名称
// ZPY

/**
 * 清理资源
 */
void cleanup_resources(void) {
    printf("正在清理资源...\n");

    // 等待所有线程结束
    // 等待线程的顺序很重要，应该按照依赖关系等待
    stop_rx_thread();
    stop_retransmit_thread();
    stop_age_thread();

    // 线程资源释放
    cleanup_rx_resources();
    cleanup_retransmit_resources();
    cleanup_age_resources();

    // 调用新的销毁接口
    destroy_flow_tables();
    destroy_connection_table();

    printf("资源清理完成\n");
}

/**
 * 信号处理函数 - 用于优雅退出
 */
void signal_handler(int sig) {
    printf("\n接收到信号 %d ，回车强制退出\n", sig);
    g_running = 0;
}

/**
 * 注册信号处理器
 */
void setup_signal_handlers(void) {
    signal(SIGHUP, signal_handler);  // 终端挂起  1
    signal(SIGINT, signal_handler);  // Ctrl+C  2
    signal(SIGSEGV, signal_handler); // 段错误（内存访问违规）  11
    signal(SIGTERM, signal_handler); // 终止请求信号  15
}

/**
 * 显示主菜单
 */
void display_menu(void) {
    printf("\n==== RDMA 网关系统====\n");
    printf("1. 创建流表\n");
    printf("2. 查看流表\n");
    printf("3. 启动报文接收\n");
    printf("4. 停止报文接收\n");

    printf("--- 本端作为服务端 (接收对端请求) ---\n");
    // ZPY
    printf("5. 启动服务端SR重传线程\n");

    printf("--- 本端作为客户端 (发送SR请求) ---\n");
    printf("6. 配置对端地址\n");

    printf("7. 设置AF_PACKET重传的全局socket\n");
    printf("8. 设置目的网关和源网关AF_PACKET接口名称\n");

    printf("9. 启动全局资源老化线程\n");
    // ZPY

    printf("0. 退出程序\n");
    printf("请选择操作 (0-11): ");
}

/**
 * 功能1: 创建流表 (适配新 API)
 */
void create_flow_table(void) {
    char src_ip[16], dst_ip[16];
    uint32_t src_qp, dst_qp;
    uint16_t pkey; // 新增

    printf("\n------ 创建流表规则 (Flow Rule) -----\n");

    printf("请输入源IP: ");
    if (scanf("%15s", src_ip) != 1)
        return;

    printf("请输入目的IP: ");
    if (scanf("%15s", dst_ip) != 1)
        return;

    printf("请输入源QP号: ");
    if (scanf("%u", &src_qp) != 1)
        return;

    printf("请输入目的QP号: ");
    if (scanf("%u", &dst_qp) != 1)
        return;

    // 新增PKey输入
    printf("请输入PKey (Hex, e.g., ffff): ");
    if (scanf("%hx", &pkey) != 1)
        pkey = 0xffff;

    // 调用新的流表接口 (同时创建正向和反向规则)
    if (add_to_flow_table(src_ip, dst_ip, src_qp, dst_qp, pkey) == 0) {
        printf("流表规则创建成功！\n");
    } else {
        printf("流表规则创建失败！\n");
    }
    printf("---------------------\n");
}

/**
 * 功能2: 查看流表
 */
void check_flow_table(void) {
    printf("\n------ 查看流表 (Flow Tables) -----\n");
    int count = 0;

    // 遍历正向表
    for (int i = 0; i < TABLE_SIZE; i++) {
        struct flow_entry *curr = g_flow_table_forward[i];
        while (curr) {
            print_flow_entry(curr, "FWD");
            curr = curr->next;
            count++;
        }
    }
    // 遍历反向表
    for (int i = 0; i < TABLE_SIZE; i++) {
        struct flow_entry *curr = g_flow_table_reverse[i];
        while (curr) {
            print_flow_entry(curr, "REV");
            curr = curr->next;
            count++;
        }
    }

    if (count == 0)
        printf("流表为空\n");
    printf("---------------------\n");
}

/**
 * 功能3: 启动报文接收
 */
void start_receiver(void) {
    char interface[16];
    printf("\n---- 启动报文接收 ----\n");

    if (g_receiver_thread > 0) {
        printf("报文接收已经在运行中！\n");
        return;
    }

    printf("请输入网络接口名: ");
    if (scanf("%15s", interface) != 1) {
        printf("输入无效\n");
        while (getchar() != '\n')
            ; // 清空输入缓冲区
        return;
    }

    printf("正在启动报文接收，接口: %s\n", interface);

    // 复制接口名字符串，确保线程安全
    char *interface_copy = strdup(interface);
    if (interface_copy == NULL) {
        perror("内存分配失败");
        return;
    }

    if (start_rx_thread(interface_copy) != 0) {
        free(interface_copy); // 启动失败时释放内存
        return;
    }

    printf("按回车键返回主菜单（报文接收继续在后台运行）\n");
    printf("---------------------\n");

    // 清空输入缓冲区
    while (getchar() != '\n')
        ;
}

/**
 * 功能4: 停止报文接收线程
 */
void stop_receiver(void) {
    printf("\n---- 停止报文接收 ----\n");

    if (g_receiver_thread <= 0) {
        printf("报文接收未运行\n");
        return;
    }

    printf("正在停止接收...\n");
    stop_rx_thread();
    cleanup_rx_resources();
    printf("---------------------\n");
}

// 功能5: 启动服务端SR重传线程
void menu_server_setup(void) {
    char ip[32];
    int port;
    // 这里监听全接口
    printf("请输入本地绑定IP (输入 0 代表 0.0.0.0): ");
    if (scanf("%31s", ip) != 1)
        return;
    if (strcmp(ip, "0") == 0)
        strcpy(ip, "0.0.0.0");

    printf("请输入本地监听端口: ");
    if (scanf("%d", &port) != 1)
        return;

    start_retransmit_thread(port);
}

// 功能6: 配置对端信息 (客户端 Config)
void menu_config_sr(void) {
    char ip[32];
    int port;
    printf("请输入对端(SR服务端)IP: ");
    if (scanf("%31s", ip) != 1)
        return;
    printf("请输入对端(SR服务端)端口: ");
    if (scanf("%d", &port) != 1)
        return;

    set_sr_client_target_info(ip, port);
}

// ZPY
/**
 * [功能7: retransmit_rdma_packet全局创建一个af_packet的socket
 */
void menu_create_retransmit_socket(void) {
    int sockfd = get_retransmit_sockfd();
    if (sockfd < 0) {
        printf("创建重传socket失败\n");
    } else {
        printf("重传socket创建成功，sockfd=%d\n", sockfd);
    }
    return;
}

// ZPY
/**
 * 功能8: 设置目的网关和源网关AF_PACKET接口名称
 */
void menu_set_gateway_interfaces(void) {
    char dst_gbn_interface[IF_NAMESIZE];
    char src_nack_interface[IF_NAMESIZE];
    char src_sr_interface[IF_NAMESIZE];

    printf("DST GBN Interface Name: ");
    if (scanf("%15s", dst_gbn_interface) != 1)
        return;

    printf("SRC NACK Interface Name: ");
    if (scanf("%15s", src_nack_interface) != 1)
        return;

    printf("SRC SR Interface Name: ");
    if (scanf("%15s", src_sr_interface) != 1)
        return;

    // 复制到全局变量
    strncpy(g_dst_gbn_interface_name, dst_gbn_interface, IF_NAMESIZE - 1);
    g_dst_gbn_interface_name[IF_NAMESIZE - 1] = '\0';

    strncpy(g_src_nack_interface_name, src_nack_interface, IF_NAMESIZE - 1);
    g_src_nack_interface_name[IF_NAMESIZE - 1] = '\0';

    strncpy(g_src_sr_interface_name, src_sr_interface, IF_NAMESIZE - 1);
    g_src_sr_interface_name[IF_NAMESIZE - 1] = '\0';

    printf("DST GBN Interface Name: %s\n", g_dst_gbn_interface_name);
    printf("SRC NACK Interface Name: %s\n", g_src_nack_interface_name);
    printf("SRC SR Interface Name: %s\n", g_src_sr_interface_name);
    return;
}

// ZPY
// 简单的资源老化线程启动
void menu_start_age_thread(void) {
    start_age_thread();
    return;
}

/**
 * 主程序循环
 */
void menu_loop(void) {
    int choice;

    setup_signal_handlers();
    while (g_running) {
        display_menu();

        if (scanf("%d", &choice) != 1) {
            printf("输入无效，请输入数字\n");
            while (getchar() != '\n')
                ; // 清空输入缓冲区
            continue;
        }

        if (!g_running)
            break;

        switch (choice) {
        case 0:
            printf("感谢使用，再见！\n");
            g_running = 0;
            break;
        case 1:
            create_flow_table();
            break;
        case 2:
            check_flow_table();
            break;
        case 3:
            start_receiver();
            break;
        case 4:
            stop_receiver();
            break;
        case 5:
            menu_server_setup();
            break;
        case 6:
            menu_config_sr();
            break;
        case 7:
            menu_create_retransmit_socket();
            break;
        case 8:
            menu_set_gateway_interfaces();
            break;
        case 9:
            menu_start_age_thread();
            break;
        default:
            printf("无效选择，请重新输入\n");
            break;
        }

        // 清空输入缓冲区，避免换行符影响下次输入
        while (getchar() != '\n')
            ;

        // 短暂暂停，让用户看到输出
        usleep(100000); // 100ms
    }
    cleanup_resources();
}

/**
 * 程序入口点
 */
int main(void) {
    // [新增] 系统级初始化：最先执行
    // 必须确保在任何线程启动前，锁和表结构已经准备好
    if (init_connection_table() != 0) {
        fprintf(stderr, "[FATAL] 连接表初始化失败，程序无法启动\n");
        return -1;
    }

    menu_loop();
    return 0;
}
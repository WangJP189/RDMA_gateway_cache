#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <signal.h>
#include <unistd.h>

#include <errno.h>
#include <sys/select.h>

#include "pkt_recv.h"
#include "pkt_cache.h"
#include "sr_control.h"

// 全局控制变量
static volatile sig_atomic_t g_shutdown_requested = 0;

/**
 * 清理资源
 */
void cleanup_resources() {
    printf("正在清理资源...\n");

    // 确保接收器已停止
    if (is_receiver_running()) {
        stop_packet_receiver();
    }
    
    // 调用新的销毁接口
    destroy_flow_tables();
    destroy_connection_table();

    cleanup_tcp();
    printf("资源清理完成\n");
}

/**
 * 信号处理函数 - 用于优雅退出
 */
void signal_handler(int sig) {

    printf("\n接收到信号 %d ，强制清理资源\n", sig);

    g_shutdown_requested = 1;
    cleanup_resources();
}

/**
 * 注册信号处理器
 */
void setup_signal_handlers() {

    signal(SIGHUP, signal_handler);   // 终端挂起  1
    signal(SIGINT, signal_handler);   // Ctrl+C  2
    // signal(SIGKILL, signal_handler);  // kill命令  9
    signal(SIGSEGV, signal_handler);   // 段错误（内存访问违规）  11
    signal(SIGTERM, signal_handler);  // 终止请求信号  15
}

/**
 * 显示主菜单
 */
void display_menu(void) {
    printf("\n==== RDMA 网关系统 ====\n");
    printf("1. 创建流表\n");
    printf("2. 查看流表\n");
    printf("3. 启动报文接收\n");
    printf("4. 停止报文接收\n");
    
    printf("5. 启动TCP Server\n");
    printf("6. 连接对端TCP Server\n");
    printf("7. 发送测试消息\n");

    printf("0. 退出程序\n");
    printf("请选择操作 (0-7): ");
}

/**
 * 功能1: 创建流表 (适配新 API)
 */
void create_flow_table() {
    if(g_shutdown_requested) return;

    char src_ip[16], dst_ip[16];
    uint32_t src_qp, dst_qp;
    uint16_t src_port, dst_port; // 新增
    uint16_t pkey;               // 新增
    
    printf("\n------ 创建流表规则 (Flow Rule) -----\n");
    
    printf("请输入源IP: ");
    if (scanf("%15s", src_ip) != 1) return;
    
    printf("请输入目的IP: ");
    if (scanf("%15s", dst_ip) != 1) return;

    // 新增端口输入
    printf("请输入源UDP端口 (默认0): ");
    if (scanf("%hu", &src_port) != 1) src_port = 0;

    printf("请输入目的UDP端口 (RoCE默认4791): ");
    if (scanf("%hu", &dst_port) != 1) dst_port = 4791;
    
    printf("请输入源QP号: ");
    if (scanf("%u", &src_qp) != 1) return;
    
    printf("请输入目的QP号: ");
    if (scanf("%u", &dst_qp) != 1) return;

    // 新增PKey输入
    printf("请输入PKey (Hex, e.g., ffff): ");
    if (scanf("%hx", &pkey) != 1) pkey = 0xffff;
    
    // 调用新的流表接口 (同时创建正向和反向规则)
    if (add_to_flow_table(src_ip, dst_ip, src_port, dst_port, src_qp, dst_qp, pkey) == 0) {
        printf("流表规则创建成功！\n");
    } else {
        printf("流表规则创建失败！\n");
    }
    printf("---------------------\n");
}

/**
 * 功能2: 查看流表
 */
void check_flow_table() {
    if(g_shutdown_requested) return;

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

    if (count == 0) printf("流表为空\n");
    printf("---------------------\n");
}

/**
 * 功能3: 启动报文接收
 */
void start_receiver() {

    if(g_shutdown_requested) return;

    char interface[16];
    
    // const char* interface = "eth0";

    printf("\n---- 启动报文接收 ----\n");
    
    if (is_receiver_running()) {
        printf("报文接收已经在运行中！\n");
        return;
    }
    
    
    printf("请输入网络接口名: ");
    if (scanf("%15s", interface) != 1) {
        printf("输入无效\n");
        while (getchar() != '\n'); // 清空输入缓冲区
        return;
    }

    printf("正在启动报文接收，接口: %s\n", interface);
    
    // 复制接口名字符串，确保线程安全
    char* interface_copy = strdup(interface);
    if (interface_copy == NULL) {
        perror("内存分配失败");
        return;
    }
    
    if (start_packet_receiver(interface_copy) != 0) {
        //printf("启动失败！\n");
        free(interface_copy);  // 启动失败时释放内存
        return;
    }

    //printf("启动成功！\n");

    printf("按回车键返回主菜单（报文接收继续在后台运行）\n");
    
    printf("---------------------\n");

    // 清空输入缓冲区
    while (getchar() != '\n');
    //getchar(); // 等待用户按回车

}

/**
 * 功能4: 停止报文接收线程
 */
void stop_receiver() {

    if(g_shutdown_requested) return;

    printf("\n---- 停止报文接收 ----\n");

    if (!is_receiver_running()) {
        printf("报文接收未运行\n");
        return;
    }
    
    printf("正在停止接收...\n");

    if (stop_packet_receiver() != 0) {
        printf("停止失败\n");
    }

    printf("---------------------\n");

}

/**
 * 功能5: TCP Server启动
 */
void menu_start_server() {
    int port = 9090; // 默认端口
    printf("启动本地 TCP 监听端口 %d...\n", port);
    start_tcp_server_thread(port);
}

/**
 * 功能6: TCP Client连接
 */
void menu_connect_peer() {
    char ip[32];
    int port = 9090;
    
    printf("请输入对端网关 IP: ");
    if (scanf("%31s", ip) != 1) return;
    
    connect_to_gateway(ip, port);
}

/**
 * [功能7: TCP发送消息
 */
void menu_send_msg() {
    char msg[128];
    printf("请输入要发送的内容: ");
    if (scanf("%127s", msg) != 1) return;
    
    send_tcp_message(msg);
}


/**
 * 主程序循环
 */
void menu_loop() {

    printf("启动时 g_shutdown_requested = %d\n", g_shutdown_requested);  // 验证初始值
    
    int choice;
    int running = 1;

    setup_signal_handlers();
    
    while (running && !g_shutdown_requested) {

        display_menu();

        // // 使用select实现非阻塞输入，避免scanf阻塞
        // fd_set readfds;
        // FD_ZERO(&readfds);
        // FD_SET(STDIN_FILENO, &readfds);
        // struct timeval tv = {1, 0}; // 1秒超时
        
        // int ret = select(STDIN_FILENO + 1, &readfds, NULL, NULL, &tv);
        // if (ret < 0) {
        //     perror("select error");
        //     break;
        // } else if (ret == 0) {
        //     // 超时，继续循环（检查退出标志）
        //     continue;
        // }
        
        if (scanf("%d", &choice) != 1) {
            printf("输入无效，请输入数字\n");
            while (getchar() != '\n'); // 清空输入缓冲区
            continue;
        }
        
        switch (choice) {
            case 0:
                printf("感谢使用，再见！\n");
                running = 0;
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
                menu_start_server();
                break;
            case 6:
                menu_connect_peer();
                break;
            case 7:
                menu_send_msg();
                break;
        
            default:
                printf("无效选择，请重新输入\n");
                break;
        }
        
        // 清空输入缓冲区，避免换行符影响下次输入
        while (getchar() != '\n');
        
        // 短暂暂停，让用户看到输出
        usleep(100000); // 100ms
    }

        cleanup_resources();
    
}

/**
 * 程序入口点
 */
int main() {
    
    // [新增] 系统级初始化：最先执行
    // 必须确保在任何线程启动前，锁和表结构已经准备好
    if (init_connection_table() != 0) {
        fprintf(stderr, "[FATAL] 连接表初始化失败，程序无法启动\n");
        return -1;
    }

    menu_loop();
    
    return 0;
}
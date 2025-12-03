/*
编译命令：
gcc -o simple_test simple_test.c ../251202/pkt_cache.c -lpthread

运行命令：
sudo ./simple_test
*/

// simple_test.c
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "../251202/pkt_cache.h"

int main() {
    printf("=== 简单缓存测试 ===\n");
    
    // 初始化
    g_cache_mgr = init_cache_manager(8);
    if (!g_cache_mgr) {
        printf("初始化失败\n");
        return 1;
    }
    
    // 测试数据
    char test_data[100];
    
    // 插入一些数据包
    for (int i = 1; i <= 100; i++) {
        snprintf(test_data, sizeof(test_data), "测试数据包 PSN=%d", i);
        int ret = add_to_batch_queue(
            "192.168.1.100", "192.168.1.200",
            100, 200,  // QP号
            i, (unsigned char*)test_data, strlen(test_data) + 1
        );
        
        if (ret == 0) {
            printf("插入成功: PSN=%d\n", i);
        } else {
            printf("插入失败: PSN=%d\n", i);
        }
        
        usleep(10000);  // 10ms间隔
    }
    
    // 测试查找
    printf("\n=== 开始查找测试 ===\n");
    for (int i = 1; i <= 100; i += 10) {
        struct cached_packet *pkt = find_packet_by_psn(
            "192.168.1.100", "192.168.1.200",
            100, 200,
            i
        );
        
        if (pkt) {
            printf("找到PSN=%d: %s\n", i, pkt->data);
            free(pkt);
        } else {
            printf("未找到PSN=%d\n", i);
        }
    }
    
    // 打印状态
    print_all_connections_status();
    
    // 清理
    destroy_cache_manager(g_cache_mgr);
    
    return 0;
}
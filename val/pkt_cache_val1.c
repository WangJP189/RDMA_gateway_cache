/*
编译命令：
gcc pkt_cache_val1.c ../251117/pkt_cache.c -o pkt_cache_val1 -lpthread
运行命令：
sudo ./pkt_cache_val1
*/


#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 使用头文件而非直接包含源文件（避免main函数冲突）
#include "../251117/pkt_cache.h"


// 验证基础插入与查询功能
void test_basic_insert_query() {
    printf("\n=== 测试1：基础插入与查询 ===\n");
    struct connection_key key = create_connection_key(
        "10.0.0.1", "10.0.0.2", 1234, 5678, 1, 2, 0, 0xffff
    );

    // 插入3个报文（PSN=10, 11, 12），指定窗口大小为100（窗口范围0-99）
    unsigned char data1[] = "test_data_10";
    unsigned char data2[] = "test_data_11";
    unsigned char data3[] = "test_data_12";
    int ret1 = add_to_connection_cache(
        "10.0.0.1", "10.0.0.2", 1234, 5678, 1, 2, 0, 0xffff, 10, data1, sizeof(data1), 100
    );
    int ret2 = add_to_connection_cache(
        "10.0.0.1", "10.0.0.2", 1234, 5678, 1, 2, 0, 0xffff, 11, data2, sizeof(data2), 100
    );
    int ret3 = add_to_connection_cache(
        "10.0.0.1", "10.0.0.2", 1234, 5678, 1, 2, 0, 0xffff, 12, data3, sizeof(data3), 100
    );

    printf("插入结果：ret1=%d, ret2=%d, ret3=%d（预期全为0）\n", ret1, ret2, ret3);

    // 查询PSN=10~12的报文
    int found_count;
    struct cached_packet* result = find_packets_by_psn_range(&key, 10, 12, &found_count);
    printf("查询结果：找到%d个报文（预期3个）\n", found_count);

    // 验证数据正确性
    struct cached_packet* curr = result;
    while (curr) {
        printf("  PSN=%u, 数据=%s（预期test_data_%u）\n", 
               curr->psn, curr->app_data, curr->psn);
        struct cached_packet* temp = curr;
        curr = curr->next;
        free(temp->app_data);
        free(temp);
    }
}

// 验证重复PSN处理
void test_duplicate_psn() {
    printf("\n=== 测试2：重复PSN处理 ===\n");
    struct connection_key key = create_connection_key(
        "10.0.0.1", "10.0.0.2", 1234, 5678, 1, 2, 0, 0xffff
    );

    // 插入PSN=20的报文，指定窗口大小为100（窗口范围0-99）
    unsigned char data_old[] = "old_data";
    add_to_connection_cache(
        "10.0.0.1", "10.0.0.2", 1234, 5678, 1, 2, 0, 0xffff, 20, data_old, sizeof(data_old), 100
    );

    // 插入相同PSN的新报文
    unsigned char data_new[] = "new_data";
    add_to_connection_cache(
        "10.0.0.1", "10.0.0.2", 1234, 5678, 1, 2, 0, 0xffff, 20, data_new, sizeof(data_new), 100
    );

    // 查询验证（修正PSN范围，之前写成了200）
    int found_count;
    struct cached_packet* result = find_packets_by_psn_range(&key, 20, 20, &found_count);
    printf("重复PSN查询：找到%d个报文（预期1个）\n", found_count);
    if (result) {
        printf("  数据=%s（预期new_data）\n", result->app_data);
        free(result->app_data);
        free(result);
    }
}

// 验证滑动窗口清理
void test_window_slide() {
    printf("\n=== 测试3：滑动窗口清理 ===\n");
    struct connection_key key = create_connection_key(
        "10.0.0.1", "10.0.0.2", 1234, 5678, 1, 2, 0, 0xffff
    );

    // 插入PSN=30~34的报文，指定窗口大小为100（窗口范围0-99）
    for (uint32_t psn = 30; psn <= 34; psn++) {
        char data[20];
        sprintf(data, "data_%u", psn);
        add_to_connection_cache(
            "10.0.0.1", "10.0.0.2", 1234, 5678, 1, 2, 0, 0xffff, psn, (unsigned char*)data, strlen(data)+1, 100
        );
    }

    // 初始状态
    print_all_connections_status();

    // 更新窗口起始到30（窗口范围变为30~129）
    int ret = update_window_start(&key, 30);
    printf("更新窗口结果：%d（预期0）\n", ret);

    // 验证清理结果
    int found_count;
    struct cached_packet* result = find_packets_by_psn_range(&key, 30, 34, &found_count);
    printf("窗口滑动后查询：找到%d个报文（预期5个：30、31、32、33、34）\n", found_count);

    // 释放查询结果
    struct cached_packet* curr = result;
    while (curr) {
        struct cached_packet* temp = curr;
        curr = curr->next;
        free(temp->app_data);
        free(temp);
    }
}

// 验证容量限制
void test_capacity_limit() {
    printf("\n=== 测试4：容量限制 ===\n");
    // 先清理之前的连接，避免干扰
    cleanup_expired_connections();
    
    // 重新初始化一个小容量的管理器（每连接最多2个报文）
    struct cache_manager* mgr = init_cache_manager(10, 10, 2, 1, 300);
    if (!mgr) {
        fprintf(stderr, "初始化缓存管理器失败\n");
        return;
    }
    
    struct connection_key key = create_connection_key(
        "10.0.0.1", "10.0.0.2", 1234, 5678, 1, 2, 0, 0xffff
    );

    // 插入3个报文（PSN=10,11,12），窗口大小100（0-99）
    unsigned char data[] = "test";
    int ret1 = add_to_connection_cache(
        "10.0.0.1", "10.0.0.2", 1234, 5678, 1, 2, 0, 0xffff, 10, data, sizeof(data), 100
    );
    int ret2 = add_to_connection_cache(
        "10.0.0.1", "10.0.0.2", 1234, 5678, 1, 2, 0, 0xffff, 11, data, sizeof(data), 100
    );
    int ret3 = add_to_connection_cache(
        "10.0.0.1", "10.0.0.2", 1234, 5678, 1, 2, 0, 0xffff, 12, data, sizeof(data), 100
    );

    printf("插入结果：ret1=%d, ret2=%d, ret3=%d（预期0,0,-1）\n", ret1, ret2, ret3);
}

// 新增测试：验证NACK批量重传功能
void test_nack_retransmit() {
    printf("\n=== 测试5：NACK批量重传 ===\n");
    // 先清理之前的连接，避免干扰
    cleanup_expired_connections();
    
    struct connection_key key = create_connection_key(
        "10.0.0.1", "10.0.0.3", 8080, 9090, 3, 4, 1, 0xff00
    );

    // 插入PSN=10~15的报文，窗口大小200（0-199）
    for (uint32_t psn = 10; psn <= 15; psn++) {
        char data[20];
        sprintf(data, "retrans_data_%u", psn);
        add_to_connection_cache(
            "10.0.0.1", "10.0.0.3", 8080, 9090, 3, 4, 1, 0xff00, 
            psn, (unsigned char*)data, strlen(data)+1, 200
        );
    }

    // 触发NACK重传（从PSN=13开始）
    printf("触发NACK重传（PSN=13及以后）:\n");
    handle_nack_batch_retransmit(&key, 13);
}

int main() {
    // 初始化全局缓存管理器（默认参数）
    struct cache_manager* g_cache_mgr = init_cache_manager(1024, 100, 100, 10, 300);
    if (!g_cache_mgr) {
        fprintf(stderr, "初始化缓存管理器失败\n");
        return 1;
    }

    // 执行所有测试
    test_basic_insert_query();
    test_duplicate_psn();
    test_window_slide();
    test_capacity_limit();
    test_nack_retransmit();  // 新增测试

    // 清理资源
    cleanup_expired_connections();
    printf("\n所有测试完成\n");
    return 0;
}
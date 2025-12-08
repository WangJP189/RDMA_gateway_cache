/*
编译命令：
gcc pkt_cache_val1_1208.c ../251208/pkt_cache.c -o pkt_cache_val1_1208 -lpthread -lpcap

运行命令：
sudo ./pkt_cache_val1_1208


gcc pkt_cache_val1_1208.c -o pkt_cache_val1_1208 -lpcap
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>

#include "../251208/pkt_cache.c"

// 模拟连接信息
typedef struct {
    char src_ip[INET_ADDRSTRLEN];
    char dst_ip[INET_ADDRSTRLEN];
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t src_qp;
    uint32_t dest_qp;
    uint32_t next_psn;
} simulated_connection;

// 模拟连接数组
simulated_connection connections[] = {
    {"192.168.1.100", "192.168.2.100", 4791, 4791, 1001, 2001, 1},
    {"192.168.1.101", "192.168.2.101", 4791, 4791, 1002, 2002, 1},
    {"192.168.1.102", "192.168.2.102", 4791, 4791, 1003, 2003, 1},
    {"192.168.1.103", "192.168.2.103", 4791, 4791, 1004, 2004, 1},
    {"192.168.1.104", "192.168.2.104", 4791, 4791, 1005, 2005, 1}
};
#define NUM_CONNECTIONS (sizeof(connections)/sizeof(connections[0]))

// 生成随机数据包
void generate_random_packet(unsigned char *data, int *len) {
    // 前n-1个包为MTU大小，最后一个为随机小尺寸
    static int packet_counter = 0;
    if (packet_counter % 10 != 9) {  // 模拟每10个包有一个小尺寸包
        *len = MTU_SIZE;
    } else {
        *len = 100 + (rand() % (MTU_SIZE - 100));  // 100到MTU之间的随机大小
    }
    packet_counter++;
    
    // 填充随机数据
    for (int i = 0; i < *len; i++) {
        data[i] = rand() % 256;
    }
}

// 模拟发送数据包线程
void *simulate_sender_thread(void *arg) {
    int thread_id = *(int *)arg;
    free(arg);
    
    printf("发送线程 %d 启动\n", thread_id);
    
    // 每个线程负责一个连接
    int conn_idx = thread_id % NUM_CONNECTIONS;
    simulated_connection *conn = &connections[conn_idx];
    
    unsigned char data[MEM_BLOCK_SIZE];
    int data_len;
    
    // 发送1000个数据包
    for (int i = 0; i < 1000; i++) {
        // 生成随机数据包
        generate_random_packet(data, &data_len);
        
        // 随机产生乱序（30%概率乱序）
        uint32_t current_psn = conn->next_psn;
        if (rand() % 10 < 3 && conn->next_psn > 3) {
            current_psn = conn->next_psn - (1 + rand() % 3);  // 回退1-3个PSN
        } else {
            conn->next_psn++;
        }
        
        // 添加到缓存
        int ret = add_packet_to_cache(
            conn->src_ip, conn->dst_ip,
            conn->src_port, conn->dst_port,
            conn->src_qp, conn->dest_qp,
            current_psn, data, data_len
        );
        
        if (ret == 0) {
            if (i % 100 == 0) {
                printf("线程 %d 发送数据包: 连接 %d, PSN=%u, 长度=%d\n",
                       thread_id, conn_idx, current_psn, data_len);
            }
        } else {
            printf("线程 %d 发送失败: 连接 %d, PSN=%u\n",
                   thread_id, conn_idx, current_psn);
        }
        
        // 随机休眠一小段时间，模拟网络延迟
        usleep(100 + (rand() % 500));
    }
    
    printf("发送线程 %d 完成\n", thread_id);
    return NULL;
}

int main() {
    printf("=== RDMA缓存模拟验证程序 ===\n");
    srand(time(NULL));
    
    // 初始化缓存管理器
    g_cache_mgr = init_cache_manager(16);
    if (!g_cache_mgr) {
        fprintf(stderr, "缓存管理器初始化失败\n");
        return 1;
    }
    
    // 打印配置信息
    printf("配置参数:\n");
    printf("  最大连接数: %d\n", MAX_CONNECTIONS);
    printf("  内存块大小: %d bytes\n", MEM_BLOCK_SIZE);
    printf("  缓冲区容量: %d\n", BUFFER_CAPACITY);
    printf("  批量阈值: %d\n", BATCH_THRESHOLD);
    printf("  批量超时: %d ms\n", BATCH_TIMEOUT_MS);
    printf("  初始内存块数: %d\n", INIT_MEM_SIZE);
    printf("  扩展内存块数: %d\n", EXTEND_MEM_SIZE);
    printf("\n");
    
    // 创建多个发送线程模拟并发连接
    #define NUM_THREADS 5
    pthread_t threads[NUM_THREADS];
    
    for (int i = 0; i < NUM_THREADS; i++) {
        int *thread_id = malloc(sizeof(int));
        *thread_id = i;
        if (pthread_create(&threads[i], NULL, simulate_sender_thread, thread_id) != 0) {
            fprintf(stderr, "创建线程 %d 失败\n", i);
            free(thread_id);
            destroy_cache_manager(g_cache_mgr);
            return 1;
        }
    }
    
    // 定期打印状态
    for (int i = 0; i < 20; i++) {
        sleep(1);
        if (i % 5 == 0) {  // 每5秒打印一次状态
            print_all_connections_status();
        }
    }
    
    // 等待所有线程完成
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }
    
    // 最后刷新所有缓冲区
    printf("\n强制刷新所有缓冲区...\n");
    pthread_mutex_lock(&g_cache_mgr->global_lock);
    for (size_t i = 0; i < g_cache_mgr->hash_table_size; i++) {
        struct hash_entry *entry = g_cache_mgr->hash_table[i];
        while (entry) {
            struct connection_cache *cache = (struct connection_cache*)shmat(entry->shm_id, NULL, 0);
            if (cache != (void*)-1) {
                flush_buffer_to_memory(cache);
                shmdt(cache);
            }
            entry = entry->next;
        }
    }
    pthread_mutex_unlock(&g_cache_mgr->global_lock);
    
    // 打印最终状态
    print_all_connections_status();
    
    // 清理资源
    destroy_cache_manager(g_cache_mgr);
    printf("\n程序正常结束\n");
    return 0;
}
#include "config.h"
#include "global.h"
#include "system/system.h"
#include "utils/debug.h"
#include <unistd.h>

int g_dbg_en = 1;
int g_dbg_error_en = 1;

struct global_data g_data;

// 错误处理
static void handle_init_error(int err_code, pthread_attr_t *attr) {
    err_code = abs(err_code) % 10;

    fprintf(stderr, "\n程序启动失败(阶段: %u)，正在清理...\n", err_code);

    if (attr)
        pthread_attr_destroy(attr);
}

int main(int argc, char *argv[]) {
    int ret;
    pthread_attr_t attr;

    memset(&g_data, 0, sizeof(g_data)); // 防御性置零

    // 阶段1 优雅退出机制注册
    ret = init_graceful_shutdown();
    if (ret == ERR_INIT_SHUTDOWN) {
        handle_init_error(ret, NULL);
        return ERROR;
    }

    // 阶段2 DPDK初始化
    ret = init_dpdk(argc, argv);
    if (ret == ERR_INIT_DPDK) {
        handle_init_error(ret, NULL);
        return ERROR;
    }

    // 阶段3 网关资源初始化
    ret = init_gw_resources();
    if (ret == ERR_INIT_GW_RESOURCE) {
        handle_init_error(ret, NULL);
        return ERROR;
    }

    // 阶段4 工作线程启动
    ret = start_worker_threads(&attr);
    if (ret == ERR_INIT_THREADS) {
        handle_init_error(ret, NULL);
        return ERROR;
    }

    pthread_attr_destroy(&attr);

    // 主循环
    while (!g_data.stop) {
        sleep(5);
        // [DEBUG]
        dbg("累计收包%lu 累计丢包%lu 累计数据包%lu 累计确认包%lu\n",
            g_data.rx_count, g_data.rx_dropped, g_data.rx_pkt_count,
            g_data.rx_ack_count);
    }

    return SUCCESS;
}
/*
编译命令：
gcc test_roce_rc_self.c -o test_roce_rc_self -libverbs

运行命令：
sudo ./test_roce_rc_self
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <infiniband/verbs.h>

struct ibv_device* get_ibv_device_by_name(const char* dev_name) {
    struct ibv_device** dev_list;
    struct ibv_device* dev = NULL;
    int i;

    dev_list = ibv_get_device_list(NULL);
    if (!dev_list) {
        perror("ibv_get_device_list failed");
        return NULL;
    }

    for (i = 0; dev_list[i] != NULL; i++) {
        if (strcmp(ibv_get_device_name(dev_list[i]), dev_name) == 0) {
            dev = dev_list[i];
            break;
        }
    }

    ibv_free_device_list(dev_list);
    return dev;
}

int main() {
    struct ibv_device* dev;
    struct ibv_context* ctx;
    struct ibv_pd* pd;
    struct ibv_qp* qp;
    struct ibv_cq* cq;
    struct ibv_mr* mr;
    char* buf;
    int ret;

    printf("=== RDMA 简单回环测试 ===\n");

    // 1. 获取rxe0设备
    dev = get_ibv_device_by_name("rxe0");
    if (!dev) {
        fprintf(stderr, "未找到RDMA设备: rxe0\n");
        return 1;
    }

    // 2. 打开设备上下文
    ctx = ibv_open_device(dev);
    if (!ctx) {
        perror("ibv_open_device failed");
        return 1;
    }
    printf("成功打开RDMA设备: %s\n", ibv_get_device_name(dev));

    // 3. 查询端口属性
    struct ibv_port_attr port_attr;
    if (ibv_query_port(ctx, 1, &port_attr) != 0) {
        perror("ibv_query_port failed");
        ibv_close_device(ctx);
        return 1;
    }
    printf("端口LID: %d, 状态: %s\n", port_attr.lid, 
           (port_attr.state == IBV_PORT_ACTIVE) ? "ACTIVE" : "INACTIVE");

    // 4. 创建保护域（PD）
    pd = ibv_alloc_pd(ctx);
    if (!pd) {
        perror("ibv_alloc_pd failed");
        ibv_close_device(ctx);
        return 1;
    }

    // 5. 分配内存并注册MR
    buf = malloc(1024);
    if (!buf) {
        perror("malloc failed");
        ibv_dealloc_pd(pd);
        ibv_close_device(ctx);
        return 1;
    }
    strcpy(buf, "Simple RDMA Loopback Test");

    mr = ibv_reg_mr(pd, buf, 1024, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    if (!mr) {
        perror("ibv_reg_mr failed");
        free(buf);
        ibv_dealloc_pd(pd);
        ibv_close_device(ctx);
        return 1;
    }

    // 6. 创建完成队列（CQ）
    cq = ibv_create_cq(ctx, 10, NULL, NULL, 0);
    if (!cq) {
        perror("ibv_create_cq failed");
        ibv_dereg_mr(mr);
        free(buf);
        ibv_dealloc_pd(pd);
        ibv_close_device(ctx);
        return 1;
    }

    // 7. 创建队列对（QP）- 使用RC模式
    struct ibv_qp_init_attr qp_attr = {
        .send_cq = cq,
        .recv_cq = cq,
        .cap = {
            .max_send_wr = 10, 
            .max_recv_wr = 10, 
            .max_send_sge = 1, 
            .max_recv_sge = 1
        },
        .qp_type = IBV_QPT_RC
    };
    
    qp = ibv_create_qp(pd, &qp_attr);
    if (!qp) {
        perror("ibv_create_qp failed");
        ibv_destroy_cq(cq);
        ibv_dereg_mr(mr);
        free(buf);
        ibv_dealloc_pd(pd);
        ibv_close_device(ctx);
        return 1;
    }
    printf("创建QP成功，QP号: %d\n", qp->qp_num);

    printf("基本RDMA资源初始化成功！\n");
    printf("可以在此基础上测试您的缓存模块\n");

    // 在这里添加您的缓存测试代码
    printf("\n=== 缓存模块测试示例 ===\n");
    printf("模拟缓存操作:\n");
    printf("1. 将数据存入缓存\n");
    printf("2. 从缓存读取数据\n");
    printf("3. 验证缓存一致性\n");
    
    // 示例：模拟缓存操作
    // cache_put(global_cache, "rdma_data", buf, strlen(buf) + 1);
    // char* cached = cache_get(global_cache, "rdma_data");
    // if (cached && strcmp(cached, buf) == 0) {
    //     printf("缓存测试成功！\n");
    // } else {
    //     printf("缓存测试失败！\n");
    // }

    // 清理资源
    ibv_destroy_qp(qp);
    ibv_destroy_cq(cq);
    ibv_dereg_mr(mr);
    free(buf);
    ibv_dealloc_pd(pd);
    ibv_close_device(ctx);
    
    printf("\n测试完成！\n");
    return 0;
}
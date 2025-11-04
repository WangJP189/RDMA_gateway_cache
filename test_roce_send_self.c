#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <infiniband/verbs.h>
#include <arpa/inet.h>

// 辅助函数：通过设备名获取ibv_device
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
    struct ibv_sge sge;
    struct ibv_send_wr wr, *bad_wr;
    struct ibv_mr* mr;
    char* buf;
    int ret;

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

    // 3. 创建保护域（PD）
    pd = ibv_alloc_pd(ctx);
    if (!pd) {
        perror("ibv_alloc_pd failed");
        ibv_close_device(ctx);
        return 1;
    }

    // 4. 分配内存并注册MR
    buf = malloc(1024);
    if (!buf) {
        perror("malloc failed");
        ibv_dealloc_pd(pd);
        ibv_close_device(ctx);
        return 1;
    }
    strcpy(buf, "RDMA Self-test: No GRH, Local Loopback");

    mr = ibv_reg_mr(pd, buf, 1024, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    if (!mr) {
        perror("ibv_reg_mr failed");
        free(buf);
        ibv_dealloc_pd(pd);
        ibv_close_device(ctx);
        return 1;
    }

    // 5. 创建完成队列（CQ）
    cq = ibv_create_cq(ctx, 10, NULL, NULL, 0);
    if (!cq) {
        perror("ibv_create_cq failed");
        ibv_dereg_mr(mr);
        free(buf);
        ibv_dealloc_pd(pd);
        ibv_close_device(ctx);
        return 1;
    }

    // 6. 创建队列对（QP）
    struct ibv_qp_init_attr qp_attr = {
        .send_cq = cq,
        .recv_cq = cq,
        .cap = {.max_send_wr = 10, .max_recv_wr = 10, .max_send_sge = 1, .max_recv_sge = 1},
        .qp_type = IBV_QPT_RC  // 可靠连接
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

    // 7. QP初始化：INIT状态（关键：显式设置access_flags）
    struct ibv_qp_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_INIT;
    attr.pkey_index = 0;        // PKey索引（默认0，与创建rxe0时的0xffff匹配）
    attr.port_num = 1;          // rxe0端口号
    attr.qp_access_flags = 0;   // 必须显式设置
    ret = ibv_modify_qp(qp, &attr, 
        IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS);
    if (ret) {
        perror("ibv_modify_qp (INIT) failed");
        goto cleanup;
    }

    // 8. QP初始化：RTR状态（跳过GRH，is_global=0）
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTR;
    attr.path_mtu = IBV_MTU_1024;  // 与rxe0的MTU匹配（默认1024）
    attr.dest_qp_num = qp->qp_num; // 自环：目标QP=本地QP
    attr.rq_psn = 0;               // 接收PSN初始值
    attr.max_dest_rd_atomic = 1;
    attr.min_rnr_timer = 12;
    // 关键修改：关闭全局路由（无需GRH和GID）
    attr.ah_attr.is_global = 1;    // 0=本地路由，1=全局路由（需GID）
    attr.ah_attr.port_num = 1;     // 本地端口号
    attr.ah_attr.dlid = 0;         // 本地LID（自环测试固定为1）

    ret = ibv_modify_qp(qp, &attr, 
        IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | 
        IBV_QP_DEST_QPN | IBV_QP_RQ_PSN | 
        IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER);
    if (ret) {
        perror("ibv_modify_qp (RTR) failed");
        goto cleanup;
    }

    // 9. QP初始化：RTS状态（准备发送）
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTS;
    attr.timeout = 0x10;
    attr.retry_cnt = 7;
    attr.rnr_retry = 7;
    attr.sq_psn = 12345;  // 发送PSN初始值
    ret = ibv_modify_qp(qp, &attr, 
        IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | 
        IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN);
    if (ret) {
        perror("ibv_modify_qp (RTS) failed");
        goto cleanup;
    }

    // 10. 发送数据
    sge.addr = (uintptr_t)buf;
    sge.length = strlen(buf) + 1;
    sge.lkey = mr->lkey;

    wr.wr_id = 1;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.opcode = IBV_WR_SEND;
    wr.send_flags = IBV_SEND_SIGNALED;  // 发送完成触发CQ事件

    ret = ibv_post_send(qp, &wr, &bad_wr);
    if (ret) {
        perror("ibv_post_send failed");
        goto cleanup;
    }
    printf("发送请求已提交，等待完成...\n");

    // 11. 轮询CQ确认结果
    struct ibv_wc wc;
    int poll_ret;
    do {
        poll_ret = ibv_poll_cq(cq, 1, &wc);
    } while (poll_ret == 0);

    if (poll_ret < 0) {
        perror("ibv_poll_cq failed");
    } else if (wc.status == IBV_WC_SUCCESS) {
        printf("RDMA自环测试成功！发送数据: %s\n", buf);
    } else {
        printf("发送失败，状态: %s\n", ibv_wc_status_str(wc.status));
    }

cleanup:
    // 清理资源
    ibv_destroy_qp(qp);
    ibv_destroy_cq(cq);
    ibv_dereg_mr(mr);
    free(buf);
    ibv_dealloc_pd(pd);
    ibv_close_device(ctx);
    return ret;
}
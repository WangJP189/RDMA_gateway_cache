#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <infiniband/verbs.h>

#define MAX_SGE 1
#define BUF_SIZE 1024

// 打开设备函数
struct ibv_context* open_ibv_device_by_name(const char *dev_name) {
    struct ibv_device **dev_list = ibv_get_device_list(NULL);
    struct ibv_context *ctx = NULL;
    for (int i = 0; dev_list && dev_list[i]; i++) {
        if (!strcmp(ibv_get_device_name(dev_list[i]), dev_name)) {
            ctx = ibv_open_device(dev_list[i]);
            break;
        }
    }
    ibv_free_device_list(dev_list);
    return ctx;
}

int main() {
    // 1. 打开 rxe_eth3
    struct ibv_context *ctx = open_ibv_device_by_name("rxe_eth3");
    if (!ctx) { perror("打开 rxe_eth3 失败"); return 1; }
    printf("✅ 接收端：成功打开 rxe_eth3\n");

    // 2. 创建 PD
    struct ibv_pd *pd = ibv_alloc_pd(ctx);
    if (!pd) { perror("创建 PD 失败"); ibv_close_device(ctx); return 1; }

    // 3. 分配接收缓冲区（仅本地写权限，避免校验）
    char *recv_buf = malloc(BUF_SIZE);
    memset(recv_buf, 0, BUF_SIZE);
    struct ibv_mr *mr = ibv_reg_mr(pd, recv_buf, BUF_SIZE, IBV_ACCESS_LOCAL_WRITE);
    if (!mr) { perror("注册 MR 失败"); free(recv_buf); ibv_dealloc_pd(pd); ibv_close_device(ctx); return 1; }
    printf("✅ 接收端：MR 地址=0x%lx，QP号=0x%x，rkey=0x%x\n", 
           (unsigned long)recv_buf, 0, mr->rkey);  // 后续用发送端手动指定 QP 号

    // 4. 创建 CQ
    struct ibv_cq *cq = ibv_create_cq(ctx, 10, NULL, NULL, 0);
    if (!cq) { perror("创建 CQ 失败"); ibv_dereg_mr(mr); free(recv_buf); ibv_dealloc_pd(pd); ibv_close_device(ctx); return 1; }

    // 5. 创建 RC 类型 QP（极简配置）
    struct ibv_qp_init_attr qp_init_attr = {
        .send_cq = cq, .recv_cq = cq,
        .cap = { .max_send_wr = 1, .max_recv_wr = 1, .max_send_sge = MAX_SGE, .max_recv_sge = MAX_SGE },
        .qp_type = IBV_QPT_RC
    };
    struct ibv_qp *qp = ibv_create_qp(pd, &qp_init_attr);
    if (!qp) { perror("创建 QP 失败"); ibv_destroy_cq(cq); ibv_dereg_mr(mr); free(recv_buf); ibv_dealloc_pd(pd); ibv_close_device(ctx); return 1; }
    printf("✅ 接收端：等待数据...\n");

    // 6. QP 状态迁移（仅 RESET→INIT，跳过 RTR，发送端触发）
    struct ibv_qp_attr qp_attr;
    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.qp_state = IBV_QPS_INIT;
    qp_attr.port_num = 1;
    int qp_flags = IBV_QP_STATE | IBV_QP_PORT;
    if (ibv_modify_qp(qp, &qp_attr, qp_flags)) { perror("QP 迁移 INIT 失败"); ibv_destroy_qp(qp); ibv_destroy_cq(cq); ibv_dereg_mr(mr); free(recv_buf); ibv_dealloc_pd(pd); ibv_close_device(ctx); return 1; }

    // 7. 投递接收请求
    struct ibv_recv_wr recv_wr, *bad_wr;
    struct ibv_sge sge = { .addr = (unsigned long)recv_buf, .length = BUF_SIZE, .lkey = mr->lkey };
    recv_wr.wr_id = 1;
    recv_wr.sg_list = &sge;
    recv_wr.num_sge = MAX_SGE;
    if (ibv_post_recv(qp, &recv_wr, &bad_wr)) { perror("投递接收请求失败"); ibv_destroy_qp(qp); ibv_destroy_cq(cq); ibv_dereg_mr(mr); free(recv_buf); ibv_dealloc_pd(pd); ibv_close_device(ctx); return 1; }

    // 8. 等待接收数据
    struct ibv_wc wc;
    while (1) {
        int ret = ibv_poll_cq(cq, 1, &wc);
        if (ret < 0) { perror("轮询 CQ 失败"); break; }
        if (ret == 1 && wc.status == IBV_WC_SUCCESS) {
            printf("\n🎉 接收成功！\n");
            printf("数据长度：%d 字节\n", wc.byte_len);
            printf("数据内容：%s\n", recv_buf);
            break;
        }
    }

    // 释放资源
    ibv_destroy_qp(qp);
    ibv_destroy_cq(cq);
    ibv_dereg_mr(mr);
    free(recv_buf);
    ibv_dealloc_pd(pd);
    ibv_close_device(ctx);
    return 0;
}
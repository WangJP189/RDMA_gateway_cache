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
    // ！！！手动填写接收端输出的 3 个参数！！！
    const uint32_t DEST_QP_NUM = 0x1;        // 接收端 QP 号（运行接收端后查看）
    const uint64_t REMOTE_ADDR = 0x56209500f130; // 接收端 MR 地址
    const uint32_t REMOTE_RKEY = 0x4a4;       // 接收端 rkey

    // 1. 打开 rxe_eth1
    struct ibv_context *ctx = open_ibv_device_by_name("rxe_eth1");
    if (!ctx) { perror("打开 rxe_eth1 失败"); return 1; }
    printf("✅ 发送端：成功打开 rxe_eth1\n");

    // 2. 创建 PD
    struct ibv_pd *pd = ibv_alloc_pd(ctx);
    if (!pd) { perror("创建 PD 失败"); ibv_close_device(ctx); return 1; }

    // 3. 分配发送缓冲区
    char *send_buf = malloc(BUF_SIZE);
    strcpy(send_buf, "rxe_eth1 → rxe_eth3：RDMA 通信成功！");
    struct ibv_mr *mr = ibv_reg_mr(pd, send_buf, BUF_SIZE, IBV_ACCESS_LOCAL_WRITE);
    if (!mr) { perror("注册 MR 失败"); free(send_buf); ibv_dealloc_pd(pd); ibv_close_device(ctx); return 1; }

    // 4. 创建 CQ
    struct ibv_cq *cq = ibv_create_cq(ctx, 10, NULL, NULL, 0);
    if (!cq) { perror("创建 CQ 失败"); ibv_dereg_mr(mr); free(send_buf); ibv_dealloc_pd(pd); ibv_close_device(ctx); return 1; }

    // 5. 创建 RC 类型 QP
    struct ibv_qp_init_attr qp_init_attr = {
        .send_cq = cq, .recv_cq = cq,
        .cap = { .max_send_wr = 1, .max_recv_wr = 1, .max_send_sge = MAX_SGE, .max_recv_sge = MAX_SGE },
        .qp_type = IBV_QPT_RC
    };
    struct ibv_qp *qp = ibv_create_qp(pd, &qp_init_attr);
    if (!qp) { perror("创建 QP 失败"); ibv_destroy_cq(cq); ibv_dereg_mr(mr); free(send_buf); ibv_dealloc_pd(pd); ibv_close_device(ctx); return 1; }

    // 6. QP 状态迁移（RESET→INIT→RTR→RTS，极简参数）
    struct ibv_qp_attr qp_attr;
    int qp_flags;

    // RESET→INIT
    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.qp_state = IBV_QPS_INIT;
    qp_attr.port_num = 1;
    qp_flags = IBV_QP_STATE | IBV_QP_PORT;
    if (ibv_modify_qp(qp, &qp_attr, qp_flags)) { perror("迁移 INIT 失败"); ibv_destroy_qp(qp); ibv_destroy_cq(cq); ibv_dereg_mr(mr); free(send_buf); ibv_dealloc_pd(pd); ibv_close_device(ctx); return 1; }

    // INIT→RTR（仅 4 个核心参数）
    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.qp_state = IBV_QPS_RTR;
    qp_attr.dest_qp_num = DEST_QP_NUM;
    qp_attr.path_mtu = IBV_MTU_1024;
    qp_attr.rq_psn = 0;
    qp_flags = IBV_QP_STATE | IBV_QP_DEST_QPN | IBV_QP_PATH_MTU | IBV_QP_RQ_PSN;
    if (ibv_modify_qp(qp, &qp_attr, qp_flags)) { perror("迁移 RTR 失败"); ibv_destroy_qp(qp); ibv_destroy_cq(cq); ibv_dereg_mr(mr); free(send_buf); ibv_dealloc_pd(pd); ibv_close_device(ctx); return 1; }

    // RTR→RTS
    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.qp_state = IBV_QPS_RTS;
    qp_attr.sq_psn = 0;
    qp_flags = IBV_QP_STATE | IBV_QP_SQ_PSN;
    if (ibv_modify_qp(qp, &qp_attr, qp_flags)) { perror("迁移 RTS 失败"); ibv_destroy_qp(qp); ibv_destroy_cq(cq); ibv_dereg_mr(mr); free(send_buf); ibv_dealloc_pd(pd); ibv_close_device(ctx); return 1; }

    // 7. 投递发送请求
    struct ibv_send_wr send_wr, *bad_wr = NULL;
    struct ibv_sge sge = { .addr = (unsigned long)send_buf, .length = strlen(send_buf) + 1, .lkey = mr->lkey };
    send_wr.wr_id = 1;
    send_wr.sg_list = &sge;
    send_wr.num_sge = MAX_SGE;
    send_wr.opcode = IBV_WR_RDMA_WRITE;
    send_wr.send_flags = IBV_SEND_SIGNALED;
    send_wr.wr.rdma.remote_addr = REMOTE_ADDR;
    send_wr.wr.rdma.rkey = REMOTE_RKEY;

    if (ibv_post_send(qp, &send_wr, &bad_wr)) { perror("投递发送请求失败"); ibv_destroy_qp(qp); ibv_destroy_cq(cq); ibv_dereg_mr(mr); free(send_buf); ibv_dealloc_pd(pd); ibv_close_device(ctx); return 1; }
    printf("📤 发送端：已发送数据，等待接收端响应...\n");

    // 8. 等待发送完成
    struct ibv_wc wc;
    while (1) {
        int ret = ibv_poll_cq(cq, 1, &wc);
        if (ret < 0) { perror("轮询 CQ 失败"); break; }
        if (ret == 1 && wc.status == IBV_WC_SUCCESS) {
            printf("🎉 发送端：数据发送完成！\n");
            break;
        }
    }

    // 释放资源
    ibv_destroy_qp(qp);
    ibv_destroy_cq(cq);
    ibv_dereg_mr(mr);
    free(send_buf);
    ibv_dealloc_pd(pd);
    ibv_close_device(ctx);
    return 0;
}
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <infiniband/verbs.h>  // 正确的头文件路径

#define MAX_SGE 1
#define BUF_SIZE 1024

// 新增：通过设备名称查找并打开 RDMA 设备（替代 ibv_open_device_by_name）
struct ibv_context* open_ibv_device_by_name(const char *dev_name) {
    struct ibv_device **dev_list;
    struct ibv_context *ctx = NULL;

    // 获取所有 RDMA 设备列表
    dev_list = ibv_get_device_list(NULL);
    if (!dev_list) { perror("ibv_get_device_list failed"); return NULL; }

    // 遍历列表，匹配设备名称
    for (int i = 0; dev_list[i]; i++) {
        if (strcmp(ibv_get_device_name(dev_list[i]), dev_name) == 0) {
            // 打开匹配的设备
            ctx = ibv_open_device(dev_list[i]);
            if (!ctx) { perror("ibv_open_device failed"); }
            break;
        }
    }

    // 释放设备列表（必须调用，避免内存泄漏）
    ibv_free_device_list(dev_list);
    return ctx;
}

int main() {
    // 1. 打开 RDMA 设备（rxe_eth3）：调用新增的函数
    struct ibv_context *ctx = open_ibv_device_by_name("rxe_eth3");
    if (!ctx) { fprintf(stderr, "打开设备 rxe_eth3 失败\n"); return 1; }
    printf("✅ 成功打开 RDMA 设备：rxe_eth3\n");

    // 2. 创建保护域（PD）
    struct ibv_pd *pd = ibv_alloc_pd(ctx);
    if (!pd) { perror("ibv_alloc_pd failed"); ibv_close_device(ctx); return 1; }
    printf("✅ 成功创建保护域（PD）\n");

    // 3. 分配内存缓冲区（用于接收数据）
    char *recv_buf = malloc(BUF_SIZE);
    memset(recv_buf, 0, BUF_SIZE);
    struct ibv_mr *mr = ibv_reg_mr(pd, recv_buf, BUF_SIZE, 
                                   IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE);
    if (!mr) { perror("ibv_reg_mr failed"); free(recv_buf); ibv_dealloc_pd(pd); ibv_close_device(ctx); return 1; }
    printf("✅ 成功注册内存区域（MR）：地址=0x%lx，长度=%d，rkey=0x%x\n", 
           (unsigned long)recv_buf, BUF_SIZE, mr->rkey);  // 新增：打印 rkey，无需额外查询

    // 4. 创建完成队列（CQ）
    struct ibv_cq *cq = ibv_create_cq(ctx, 10, NULL, NULL, 0);
    if (!cq) { perror("ibv_create_cq failed"); ibv_dereg_mr(mr); free(recv_buf); ibv_dealloc_pd(pd); ibv_close_device(ctx); return 1; }
    printf("✅ 成功创建完成队列（CQ）\n");

    // 5. 创建队列对（QP，类型：RC）
    struct ibv_qp_init_attr qp_init_attr = {
        .send_cq = cq, .recv_cq = cq,
        .cap = { .max_send_wr = 1, .max_recv_wr = 1, .max_send_sge = MAX_SGE, .max_recv_sge = MAX_SGE },
        .qp_type = IBV_QPT_RC
    };
    struct ibv_qp *qp = ibv_create_qp(pd, &qp_init_attr);
    if (!qp) { perror("ibv_create_qp failed"); ibv_destroy_cq(cq); ibv_dereg_mr(mr); free(recv_buf); ibv_dealloc_pd(pd); ibv_close_device(ctx); return 1; }
    printf("✅ 成功创建队列对（QP）：QP号=0x%x\n", qp->qp_num);

    // 6. QP 状态迁移（RESET → INIT → RTR）
    struct ibv_qp_attr qp_attr;
    int qp_flags;

    // RESET → INIT（核心修改：添加 qp_access_flags 参数）
    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.qp_state = IBV_QPS_INIT;       // 目标状态：INIT
    qp_attr.pkey_index = 0;                // PKey 索引（默认 0）
    qp_attr.port_num = 1;                  // 端口号（固定 1）
    qp_attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ;
    qp_flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;  // 新增：添加 IBV_QP_ACCESS_FLAGS 标志
    if (ibv_modify_qp(qp, &qp_attr, qp_flags)) { 
        perror("ibv_modify_qp (INIT) failed"); 
        ibv_destroy_qp(qp); 
        ibv_destroy_cq(cq); 
        ibv_dereg_mr(mr); 
        free(recv_buf); 
        ibv_dealloc_pd(pd); 
        ibv_close_device(ctx); 
        return 1; 
    }

    // 7. 投递接收请求（WR）
    struct ibv_recv_wr recv_wr, *bad_wr;
    struct ibv_sge sge;
    memset(&recv_wr, 0, sizeof(recv_wr));
    memset(&sge, 0, sizeof(sge));

    sge.addr = (unsigned long)recv_buf;
    sge.length = BUF_SIZE;
    sge.lkey = mr->lkey;

    recv_wr.wr_id = 1;
    recv_wr.sg_list = &sge;
    recv_wr.num_sge = MAX_SGE;

    if (ibv_post_recv(qp, &recv_wr, &bad_wr)) { perror("ibv_post_recv failed"); ibv_destroy_qp(qp); ibv_destroy_cq(cq); ibv_dereg_mr(mr); free(recv_buf); ibv_dealloc_pd(pd); ibv_close_device(ctx); return 1; }
    printf("📥 等待接收数据...（QP号=0x%x，缓冲区地址=0x%lx，rkey=0x%x）\n", 
           qp->qp_num, (unsigned long)recv_buf, mr->rkey);

    // 8. 等待完成事件并获取数据
    struct ibv_wc wc;
    while (1) {
        int ret = ibv_poll_cq(cq, 1, &wc);
        if (ret < 0) { perror("ibv_poll_cq failed"); break; }
        if (ret == 1 && wc.status == IBV_WC_SUCCESS) {
            printf("\n🎉 成功接收 RDMA 数据！\n");
            printf("接收长度：%d 字节\n", wc.byte_len);
            printf("数据内容：%s\n", recv_buf);
            break;
        }
    }

    // 9. 资源释放
    ibv_destroy_qp(qp);
    ibv_destroy_cq(cq);
    ibv_dereg_mr(mr);
    free(recv_buf);
    ibv_dealloc_pd(pd);
    ibv_close_device(ctx);
    return 0;
}
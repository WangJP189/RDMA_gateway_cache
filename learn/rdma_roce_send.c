#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <infiniband/verbs.h>

#define MAX_SGE 1
#define BUF_SIZE 1024

// 通过设备名称查找并打开 RDMA 设备（与接收端完全一致）
struct ibv_context* open_ibv_device_by_name(const char *dev_name) {
    struct ibv_device **dev_list;
    struct ibv_context *ctx = NULL;

    dev_list = ibv_get_device_list(NULL);
    if (!dev_list) { perror("ibv_get_device_list failed"); return NULL; }

    for (int i = 0; dev_list[i]; i++) {
        if (strcmp(ibv_get_device_name(dev_list[i]), dev_name) == 0) {
            ctx = ibv_open_device(dev_list[i]);
            if (!ctx) { perror("ibv_open_device failed"); }
            break;
        }
    }

    ibv_free_device_list(dev_list);
    return ctx;
}

int main() {
    // ！！！替换为你接收端输出的实际参数！！！
    const uint32_t DEST_QP_NUM = 0x17;        // 接收端 QP 号（例如接收端输出的 0x13）
    const uint64_t REMOTE_ADDR = 0x5e1fd2c09130; // 接收端缓冲区地址（例如接收端输出的 0x56209500f130）
    const uint32_t REMOTE_RKEY = 0x8f7;       // 接收端 rkey（例如接收端输出的 0x4a4）

    // 1. 打开 RDMA 设备（rxe_eth1）
    struct ibv_context *ctx = open_ibv_device_by_name("rxe_eth1");
    if (!ctx) { fprintf(stderr, "打开设备 rxe_eth1 失败\n"); return 1; }
    printf("✅ 成功打开 RDMA 设备：rxe_eth1\n");

    // 2. 创建保护域（PD）
    struct ibv_pd *pd = ibv_alloc_pd(ctx);
    if (!pd) { perror("ibv_alloc_pd failed"); ibv_close_device(ctx); return 1; }
    printf("✅ 成功创建保护域（PD）\n");

    // 3. 分配发送缓冲区并注册 MR（权限与接收端匹配）
    char *send_buf = malloc(BUF_SIZE);
    strcpy(send_buf, "rxe_eth1 → rxe_eth3：RoCEv2 传输成功！");
    // 权限与接收端一致：本地写 + 远程读（不包含远程写，避免冲突）
    struct ibv_mr *mr = ibv_reg_mr(pd, send_buf, BUF_SIZE, 
                                   IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ);
    if (!mr) { perror("ibv_reg_mr failed"); free(send_buf); ibv_dealloc_pd(pd); ibv_close_device(ctx); return 1; }
    printf("✅ 成功注册内存区域（MR）：地址=0x%lx，长度=%d\n", 
           (unsigned long)send_buf, BUF_SIZE);
    printf("发送数据：%s\n", send_buf);

    // 4. 创建完成队列（CQ）
    struct ibv_cq *cq = ibv_create_cq(ctx, 10, NULL, NULL, 0);
    if (!cq) { perror("ibv_create_cq failed"); ibv_dereg_mr(mr); free(send_buf); ibv_dealloc_pd(pd); ibv_close_device(ctx); return 1; }
    printf("✅ 成功创建完成队列（CQ）\n");

    // 5. 创建队列对（QP，RC 类型，与接收端配置一致）
    struct ibv_qp_init_attr qp_init_attr = {
        .send_cq = cq, .recv_cq = cq,
        .cap = { .max_send_wr = 1, .max_recv_wr = 1, .max_send_sge = MAX_SGE, .max_recv_sge = MAX_SGE },
        .qp_type = IBV_QPT_RC  // 与接收端一致的 RC 类型
    };
    struct ibv_qp *qp = ibv_create_qp(pd, &qp_init_attr);
    if (!qp) { perror("ibv_create_qp failed"); ibv_destroy_cq(cq); ibv_dereg_mr(mr); free(send_buf); ibv_dealloc_pd(pd); ibv_close_device(ctx); return 1; }
    printf("✅ 成功创建队列对（QP）：QP号=0x%x\n", qp->qp_num);

    // 6. QP 状态迁移（与接收端参数严格匹配，关键修复）
    struct ibv_qp_attr qp_attr;
    int qp_flags;

    // RESET → INIT（参数与接收端 INIT 状态完全对齐）
    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.qp_state = IBV_QPS_INIT;
    qp_attr.pkey_index = 0;                // 与接收端一致的 PKey 索引
    qp_attr.port_num = 1;                  // 与接收端一致的端口号
    qp_attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ;  // 与接收端权限完全相同
    qp_flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;  // 标志位与接收端一致
    if (ibv_modify_qp(qp, &qp_attr, qp_flags)) { 
        perror("ibv_modify_qp (INIT) failed"); 
        ibv_destroy_qp(qp); 
        ibv_destroy_cq(cq); 
        ibv_dereg_mr(mr); 
        free(send_buf); 
        ibv_dealloc_pd(pd); 
        ibv_close_device(ctx); 
        return 1; 
    }

// INIT → RTR（终极修复：补充 ah_attr.port_num=1）
    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.qp_state = IBV_QPS_RTR;       // 目标状态：RTR
    qp_attr.pkey_index = 0;                // PKey 索引（默认 0）
    qp_attr.port_num = 1;                  // 端口号（固定 1）
    qp_attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ;
    qp_attr.path_mtu = IBV_MTU_1024;       // 与接收端一致的 MTU
    qp_attr.dest_qp_num = DEST_QP_NUM;     // 目标 QP 号（接收端 QP 号）
    qp_attr.rq_psn = 0;                    // 接收端起始序
    qp_flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS | IBV_QP_PATH_MTU | ;  // 新增：添加 IBV_QP_ACCESS_FLAGS 标志

if (ibv_modify_qp(qp, &qp_attr, qp_flags)) { 
    perror("ibv_modify_qp (RTR) failed"); 
    ibv_destroy_qp(qp); 
    ibv_destroy_cq(cq); 
    ibv_dereg_mr(mr); 
    free(send_buf); 
    ibv_dealloc_pd(pd); 
    ibv_close_device(ctx); 
    return 1; 
}

    // RTR → RTS（最终状态迁移）
    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.qp_state = IBV_QPS_RTS;
    qp_attr.sq_psn = 0;                     // 发送端起始序列号
    qp_attr.max_rd_atomic = 1;              // 与接收端匹配
    qp_flags = IBV_QP_STATE | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;
    if (ibv_modify_qp(qp, &qp_attr, qp_flags)) { 
        perror("ibv_modify_qp (RTS) failed"); 
        ibv_destroy_qp(qp); 
        ibv_destroy_cq(cq); 
        ibv_dereg_mr(mr); 
        free(send_buf); 
        ibv_dealloc_pd(pd); 
        ibv_close_device(ctx); 
        return 1; 
    }
    printf("✅ QP 状态迁移完成：RESET → INIT → RTR → RTS\n");

    // 7. 投递 RDMA 发送请求（使用接收端的参数）
    struct ibv_send_wr send_wr, *bad_wr = NULL;
    struct ibv_sge sge;
    memset(&send_wr, 0, sizeof(send_wr));
    memset(&sge, 0, sizeof(sge));

    sge.addr = (unsigned long)send_buf;
    sge.length = strlen(send_buf) + 1;  // 包含字符串结束符
    sge.lkey = mr->lkey;                // 本地内存密钥

    send_wr.wr_id = 1;
    send_wr.sg_list = &sge;
    send_wr.num_sge = MAX_SGE;
    send_wr.opcode = IBV_WR_RDMA_WRITE;  // RDMA 写操作（与接收端权限兼容）
    send_wr.send_flags = IBV_SEND_SIGNALED;  // 发送完成后产生信号

    // 绑定接收端的内存信息（必须与接收端输出一致）
    send_wr.wr.rdma.remote_addr = REMOTE_ADDR;  // 接收端缓冲区地址
    send_wr.wr.rdma.rkey = REMOTE_RKEY;        // 接收端 rkey

    if (ibv_post_send(qp, &send_wr, &bad_wr)) { 
        perror("ibv_post_send failed"); 
        ibv_destroy_qp(qp); 
        ibv_destroy_cq(cq); 
        ibv_dereg_mr(mr); 
        free(send_buf); 
        ibv_dealloc_pd(pd); 
        ibv_close_device(ctx); 
        return 1; 
    }
    printf("📤 已投递 RDMA 发送请求，等待完成...\n");

    // 8. 等待发送完成
    struct ibv_wc wc;
    while (1) {
        int ret = ibv_poll_cq(cq, 1, &wc);
        if (ret < 0) { perror("ibv_poll_cq failed"); break; }
        if (ret == 1 && wc.status == IBV_WC_SUCCESS) {
            printf("🎉 RDMA 发送完成！\n");
            printf("发送长度：%d 字节\n", wc.byte_len);
            break;
        }
    }

    // 9. 释放资源
    ibv_destroy_qp(qp);
    ibv_destroy_cq(cq);
    ibv_dereg_mr(mr);
    free(send_buf);
    ibv_dealloc_pd(pd);
    ibv_close_device(ctx);
    return 0;
}
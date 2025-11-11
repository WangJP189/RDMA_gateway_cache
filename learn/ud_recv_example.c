#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <infiniband/verbs.h>

#define MAX_BUFFER_SIZE 1024
#define QP_NUM 1
#define CQ_SIZE 1024

// RDMA资源结构体
struct rdma_res {
    struct ibv_context *ctx;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    struct ibv_mr *mr;
    char *recv_buf;
    uint32_t qp_num;
    union ibv_gid gid;
    uint16_t lid;
};

// 初始化RDMA资源
int rdma_init(struct rdma_res *res, const char *dev_name) {
    memset(res, 0, sizeof(*res));
    struct ibv_device **dev_list;
    struct ibv_device *dev = NULL;

    // 获取设备列表
    dev_list = ibv_get_device_list(NULL);
    if (!dev_list) {
        perror("ibv_get_device_list failed");
        return -1;
    }

    // 查找指定设备
    for (int i = 0; dev_list[i]; i++) {
        if (strcmp(ibv_get_device_name(dev_list[i]), dev_name) == 0) {
            dev = dev_list[i];
            break;
        }
    }
    if (!dev) {
        fprintf(stderr, "Device %s not found\n", dev_name);
        ibv_free_device_list(dev_list);
        return -1;
    }

    // 打开设备上下文
    res->ctx = ibv_open_device(dev);
    if (!res->ctx) {
        perror("ibv_open_device failed");
        ibv_free_device_list(dev_list);
        return -1;
    }
    ibv_free_device_list(dev_list);

    // 分配保护域
    res->pd = ibv_alloc_pd(res->ctx);
    if (!res->pd) {
        perror("ibv_alloc_pd failed");
        goto err;
    }

    // 创建完成队列
    res->cq = ibv_create_cq(res->ctx, CQ_SIZE, NULL, NULL, 0);
    if (!res->cq) {
        perror("ibv_create_cq failed");
        goto err;
    }

    // 创建接收缓冲区
    res->recv_buf = malloc(MAX_BUFFER_SIZE);
    if (!res->recv_buf) {
        perror("malloc recv_buf failed");
        goto err;
    }
    memset(res->recv_buf, 0, MAX_BUFFER_SIZE);

    // 注册内存区域
    res->mr = ibv_reg_mr(res->pd, res->recv_buf, MAX_BUFFER_SIZE,
                        IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    if (!res->mr) {
        perror("ibv_reg_mr failed");
        goto err;
    }

    // 创建QP（UD类型）
    struct ibv_qp_init_attr qp_init_attr = {
        .send_cq = res->cq,
        .recv_cq = res->cq,
        .qp_type = IBV_QPT_UD,
        .cap = {
            .max_send_wr = 10,
            .max_recv_wr = 10,
            .max_send_sge = 1,
            .max_recv_sge = 1
        }
    };
    res->qp = ibv_create_qp(res->pd, &qp_init_attr);
    if (!res->qp) {
        perror("ibv_create_qp failed");
        goto err;
    }
    res->qp_num = res->qp->qp_num;

    // 获取端口属性以获取LID
    struct ibv_port_attr port_attr;
    if (ibv_query_port(res->ctx, 1, &port_attr) != 0) {
        perror("ibv_query_port failed");
        goto err;
    }
    res->lid = port_attr.lid;

    // 获取本地GID（索引0）
    if (ibv_query_gid(res->ctx, 1, 0, &res->gid) != 0) {
        perror("ibv_query_gid failed");
        goto err;
    }

    printf("接收端初始化完成:\n");
    printf("  QP号: %u\n", res->qp_num);
    printf("  LID: %u\n", res->lid);
    printf("  GID: ");
    for (int i = 0; i < 16; i++)
        printf("%02x", res->gid.raw[i]);
    printf("\n");
    return 0;

err:
    if (res->mr) ibv_dereg_mr(res->mr);
    if (res->recv_buf) free(res->recv_buf);
    if (res->cq) ibv_destroy_cq(res->cq);
    if (res->pd) ibv_dealloc_pd(res->pd);
    if (res->ctx) ibv_close_device(res->ctx);
    return -1;
}

// QP状态转换
int qp_transition(struct ibv_qp *qp, enum ibv_qp_state new_state,
                 struct ibv_qp_attr *attr, int flags) {
    if (ibv_modify_qp(qp, attr, flags) != 0) {
        perror("ibv_modify_qp failed");
        return -1;
    }
    printf("QP状态已转换为: %d\n", new_state);
    return 0;
}

// 初始化QP到可接收状态
int qp_init(struct ibv_qp *qp) {
    struct ibv_qp_attr attr = {0};

    // RESET -> INIT
    attr.qp_state = IBV_QPS_INIT;
    attr.pkey_index = 0;
    attr.port_num = 1;
    attr.qkey = 0x11111111;  // 使用非零的QKey
    if (qp_transition(qp, IBV_QPS_INIT, &attr,
                     IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_QKEY) != 0)
        return -1;

    // INIT -> RTR
    attr.qp_state = IBV_QPS_RTR;
    if (qp_transition(qp, IBV_QPS_RTR, &attr, IBV_QP_STATE) != 0)
        return -1;

    // RTR -> RTS
    attr.qp_state = IBV_QPS_RTS;
    attr.sq_psn = 0;  // 初始PSN
    if (qp_transition(qp, IBV_QPS_RTS, &attr, IBV_QP_STATE | IBV_QP_SQ_PSN) != 0)
        return -1;

    return 0;
}

// 投递接收请求
int post_recv(struct rdma_res *res) {
    struct ibv_recv_wr wr = {0};
    struct ibv_sge sge = {0};
    struct ibv_recv_wr *bad_wr;

    sge.addr = (uintptr_t)res->recv_buf;
    sge.length = MAX_BUFFER_SIZE;
    sge.lkey = res->mr->lkey;

    wr.sg_list = &sge;
    wr.num_sge = 1;

    if (ibv_post_recv(res->qp, &wr, &bad_wr) != 0) {
        perror("ibv_post_recv failed");
        return -1;
    }
    printf("接收请求已投递\n");
    return 0;
}

// 等待接收完成
int wait_recv(struct rdma_res *res) {
    struct ibv_wc wc;
    int ret;

    while (1) {
        ret = ibv_poll_cq(res->cq, 1, &wc);
        if (ret < 0) {
            perror("ibv_poll_cq failed");
            return -1;
        } else if (ret == 0) {
            usleep(1000);
            continue;
        }

        if (wc.status != IBV_WC_SUCCESS) {
            fprintf(stderr, "接收失败，状态: %s\n", ibv_wc_status_str(wc.status));
            return -1;
        }

        printf("接收成功!\n");
        printf("  长度: %d\n", wc.byte_len);
        printf("  数据: %s\n", res->recv_buf);
        return 0;
    }
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "用法: %s <RDMA设备名>\n", argv[0]);
        fprintf(stderr, "示例: %s rxe_eth3\n", argv[0]);
        return 1;
    }

    struct rdma_res res;
    if (rdma_init(&res, argv[1]) != 0)
        return 1;

    // 初始化QP状态
    if (qp_init(res.qp) != 0) {
        fprintf(stderr, "QP初始化失败\n");
        return 1;
    }

    // 投递接收请求
    if (post_recv(&res) != 0)
        return 1;

    printf("等待接收数据...\n");
    if (wait_recv(&res) != 0)
        return 1;

    // 清理资源
    ibv_dereg_mr(res.mr);
    free(res.recv_buf);
    ibv_destroy_qp(res.qp);
    ibv_destroy_cq(res.cq);
    ibv_dealloc_pd(res.pd);
    ibv_close_device(res.ctx);
    return 0;
}
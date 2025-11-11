/*
编译命令：
gcc -o rc_send_example rc_send_example.c -libverbs

运行命令：
sudo ./rc_send_example rxe_eth1 30 0
*/



#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <infiniband/verbs.h>

#define MAX_BUFFER_SIZE 1024
#define CQ_SIZE 1024

struct rdma_res {
    struct ibv_context *ctx;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    struct ibv_mr *mr;
    char *buffer;
    uint32_t qp_num;
    uint16_t lid;
};

int rdma_init(struct rdma_res *res, const char *dev_name) {
    memset(res, 0, sizeof(*res));
    struct ibv_device **dev_list = ibv_get_device_list(NULL);
    if (!dev_list) {
        perror("ibv_get_device_list failed");
        return -1;
    }

    struct ibv_device *dev = NULL;
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

    res->ctx = ibv_open_device(dev);
    ibv_free_device_list(dev_list);
    
    if (!res->ctx) {
        perror("ibv_open_device failed");
        return -1;
    }

    res->pd = ibv_alloc_pd(res->ctx);
    if (!res->pd) {
        perror("ibv_alloc_pd failed");
        goto err;
    }

    res->cq = ibv_create_cq(res->ctx, CQ_SIZE, NULL, NULL, 0);
    if (!res->cq) {
        perror("ibv_create_cq failed");
        goto err;
    }

    res->buffer = malloc(MAX_BUFFER_SIZE);
    if (!res->buffer) {
        perror("malloc buffer failed");
        goto err;
    }
    memset(res->buffer, 0, MAX_BUFFER_SIZE);

    res->mr = ibv_reg_mr(res->pd, res->buffer, MAX_BUFFER_SIZE,
                        IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    if (!res->mr) {
        perror("ibv_reg_mr failed");
        goto err;
    }

    // 创建QP（RC类型）
    struct ibv_qp_init_attr qp_init_attr = {
        .send_cq = res->cq,
        .recv_cq = res->cq,
        .qp_type = IBV_QPT_RC,
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

    // 获取LID
    struct ibv_port_attr port_attr;
    if (ibv_query_port(res->ctx, 1, &port_attr) != 0) {
        perror("ibv_query_port failed");
        goto err;
    }
    res->lid = port_attr.lid;

    printf("RC发送端初始化完成:\n");
    printf("  QP号: %u\n", res->qp_num);
    printf("  LID: %u\n", res->lid);
    printf("  端口状态: %s\n", (port_attr.state == IBV_PORT_ACTIVE) ? "ACTIVE" : "INACTIVE");
    return 0;

err:
    if (res->mr) ibv_dereg_mr(res->mr);
    if (res->buffer) free(res->buffer);
    if (res->cq) ibv_destroy_cq(res->cq);
    if (res->pd) ibv_dealloc_pd(res->pd);
    if (res->ctx) ibv_close_device(res->ctx);
    return -1;
}

int qp_transition(struct ibv_qp *qp, enum ibv_qp_state new_state,
                 struct ibv_qp_attr *attr, int flags) {
    if (ibv_modify_qp(qp, attr, flags) != 0) {
        perror("ibv_modify_qp failed");
        return -1;
    }
    printf("QP状态已转换为: %d\n", new_state);
    return 0;
}

// RC模式发送端QP初始化
int qp_init_to_rtr(struct ibv_qp *qp, uint32_t remote_qp_num, uint16_t remote_lid) {
    struct ibv_qp_attr attr = {0};

    // RESET -> INIT
    attr.qp_state = IBV_QPS_INIT;
    attr.pkey_index = 0;
    attr.port_num = 1;
    attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE;
    if (qp_transition(qp, IBV_QPS_INIT, &attr,
                     IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS) != 0)
        return -1;

    // INIT -> RTR
    attr.qp_state = IBV_QPS_RTR;
    attr.path_mtu = IBV_MTU_1024;
    attr.dest_qp_num = remote_qp_num;
    attr.rq_psn = 0;
    attr.max_dest_rd_atomic = 1;
    attr.min_rnr_timer = 12;
    attr.ah_attr.is_global = 0;
    attr.ah_attr.dlid = remote_lid;
    attr.ah_attr.sl = 0;
    attr.ah_attr.src_path_bits = 0;
    attr.ah_attr.port_num = 1;
    if (qp_transition(qp, IBV_QPS_RTR, &attr,
                     IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
                     IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER) != 0)
        return -1;

    return 0;
}

int qp_init_to_rts(struct ibv_qp *qp) {
    struct ibv_qp_attr attr = {0};

    // RTR -> RTS
    attr.qp_state = IBV_QPS_RTS;
    attr.timeout = 14;
    attr.retry_cnt = 7;
    attr.rnr_retry = 7;
    attr.sq_psn = 0;
    attr.max_rd_atomic = 1;
    if (qp_transition(qp, IBV_QPS_RTS, &attr,
                     IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
                     IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC) != 0)
        return -1;

    return 0;
}

int send_data(struct rdma_res *res, const char *data) {
    struct ibv_send_wr wr = {0};
    struct ibv_sge sge = {0};
    struct ibv_send_wr *bad_wr;

    strncpy(res->buffer, data, MAX_BUFFER_SIZE-1);
    res->buffer[MAX_BUFFER_SIZE-1] = '\0';
    
    sge.addr = (uintptr_t)res->buffer;
    sge.length = strlen(res->buffer) + 1;
    sge.lkey = res->mr->lkey;

    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.opcode = IBV_WR_SEND;
    wr.send_flags = IBV_SEND_SIGNALED;

    printf("准备发送数据: '%s', 长度: %zu\n", data, strlen(data));

    if (ibv_post_send(res->qp, &wr, &bad_wr) != 0) {
        perror("ibv_post_send failed");
        return -1;
    }

    printf("发送请求已提交，等待完成...\n");

    // 等待发送完成
    struct ibv_wc wc;
    int retry_count = 0;
    const int max_retries = 100;
    
    while (retry_count < max_retries) {
        int ret = ibv_poll_cq(res->cq, 1, &wc);
        if (ret < 0) {
            perror("ibv_poll_cq failed");
            return -1;
        } else if (ret > 0) {
            if (wc.status != IBV_WC_SUCCESS) {
                fprintf(stderr, "发送失败，状态: %s\n", ibv_wc_status_str(wc.status));
                return -1;
            }
            printf("发送成功! 字节数: %d\n", wc.byte_len);
            return 0;
        }
        
        usleep(10000);
        retry_count++;
    }

    fprintf(stderr, "发送超时，未收到完成通知\n");
    return -1;
}

int main(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(stderr, "用法: %s <设备名> <远程QP号> <远程LID>\n", argv[0]);
        fprintf(stderr, "示例: %s rxe_eth1 30 0\n", argv[0]);
        return 1;
    }

    const char *dev_name = argv[1];
    uint32_t remote_qp_num = atoi(argv[2]);
    uint16_t remote_lid = atoi(argv[3]);

    printf("目标信息:\n");
    printf("  QP号: %u\n", remote_qp_num);
    printf("  LID: %u\n", remote_lid);

    struct rdma_res res;
    if (rdma_init(&res, dev_name) != 0)
        return 1;

    // 初始化QP到RTR状态
    if (qp_init_to_rtr(res.qp, remote_qp_num, remote_lid) != 0) {
        fprintf(stderr, "QP RTR初始化失败\n");
        return 1;
    }

    // 初始化QP到RTS状态
    if (qp_init_to_rts(res.qp) != 0) {
        fprintf(stderr, "QP RTS初始化失败\n");
        return 1;
    }

    // 发送测试数据
    const char *data = "Hello RDMA RC Mode!";
    if (send_data(&res, data) != 0)
        return 1;

    // 清理资源
    ibv_dereg_mr(res.mr);
    free(res.buffer);
    ibv_destroy_qp(res.qp);
    ibv_destroy_cq(res.cq);
    ibv_dealloc_pd(res.pd);
    ibv_close_device(res.ctx);
    
    printf("发送端程序正常退出\n");
    return 0;
}
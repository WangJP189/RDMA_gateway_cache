/*
编译命令：
gcc ud_send_example.c -o ud_send_example -lpthread -lrdmacm -libverbs

运行命令：
sudo ./ud_send_example <device_name> <dest_gid> <dest_qp>
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
    char *send_buf;
    uint32_t qp_num;
    uint16_t lid;
    union ibv_gid gid;
};

// 初始化发送端RDMA资源
int rdma_init(struct rdma_res *res, const char *dev_name) {
    memset(res, 0, sizeof(*res));
    struct ibv_device **dev_list;
    struct ibv_device *dev = NULL;

    dev_list = ibv_get_device_list(NULL);
    if (!dev_list) {
        perror("ibv_get_device_list failed");
        return -1;
    }

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
    if (!res->ctx) {
        perror("ibv_open_device failed");
        ibv_free_device_list(dev_list);
        return -1;
    }
    ibv_free_device_list(dev_list);

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

    res->send_buf = malloc(MAX_BUFFER_SIZE);
    if (!res->send_buf) {
        perror("malloc send_buf failed");
        goto err;
    }
    memset(res->send_buf, 0, MAX_BUFFER_SIZE);

    res->mr = ibv_reg_mr(res->pd, res->send_buf, MAX_BUFFER_SIZE,
                        IBV_ACCESS_LOCAL_WRITE);
    if (!res->mr) {
        perror("ibv_reg_mr failed");
        goto err;
    }

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

    // 获取LID
    struct ibv_port_attr port_attr;
    if (ibv_query_port(res->ctx, 1, &port_attr) != 0) {
        perror("ibv_query_port failed");
        goto err;
    }
    res->lid = port_attr.lid;

    // 获取本地GID
    if (ibv_query_gid(res->ctx, 1, 0, &res->gid) != 0) {
        perror("ibv_query_gid failed");
        goto err;
    }

    printf("发送端初始化完成:\n");
    printf("  QP号: %u\n", res->qp_num);
    printf("  LID: %u\n", res->lid);
    printf("  本地GID: ");
    for (int i = 0; i < 16; i++) printf("%02x", res->gid.raw[i]);
    printf("\n");
    
    // 检查端口状态
    printf("  端口状态: %s\n", (port_attr.state == IBV_PORT_ACTIVE) ? "ACTIVE" : "INACTIVE");
    
    return 0;

err:
    if (res->mr) ibv_dereg_mr(res->mr);
    if (res->send_buf) free(res->send_buf);
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

// 初始化发送端QP
int qp_init(struct ibv_qp *qp) {
    struct ibv_qp_attr attr = {0};

    // RESET -> INIT
    attr.qp_state = IBV_QPS_INIT;
    attr.pkey_index = 0;
    attr.port_num = 1;
    attr.qkey = 0x11111111;
    if (qp_transition(qp, IBV_QPS_INIT, &attr,
                     IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_QKEY) != 0)
        return -1;

    // INIT -> RTR
    attr.qp_state = IBV_QPS_RTR;
    if (qp_transition(qp, IBV_QPS_RTR, &attr, IBV_QP_STATE) != 0)
        return -1;

    // RTR -> RTS
    attr.qp_state = IBV_QPS_RTS;
    attr.sq_psn = 0;
    if (qp_transition(qp, IBV_QPS_RTS, &attr, IBV_QP_STATE | IBV_QP_SQ_PSN) != 0)
        return -1;

    return 0;
}

// 创建地址句柄
struct ibv_ah *create_ah(struct ibv_pd *pd, union ibv_gid *dgid) {
    struct ibv_ah_attr ah_attr = {
        .is_global = 1,  // UD模式必须启用全局路由（使用GRH）
        .port_num = 1,   // 端口号（rxe设备通常为1）
        // 配置全局路由头部（GRH），UD模式必需
        .grh = {
            .dgid = *dgid,        // 目标GID（接收端的GID）
            .sgid_index = 0,      // 本地GID索引（通常为0）
            .hop_limit = 1,       // 跳数限制（本地子网填1即可）
            .flow_label = 0,      // 流标签（UD模式可设为0）
            .traffic_class = 0    // 流量类别（默认0）
        }
        // UD模式下不需要LID（dlid），软RoCE中LID无效
    };
    
    printf("创建AH，目标GID: ");
    for (int i = 0; i < 16; i++) printf("%02x", dgid->raw[i]);
    printf("\n");
    
    struct ibv_ah *ah = ibv_create_ah(pd, &ah_attr);
    if (!ah) {
        perror("ibv_create_ah failed");
        fprintf(stderr, "错误详情: 检查GID是否正确、端口号是否为1、GRH配置是否完整\n");
    }
    return ah;
}

// 发送数据
int send_data(struct rdma_res *res, uint32_t remote_qp_num, union ibv_gid *remote_gid, const char *data) {
    struct ibv_send_wr wr = {0};
    struct ibv_sge sge = {0};
    struct ibv_send_wr *bad_wr;

    // 准备发送数据
    strncpy(res->send_buf, data, MAX_BUFFER_SIZE-1);
    res->send_buf[MAX_BUFFER_SIZE-1] = '\0';
    
    sge.addr = (uintptr_t)res->send_buf;
    sge.length = strlen(res->send_buf) + 1;
    sge.lkey = res->mr->lkey;

    // 创建地址句柄
    struct ibv_ah *ah = create_ah(res->pd, remote_gid);
    if (!ah) {
        return -1;
    }

    // 设置发送工作请求
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.opcode = IBV_WR_SEND;
    wr.send_flags = IBV_SEND_SIGNALED;
    wr.wr.ud.ah = ah;
    wr.wr.ud.remote_qpn = remote_qp_num;
    wr.wr.ud.remote_qkey = 0x11111111;

    printf("准备发送数据: '%s', 长度: %zu\n", data, strlen(data));
    printf("目标QP: %u\n", remote_qp_num);

    if (ibv_post_send(res->qp, &wr, &bad_wr) != 0) {
        perror("ibv_post_send failed");
        ibv_destroy_ah(ah);
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
            ibv_destroy_ah(ah);
            return -1;
        } else if (ret > 0) {
            if (wc.status != IBV_WC_SUCCESS) {
                fprintf(stderr, "发送失败，状态: %s\n", ibv_wc_status_str(wc.status));
                ibv_destroy_ah(ah);
                return -1;
            }
            printf("发送成功! 字节数: %d\n", wc.byte_len);
            ibv_destroy_ah(ah);
            return 0;
        }
        
        usleep(10000);
        retry_count++;
    }

    fprintf(stderr, "发送超时，未收到完成通知\n");
    ibv_destroy_ah(ah);
    return -1;
}

int main(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(stderr, "用法: %s <本地设备名> <接收端QP号> <接收端GID>\n", argv[0]);
        fprintf(stderr, "示例: %s rxe_eth1 28 fe80000000000000020c29fffe10011d\n", argv[0]);
        return 1;
    }

    // 解析参数
    const char *dev_name = argv[1];
    uint32_t remote_qp_num = atoi(argv[2]);
    union ibv_gid remote_gid;
    memset(&remote_gid, 0, sizeof(remote_gid));
    
    // 解析16字节GID字符串
    if (strlen(argv[3]) != 32) {
        fprintf(stderr, "GID格式错误，应该是32个字符的十六进制字符串\n");
        return 1;
    }
    
    for (int i = 0; i < 16; i++) {
        if (sscanf(argv[3] + 2*i, "%02hhx", &remote_gid.raw[i]) != 1) {
            fprintf(stderr, "解析GID失败\n");
            return 1;
        }
    }

    printf("目标信息:\n");
    printf("  QP号: %u\n", remote_qp_num);
    printf("  GID: ");
    for (int i = 0; i < 16; i++) printf("%02x", remote_gid.raw[i]);
    printf("\n");

    struct rdma_res res;
    if (rdma_init(&res, dev_name) != 0)
        return 1;

    // 初始化QP
    if (qp_init(res.qp) != 0) {
        fprintf(stderr, "QP初始化失败\n");
        return 1;
    }

    // 发送测试数据
    const char *data = "Hello RDMA UD Mode!";
    if (send_data(&res, remote_qp_num, &remote_gid, data) != 0)
        return 1;

    // 清理资源
    ibv_dereg_mr(res.mr);
    free(res.send_buf);
    ibv_destroy_qp(res.qp);
    ibv_destroy_cq(res.cq);
    ibv_dealloc_pd(res.pd);
    ibv_close_device(res.ctx);
    
    printf("发送端程序正常退出\n");
    return 0;
}
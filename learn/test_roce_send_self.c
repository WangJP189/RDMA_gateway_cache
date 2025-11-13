/*
编译命令：
gcc test_roce_send_self.c -o test_roce_send_self -libverbs

运行命令：
sudo ./test_roce_send_self
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

// QP状态转换函数
int qp_transition(struct ibv_qp *qp, enum ibv_qp_state new_state,
                 struct ibv_qp_attr *attr, int flags) {
    if (ibv_modify_qp(qp, attr, flags) != 0) {
        perror("ibv_modify_qp failed");
        return -1;
    }
    printf("QP状态已转换为: %d\n", new_state);
    return 0;
}

// UD模式的QP初始化
int qp_init_ud(struct ibv_qp *qp) {
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

// 专门为回环设备创建AH
struct ibv_ah* create_ah_for_loopback(struct ibv_pd *pd, int port_num) {
    struct ibv_ah_attr ah_attr;
    
    // 尝试不同的配置组合
    int configs[][2] = {
        {0, 0},  // is_global=0, dlid=0
        {0, 1},  // is_global=0, dlid=1
        {1, 0},  // is_global=1, dlid=0
        {1, 1}   // is_global=1, dlid=1
    };
    
    for (int i = 0; i < 4; i++) {
        memset(&ah_attr, 0, sizeof(ah_attr));
        ah_attr.is_global = configs[i][0];
        ah_attr.dlid = configs[i][1];
        ah_attr.sl = 0;
        ah_attr.src_path_bits = 0;
        ah_attr.port_num = port_num;
        ah_attr.static_rate = 0;
        
        // 如果是全局路由，需要设置GID
        if (ah_attr.is_global) {
            // 获取本地GID
            struct ibv_context* ctx = pd->context;
            union ibv_gid gid;
            if (ibv_query_gid(ctx, port_num, 0, &gid) == 0) {
                ah_attr.grh.dgid = gid;
                ah_attr.grh.sgid_index = 0;
                ah_attr.grh.hop_limit = 1;
                ah_attr.grh.flow_label = 0;
            }
        }
        
        printf("尝试AH配置 %d: is_global=%d, dlid=%d\n", 
               i+1, ah_attr.is_global, ah_attr.dlid);
        
        struct ibv_ah *ah = ibv_create_ah(pd, &ah_attr);
        if (ah) {
            printf("AH创建成功! 配置: is_global=%d, dlid=%d\n", 
                   ah_attr.is_global, ah_attr.dlid);
            return ah;
        } else {
            printf("AH配置 %d 失败: %s\n", i+1, strerror(errno));
        }
    }
    
    return NULL;
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
    dev = get_ibv_device_by_name("rxe_eth1");
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

    // 3. 查询端口信息
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
    strcpy(buf, "RDMA Loopback Test with Multiple AH Configurations");

    mr = ibv_reg_mr(pd, buf, 1024, IBV_ACCESS_LOCAL_WRITE);
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

    // 7. 创建队列对（QP）- 使用UD模式
    struct ibv_qp_init_attr qp_attr = {
        .send_cq = cq,
        .recv_cq = cq,
        .cap = {
            .max_send_wr = 10, 
            .max_recv_wr = 10, 
            .max_send_sge = 1, 
            .max_recv_sge = 1
        },
        .qp_type = IBV_QPT_UD
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

    // 8. QP状态转换
    printf("开始QP状态转换...\n");
    if (qp_init_ud(qp) != 0) {
        fprintf(stderr, "QP状态转换失败\n");
        goto cleanup;
    }
    printf("所有QP状态转换成功完成！\n");

    // 9. 创建地址句柄（AH）- 使用专门的回环函数
    printf("尝试创建地址句柄(AH)...\n");
    struct ibv_ah *ah = create_ah_for_loopback(pd, 1);
    if (!ah) {
        fprintf(stderr, "所有AH配置尝试都失败了\n");
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
    wr.send_flags = IBV_SEND_SIGNALED;
    wr.wr.ud.ah = ah;
    wr.wr.ud.remote_qpn = qp->qp_num;
    wr.wr.ud.remote_qkey = 0x11111111;

    ret = ibv_post_send(qp, &wr, &bad_wr);
    if (ret) {
        perror("ibv_post_send failed");
        ibv_destroy_ah(ah);
        goto cleanup;
    }
    printf("发送请求已提交，等待完成...\n");

    // 11. 轮询CQ确认结果
    struct ibv_wc wc;
    int poll_ret;
    int retry_count = 0;
    const int max_retries = 100;
    
    do {
        poll_ret = ibv_poll_cq(cq, 1, &wc);
        if (poll_ret == 0) {
            usleep(10000);
            retry_count++;
            if (retry_count >= max_retries) {
                printf("轮询超时，未收到完成事件\n");
                break;
            }
        }
    } while (poll_ret == 0);

    if (poll_ret < 0) {
        perror("ibv_poll_cq failed");
    } else if (poll_ret > 0) {
        if (wc.status == IBV_WC_SUCCESS) {
            printf("RDMA回环测试成功！发送数据: %s\n", buf);
            printf("完成状态: %s\n", ibv_wc_status_str(wc.status));
        } else {
            printf("发送失败，状态: %s\n", ibv_wc_status_str(wc.status));
        }
    }

    if (ah) ibv_destroy_ah(ah);

cleanup:
    // 清理资源
    if (qp) ibv_destroy_qp(qp);
    if (cq) ibv_destroy_cq(cq);
    if (mr) ibv_dereg_mr(mr);
    if (buf) free(buf);
    if (pd) ibv_dealloc_pd(pd);
    if (ctx) ibv_close_device(ctx);
    return ret;
}
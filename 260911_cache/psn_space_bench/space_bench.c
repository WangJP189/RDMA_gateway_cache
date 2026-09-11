/*
 * space_bench.c — RDMA 网关「缓存空间开销」微基准测试
 * ------------------------------------------------------------------
 *
 * 目的：
 *   在单台机器上，对六种「按 PSN 缓存报文」的内存组织方式做「空间开销」对比，
 *   与 psn_lookup_benchmark（时间开销）互补，构成论文的「时间 + 空间」两组证据。
 *
 *   对比的方法（与时间实验一致，外加一个理想下界）：
 *     1. fifo          —— FIFO 队列：指针数组 + 每包一个报文结构   O(n) 查找
 *     2. chained_hash  —— 链式哈希：桶数组 + 每包一个节点 + payload
 *     3. balanced_tree —— AVL 平衡树：每包一个节点（左/右指针）+ payload
 *     4. psn_map_fixed —— PSN 映射（现状）：每包固定 malloc(5KB) + 环形数组
 *     5. psn_map_dynamic——PSN 映射（新机制）：每包 malloc(header+payload) + 环形数组
 *     6. contiguous    —— 直接叠加包（理想下界）：N 个 [header+payload] 紧密拼接
 *
 *   论文核心卖点：PSN 映射用 ring_index = psn % RING_SIZE 直接下标，
 *   「以空间换时间」。本基准量化它究竟多花了多少空间（环形数组 + 内存块），
 *   并展示「动态块」机制如何把空间利用率抬到逼近 contiguous 下界。
 *
 * 指标（因变量）：
 *   - allocated_bytes ：总分配字节（逐项精确统计 malloc / 数组 / 节点 / 对齐开销）
 *   - payload_bytes   ：有效数据字节 = N × L
 *   - utilization     ：空间利用率 = payload_bytes / allocated_bytes（0~1）
 *   - avg_bytes_per_pkt：每包平均分配字节
 *
 * 变量（自变量）：
 *   - L ∈ {64,128,256,512,1024,2048,4096}（覆盖到 RoCEv2 MTU 附近）
 *   - N（缓存包数，默认 10240，与时间实验对齐）
 *
 * 分配模型（关键假设，见 README）：
 *   每次 malloc(n) 实际占用 ALIGN16(n) 字节（与 glibc 16 字节对齐一致），
 *   不含 glibc 内部 chunk 头（各方法近似相同，在相对比较中被抵消）。
 *   PSN 映射的环形数组固定为 RING_SIZE(10240) × 8B = 80KB / 连接。
 *
 * 输出：
 *   - space_summary.csv      ：方法 × 包大小 的空间利用率总表
 *   - space_multiflow.csv    ：（可选 --multiflow）多流缓存摊销结果
 *   - 终端打印可直接进论文的对比表
 *
 * 编译（Ubuntu 24.04）：
 *   make     # 等价于 gcc -O2 -Wall -std=gnu11 space_bench.c -o space_bench
 *
 * 作者：为 RDMA 网关论文补实验所用
 */

#define _GNU_SOURCE
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ===================== 常量（对齐真实工程 pkt_cache.h） ===================== */

#define RING_SIZE 10240   /* 环形数组长度（每连接） */
#define HASH_NBUCKETS 4096 /* 链式哈希桶数（对齐时间实验） */
#define MEM_BLOCK_SIZE 5120 /* 固定模式：每个包固定 5KB */
#define BLOCK_ALIGN 16    /* 动态块对齐粒度（glibc malloc 对齐） */

#define ALIGN16(x) (((x) + (BLOCK_ALIGN - 1u)) & ~(uint64_t)(BLOCK_ALIGN - 1u))

/* ===================== 结构定义（与真实工程/时间实验对齐） ================== */

/* 内存块头：嵌在内存块开头，布局 [mem_block_header][data]。
 * sizeof = 24（int 4 + pad4 + uint64 8 + uint32 4 + pad4） */
struct mem_block_header {
    int data_len;        /* 有效 RDMA 数据包长度 */
    uint64_t recv_stamp; /* 缓存时间戳（毫秒） */
    uint32_t psn;        /* 该内存块对应的 PSN */
};

/* FIFO 报文槽：元数据内联（psn/len/stamp），payload 紧随其后 */
struct fifo_packet {
    uint32_t psn;
    uint32_t len;
    uint64_t stamp;
    /* + payload[L]（malloc 时追加） */
};

/* 链式哈希节点：psn + 指向 payload 块的指针 + next */
struct hash_node {
    uint32_t psn;
    struct mem_block_header *block;
    struct hash_node *next;
};

/* AVL 平衡树节点：psn + 指向 payload 的指针 + 左/右子树 + 高度 */
struct avl_node {
    uint32_t psn;
    struct mem_block_header *block;
    struct avl_node *left;
    struct avl_node *right;
    int height;
};

/* ===================== 方法枚举 ===================== */

typedef enum {
    M_FIFO = 0,
    M_HASH = 1,
    M_TREE = 2,
    M_PSN_FIXED = 3,
    M_PSN_DYNAMIC = 4,
    M_CONTIG = 5,
    M_COUNT = 6
} method_t;

static const char *method_name[] = {"fifo",       "chained_hash",
                                    "balanced_tree", "psn_map_fixed",
                                    "psn_map_dynamic", "contiguous"};

/* ===================== 空间开销模型 ===================== */

/* 每个 malloc(n) 实际占用的字节 = ALIGN16(n)。
 * 注：不含 glibc chunk 头（~8~16B/块，各方法近似相同，相对比较抵消）。 */
static uint64_t m_alloc(uint64_t n) { return ALIGN16(n); }

/* 单个 payload 块 = [mem_block_header + data]，按 16B 对齐 */
static uint64_t payload_block(uint64_t L) {
    return m_alloc(sizeof(struct mem_block_header) + L);
}

/* 各方法的「总分配字节」。N = 缓存包数，L = 单包 payload 字节。 */

static uint64_t bytes_fifo(uint64_t N, uint64_t L) {
    /* 指针数组 N×8B + 每包一个内联报文结构 */
    uint64_t slot = m_alloc(sizeof(struct fifo_packet) + L);
    return N * sizeof(void *) + N * slot;
}

static uint64_t bytes_hash(uint64_t N, uint64_t L) {
    /* 桶数组 + 每包一个节点(含 next 指针) + 每包一个独立 payload 块 */
    uint64_t node = m_alloc(sizeof(struct hash_node));
    return HASH_NBUCKETS * sizeof(void *) + N * (node + payload_block(L));
}

static uint64_t bytes_tree(uint64_t N, uint64_t L) {
    /* 每包一个 AVL 节点(含 left/right 指针) + 每包一个独立 payload 块 */
    uint64_t node = m_alloc(sizeof(struct avl_node));
    return N * (node + payload_block(L));
}

static uint64_t bytes_psn_fixed(uint64_t N, uint64_t L) {
    /* 现状：每包固定 malloc(5KB) + 环形数组 80KB */
    (void)L;
    return N * m_alloc(MEM_BLOCK_SIZE) + RING_SIZE * sizeof(void *);
}

static uint64_t bytes_psn_dynamic(uint64_t N, uint64_t L) {
    /* 新机制：每包 malloc(header+payload) + 环形数组 80KB */
    return N * payload_block(L) + RING_SIZE * sizeof(void *);
}

static uint64_t bytes_contiguous(uint64_t N, uint64_t L) {
    /* 理想下界：N 个 [header+payload] 紧密拼接，无对齐、无每包 malloc 头 */
    return N * (sizeof(struct mem_block_header) + L);
}

static uint64_t allocated_bytes(method_t m, uint64_t N, uint64_t L) {
    switch (m) {
    case M_FIFO:
        return bytes_fifo(N, L);
    case M_HASH:
        return bytes_hash(N, L);
    case M_TREE:
        return bytes_tree(N, L);
    case M_PSN_FIXED:
        return bytes_psn_fixed(N, L);
    case M_PSN_DYNAMIC:
        return bytes_psn_dynamic(N, L);
    case M_CONTIG:
        return bytes_contiguous(N, L);
    default:
        return 0;
    }
}

/* ===================== 输出工具 ===================== */

static void fmt_bytes(uint64_t b, char *buf, size_t len) {
    if (b < 1024)
        snprintf(buf, len, "%" PRIu64 " B", b);
    else if (b < 1024 * 1024)
        snprintf(buf, len, "%.2f KB", (double)b / 1024.0);
    else if (b < 1024ULL * 1024 * 1024)
        snprintf(buf, len, "%.2f MB", (double)b / (1024.0 * 1024.0));
    else
        snprintf(buf, len, "%.2f GB", (double)b / (1024.0 * 1024.0 * 1024.0));
}

static FILE *fopen_join(const char *dir, const char *name, const char *mode) {
    char path[512];
    if (strcmp(dir, ".") == 0)
        snprintf(path, sizeof path, "%s", name);
    else
        snprintf(path, sizeof path, "%s/%s", dir, name);
    return fopen(path, mode);
}

/* ===================== 主实验：单流空间利用率 ===================== */

static void run_summary(uint64_t N, const char *outdir) {
    static const uint64_t L_list[] = {64, 128, 256, 512, 1024, 2048, 4096};
    int nL = (int)(sizeof L_list / sizeof L_list[0]);

    printf("\n========== 空间开销实验 ==========\n");
    printf("缓存包数 N = %" PRIu64 "\n", N);
    printf("结构大小: mem_block_header=%zu B, fifo_packet=%zu B, "
           "hash_node=%zu B, avl_node=%zu B\n\n",
           sizeof(struct mem_block_header), sizeof(struct fifo_packet),
           sizeof(struct hash_node), sizeof(struct avl_node));

    FILE *out = fopen_join(outdir, "space_summary.csv", "w");
    fprintf(out, "method,packet_size,N,payload_bytes,allocated_bytes,"
                 "utilization,avg_bytes_per_pkt\n");

    /* 表头：第一行为各方法的列名（转置打印，便于论文直接使用） */
    printf("%-10s", "L(bytes)");
    for (method_t m = 0; m < M_COUNT; m++)
        printf("%18s", method_name[m]);
    printf("\n");

    for (int i = 0; i < nL; i++) {
        uint64_t L = L_list[i];
        printf("%-10" PRIu64, L);
        for (method_t m = 0; m < M_COUNT; m++) {
            uint64_t alloc = allocated_bytes(m, N, L);
            uint64_t payload = N * L;
            double util = (double)payload / (double)alloc;
            double avg = (double)alloc / (double)N;
            printf("%17.1f%%", util * 100.0);
            fprintf(out,
                    "%s,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
                    ",%.6f,%.2f\n",
                    method_name[m], L, N, payload, alloc, util, avg);
        }
        printf("\n");
    }
    fclose(out);
    printf("\n[OK] 空间利用率表已写入 %s/space_summary.csv\n", outdir);
}

/* =================== 可选实验：多流缓存（环形数组摊销） =================== */

/* 多流：K 条连接，每条连接一个 ring_buf(80KB 固定开销)，每连接缓存 npp 包。
 * total_packets = K * npp，ring = K * 80KB，block = total_packets * 块大小。 */
static void run_multiflow(uint64_t L, const char *outdir) {
    FILE *out = fopen_join(outdir, "space_multiflow.csv", "w");
    fprintf(out, "scenario,K,npp,total_packets,packet_size,ring_bytes,"
                 "block_bytes,allocated_bytes,utilization\n");

    printf("\n========== 多流缓存实验（环形数组摊销） ==========\n");
    printf("固定 packet_size L = %" PRIu64 " B\n", L);

    /* 场景 1：每连接满载 npp=10240，连接数 K 变化 → 环形数组开销占比恒定 */
    printf("\n[场景1] 每连接满载 npp=%d，连接数 K 变化（利用率应不随 K 劣化）\n",
           RING_SIZE);
    printf("%-8s %-10s %-14s %-14s %-12s\n", "K", "总包数", "环形数组",
           "内存块", "利用率");
    static const uint64_t K_list[] = {1, 8, 64, 256};
    int nK = (int)(sizeof K_list / sizeof K_list[0]);
    for (int i = 0; i < nK; i++) {
        uint64_t K = K_list[i];
        uint64_t npp = RING_SIZE; /* 每连接满载 */
        uint64_t total = K * npp;
        uint64_t ring = K * RING_SIZE * sizeof(void *);
        uint64_t block = total * payload_block(L);
        uint64_t alloc = ring + block;
        double util = (double)(total * L) / (double)alloc;
        char rb[32], bb[32];
        fmt_bytes(ring, rb, sizeof rb);
        fmt_bytes(block, bb, sizeof bb);
        printf("%-8" PRIu64 " %-10" PRIu64 " %-14s %-14s %-11.1f%%\n", K,
               total, rb, bb, util * 100.0);
        fprintf(out,
                "vary_K,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
                ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%.6f\n",
                K, npp, total, L, ring, block, alloc, util);
    }

    /* 场景 2：固定 1 条连接，占用深度 npp 变化 → 环形数组固定开销被摊薄 */
    printf("\n[场景2] 固定 1 条连接，占用深度 npp 变化（环形数组 80KB 被摊薄）\n");
    printf("%-12s %-14s %-12s\n", "npp", "环形数组/包", "利用率");
    static const uint64_t npp_list[] = {256, 512, 1024, 2048, 4096, RING_SIZE};
    int nn = (int)(sizeof npp_list / sizeof npp_list[0]);
    for (int i = 0; i < nn; i++) {
        uint64_t npp = npp_list[i];
        uint64_t total = npp;
        uint64_t ring = RING_SIZE * sizeof(void *); /* 1 条连接的 80KB */
        uint64_t block = total * payload_block(L);
        uint64_t alloc = ring + block;
        double util = (double)(total * L) / (double)alloc;
        double ring_per_pkt = (double)ring / (double)total;
        printf("%-12" PRIu64 " %-14.1f B %-11.1f%%\n", npp, ring_per_pkt,
               util * 100.0);
        fprintf(out,
                "amortize,1,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
                ",%" PRIu64 ",%" PRIu64 ",%.6f\n",
                npp, total, L, ring, block, alloc, util);
    }

    fclose(out);
    printf("\n[OK] 多流结果已写入 %s/space_multiflow.csv\n", outdir);
}

/* ===================== 主流程 ===================== */

static void usage(const char *prog) {
    fprintf(stderr,
            "用法: %s [选项]\n"
            "  -n N         缓存包数（默认 10240）\n"
            "  -o DIR       输出目录（默认 .）\n"
            "  --multiflow  额外生成多流缓存摊销结果 space_multiflow.csv\n"
            "  -L L         --multiflow 下的 packet_size（默认 1024）\n"
            "  -h           帮助\n",
            prog);
}

int main(int argc, char **argv) {
    uint64_t N = RING_SIZE;
    uint64_t mf_L = 1024;
    const char *outdir = ".";
    int multiflow = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-n") && i + 1 < argc)
            N = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(a, "-o") && i + 1 < argc)
            outdir = argv[++i];
        else if (!strcmp(a, "-L") && i + 1 < argc)
            mf_L = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(a, "--multiflow"))
            multiflow = 1;
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "未知选项: %s\n", a);
            usage(argv[0]);
            return 1;
        }
    }

    run_summary(N, outdir);
    if (multiflow)
        run_multiflow(mf_L, outdir);

    printf("\n提示：运行 python3 plot_space.py --dir %s --out %s 画图\n",
           outdir, outdir);
    return 0;
}

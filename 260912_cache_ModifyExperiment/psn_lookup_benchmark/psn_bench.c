/*
 * psn_bench.c — RDMA 网关「PSN 查找延迟」微基准测试
 * ------------------------------------------------------------------
 *
 * 目的：
 *   在单台机器上，对四种「按 PSN 缓存/查找报文」的数据结构做查找延迟对比：
 *     1. FIFO Queue         —— 按到达顺序存储，按 PSN 查找需线性扫描   O(n)
 *     2. Chained Hash       —— PSN 哈希 + 拉链法，查找平均 O(1)（无序）
 *     3. Balanced Tree      —— AVL 平衡树，查找 O(log n)（有序但指针跳转）
 *     4. PSN Mapping (本文) —— 环形数组直接映射 psn % RING_SIZE，O(1)
 *                              且支持按 PSN 顺序的连续存储与提取
 *
 * 输出：
 *   - 每种方法 x 访问模式 的查找延迟样本（排序后），写入 cdf_<方法>_<模式>.csv
 *   - 汇总统计（min/p50/p90/p99/p99.9/max/mean），打印到终端并写入 summary.csv
 *   - 可选 --sweep ：不同缓存占用 N 下的中位延迟，写入 scaling.csv
 *   - 可选 --mode range ：按 PSN 顺序连续提取（GBN/SR 重传场景），写入
 * range_summary.csv
 *
 * 测量方法（批量计时）：
 *   把 B 次查找打包成一批，用一对 rdtsc 计时，再除以 B 得到「每包平均周期」。
 *   这样可以把 rdtsc 自身的固定开销（在虚拟机里可能高达微秒级，因为
 * lfence/rdtscp 会触发 VM exit / 模拟）摊薄到每次查找上，避免其掩盖 O(1) 与
 * O(n) 之间的纳秒级差异。 —— 这是测量纳秒级内存操作的通用做法（如 LMbench）。
 *
 * 编译（Ubuntu 24.04）：
 *   make            # 等价于 gcc -O2 -Wall -std=gnu11 psn_bench.c -o psn_bench
 *
 * 作者：为 RDMA 网关论文补实验所用
 */

#define _GNU_SOURCE
#include <inttypes.h>
#include <math.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* ===================== 常量（对齐真实工程） ===================== */

/* 对齐 pkt_cache.h 中的 RING_BUFFER_SIZE：真实网关环形数组大小 */
#define RING_SIZE 10240
/* 链式哈希桶数：负载因子 = N / HASH_NBUCKETS */
#define HASH_NBUCKETS 4096
/* 每个「缓存报文」的载荷字节数（仅影响内存占用，不影响查找路径） */
#define PAYLOAD_LEN 64

/* ===================== 报文与结构定义 ===================== */

/* 一个缓存报文：模拟真实工程中的 [mem_block_header][data] 布局 */
struct packet {
    uint32_t psn;   /* 包序列号（24 位 PSN 的低 24 位） */
    uint32_t len;   /* 有效载荷长度 */
    uint64_t stamp; /* 缓存时间戳（模拟 recv_stamp） */
    uint8_t data[PAYLOAD_LEN];
};

/* ---- FIFO 队列：数组式循环队列，按到达顺序存放 ---- */
struct fifo {
    struct packet **arr;
    uint32_t cap;
    uint32_t count;
};

/* ---- Chained Hash：桶数组 + 链表 ---- */
struct hash_node {
    uint32_t psn;
    struct packet *pkt;
    struct hash_node *next;
};
struct hash_table {
    struct hash_node **buckets;
    uint32_t nbuckets;
};

/* ---- Balanced Tree：AVL 平衡二叉搜索树 ---- */
struct avl_node {
    uint32_t psn;
    struct packet *pkt;
    struct avl_node *left, *right;
    int height;
};

/* ---- PSN Mapping：环形数组直接映射 ---- */
struct psnmap {
    struct packet **arr; /* 大小 RING_SIZE，索引 = psn % RING_SIZE */
    uint32_t size;
};

/* 方法枚举 */
typedef enum {
    M_FIFO = 0,
    M_HASH = 1,
    M_TREE = 2,
    M_PSNMAP = 3,
    M_COUNT = 4
} method_t;

static const char *method_name[] = {"fifo", "chained_hash", "balanced_tree",
                                    "psn_mapping"};
static const char *method_label[] = {"FIFO Queue", "Chained Hash",
                                     "Balanced Tree", "PSN Mapping (ours)"};

/* 统一缓存抽象 */
struct cache {
    method_t m;
    union {
        struct fifo fifo;
        struct hash_table hash;
        struct avl_node *tree;
        struct psnmap map;
    } u;
};

/* ===================== TSC 计时 ===================== */

/* 普通 rdtsc + 编译器屏障（memory clobber）。
 * 故意不用 lfence / rdtscp：它们在 VMware 虚拟机里可能触发 VM exit，
 * 单次可达微秒级，会完全淹没纳秒级的查找开销。 */
static inline uint64_t rdtsc_raw(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi) : : "memory");
    return ((uint64_t)hi << 32) | lo;
}

static double cycles_per_ns = 1.0; /* 校准得到 */

/* 校准 TSC：在约 100ms 内对 clock_gettime 与 rdtsc 求比值 */
static void calibrate_tsc(void) {
    struct timespec a, b;
    uint64_t c0, c1;
    const double target_ns = 100.0 * 1e6; /* 100 ms */

    clock_gettime(CLOCK_MONOTONIC, &a);
    c0 = rdtsc_raw();
    do {
        clock_gettime(CLOCK_MONOTONIC, &b);
        c1 = rdtsc_raw();
    } while ((double)(b.tv_sec - a.tv_sec) * 1e9 + (b.tv_nsec - a.tv_nsec) <
             target_ns);

    double ns = (double)(b.tv_sec - a.tv_sec) * 1e9 + (b.tv_nsec - a.tv_nsec);
    cycles_per_ns = (double)(c1 - c0) / ns;
    /* cycles_per_ns 的单位就是 GHz（cycles/ns == GHz） */
    printf("[CALIB] TSC 频率约 %.2f GHz (cycles_per_ns=%.3f)\n", cycles_per_ns,
           cycles_per_ns);
}

/* ===================== 伪随机数（xorshift32，可复现） ===================== */

static uint32_t rng_state = 42;
static inline uint32_t xorshift32(void) {
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return rng_state = x;
}

/* ===================== AVL 实现 ===================== */

static inline int avl_h(const struct avl_node *n) { return n ? n->height : 0; }

static inline void avl_upd(struct avl_node *n) {
    int l = avl_h(n->left), r = avl_h(n->right);
    n->height = 1 + (l > r ? l : r);
}

static inline int avl_bal(const struct avl_node *n) {
    return avl_h(n->left) - avl_h(n->right);
}

static struct avl_node *avl_rot_r(struct avl_node *y) {
    struct avl_node *x = y->left, *t2 = x->right;
    x->right = y;
    y->left = t2;
    avl_upd(y);
    avl_upd(x);
    return x;
}

static struct avl_node *avl_rot_l(struct avl_node *x) {
    struct avl_node *y = x->right, *t2 = y->left;
    y->left = x;
    x->right = t2;
    avl_upd(x);
    avl_upd(y);
    return y;
}

static struct avl_node *avl_insert(struct avl_node *n, uint32_t psn,
                                   struct packet *p) {
    if (!n) {
        struct avl_node *nn = malloc(sizeof(*nn));
        nn->psn = psn;
        nn->pkt = p;
        nn->left = nn->right = NULL;
        nn->height = 1;
        return nn;
    }
    if (psn < n->psn)
        n->left = avl_insert(n->left, psn, p);
    else if (psn > n->psn)
        n->right = avl_insert(n->right, psn, p);
    else {
        n->pkt = p;
        return n;
    }

    avl_upd(n);
    int b = avl_bal(n);
    if (b > 1 && psn < n->left->psn)
        return avl_rot_r(n);
    if (b < -1 && psn > n->right->psn)
        return avl_rot_l(n);
    if (b > 1 && psn > n->left->psn) {
        n->left = avl_rot_l(n->left);
        return avl_rot_r(n);
    }
    if (b < -1 && psn < n->right->psn) {
        n->right = avl_rot_r(n->right);
        return avl_rot_l(n);
    }
    return n;
}

static inline struct packet *avl_lookup(const struct avl_node *n,
                                        uint32_t psn) {
    while (n) {
        if (psn == n->psn)
            return n->pkt;
        n = (psn < n->psn) ? n->left : n->right;
    }
    return NULL;
}

static void avl_free(struct avl_node *n) {
    if (!n)
        return;
    avl_free(n->left);
    avl_free(n->right);
    free(n);
}

/* ===================== 缓存统一接口 ===================== */

static void cache_init(struct cache *c, method_t m, uint32_t cap) {
    memset(c, 0, sizeof(*c));
    c->m = m;
    switch (m) {
    case M_FIFO:
        c->u.fifo.arr = calloc(cap, sizeof(struct packet *));
        c->u.fifo.cap = cap;
        c->u.fifo.count = 0;
        break;
    case M_HASH:
        c->u.hash.nbuckets = HASH_NBUCKETS;
        c->u.hash.buckets = calloc(HASH_NBUCKETS, sizeof(struct hash_node *));
        break;
    case M_TREE:
        c->u.tree = NULL;
        break;
    case M_PSNMAP:
        c->u.map.size = RING_SIZE;
        c->u.map.arr = calloc(RING_SIZE, sizeof(struct packet *));
        break;
    default:
        break;
    }
}

static void cache_insert(struct cache *c, uint32_t psn, struct packet *p) {
    switch (c->m) {
    case M_FIFO: {
        if (c->u.fifo.count < c->u.fifo.cap)
            c->u.fifo.arr[c->u.fifo.count++] = p;
        break;
    }
    case M_HASH: {
        uint32_t h = psn % c->u.hash.nbuckets;
        struct hash_node *n = malloc(sizeof(*n));
        n->psn = psn;
        n->pkt = p;
        n->next = c->u.hash.buckets[h];
        c->u.hash.buckets[h] = n; /* 头插 */
        break;
    }
    case M_TREE:
        c->u.tree = avl_insert(c->u.tree, psn, p);
        break;
    case M_PSNMAP: {
        uint32_t idx = psn % c->u.map.size;
        c->u.map.arr[idx] = p;
        break;
    }
    default:
        break;
    }
}

/* 查找：给定 PSN，返回命中报文指针，未命中返回 NULL。
 * 四种方法都执行「按 PSN 匹配」这一相同语义，差异只在遍历长度。 */
static inline struct packet *cache_lookup(const struct cache *c, uint32_t psn) {
    switch (c->m) {
    case M_FIFO: {
        struct packet **arr = c->u.fifo.arr;
        uint32_t n = c->u.fifo.count;
        for (uint32_t i = 0; i < n; i++) {
            if (arr[i]->psn == psn)
                return arr[i];
        }
        return NULL;
    }
    case M_HASH: {
        uint32_t h = psn % c->u.hash.nbuckets;
        for (struct hash_node *n = c->u.hash.buckets[h]; n; n = n->next) {
            if (n->psn == psn)
                return n->pkt;
        }
        return NULL;
    }
    case M_TREE:
        return avl_lookup(c->u.tree, psn);
    case M_PSNMAP: {
        uint32_t idx = psn % c->u.map.size;
        struct packet *p = c->u.map.arr[idx];
        if (p && p->psn == psn)
            return p;
        return NULL;
    }
    default:
        return NULL;
    }
}

static void cache_destroy(struct cache *c) {
    switch (c->m) {
    case M_FIFO:
        free(c->u.fifo.arr);
        break;
    case M_HASH: {
        for (uint32_t i = 0; i < c->u.hash.nbuckets; i++) {
            struct hash_node *n = c->u.hash.buckets[i];
            while (n) {
                struct hash_node *t = n->next;
                free(n);
                n = t;
            }
        }
        free(c->u.hash.buckets);
        break;
    }
    case M_TREE:
        avl_free(c->u.tree);
        break;
    case M_PSNMAP:
        free(c->u.map.arr);
        break;
    default:
        break;
    }
}

/* ===================== 按序提取（GBN/SR 重传场景） ===================== */

/* 提取 [start, start+L) 的连续 PSN 报文，按 PSN 顺序写入 out，返回命中数 */
static uint32_t fifo_extract(const struct fifo *f, uint32_t start, uint32_t L,
                             struct packet **out) {
    uint32_t found = 0;
    for (uint32_t i = 0; i < f->count; i++) {
        if (f->arr[i]->psn == start) {
            for (uint32_t j = 0; j < L && (i + j) < f->count; j++)
                out[found++] = f->arr[i + j];
            break;
        }
    }
    return found;
}

static uint32_t hash_extract(const struct hash_table *h, uint32_t start,
                             uint32_t L, struct packet **out) {
    uint32_t found = 0;
    for (uint32_t i = 0; i < L; i++) {
        uint32_t psn = start + i;
        uint32_t b = psn % h->nbuckets;
        for (struct hash_node *n = h->buckets[b]; n; n = n->next) {
            if (n->psn == psn) {
                out[found++] = n->pkt;
                break;
            }
        }
    }
    return found;
}

static uint32_t tree_extract(struct avl_node *root, uint32_t start, uint32_t L,
                             struct packet **out) {
    /* 迭代式中序遍历，收集 [start, start+L) */
    struct avl_node *stack[64];
    int top = 0;
    struct avl_node *cur = root;
    uint32_t found = 0;
    uint32_t end = start + L;
    while (cur || top > 0) {
        while (cur) {
            stack[top++] = cur;
            cur = cur->left;
        }
        cur = stack[--top];
        if (cur->psn >= start) {
            if (cur->psn < end && found < L) {
                out[found++] = cur->pkt;
            } else if (cur->psn >= end) {
                return found;
            }
        }
        cur = cur->right;
    }
    return found;
}

static uint32_t map_extract(const struct psnmap *m, uint32_t start, uint32_t L,
                            struct packet **out) {
    uint32_t found = 0;
    for (uint32_t i = 0; i < L; i++) {
        uint32_t psn = start + i;
        uint32_t idx = psn % m->size;
        struct packet *p = m->arr[idx];
        if (p && p->psn == psn)
            out[found++] = p;
    }
    return found;
}

static uint32_t cache_extract(const struct cache *c, uint32_t start, uint32_t L,
                              struct packet **out) {
    switch (c->m) {
    case M_FIFO:
        return fifo_extract(&c->u.fifo, start, L, out);
    case M_HASH:
        return hash_extract(&c->u.hash, start, L, out);
    case M_TREE:
        return tree_extract(c->u.tree, start, L, out);
    case M_PSNMAP:
        return map_extract(&c->u.map, start, L, out);
    default:
        return 0;
    }
}

/* ===================== 统计 ===================== */

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

/* 重复测量（--reps）的均值 / 样本标准差（误差棒用） */
static double mean_d(const double *v, uint32_t n) {
    double s = 0;
    for (uint32_t i = 0; i < n; i++)
        s += v[i];
    return n ? s / n : 0.0;
}

static double stddev_d(const double *v, uint32_t n, double m) {
    double s = 0;
    for (uint32_t i = 0; i < n; i++) {
        double d = v[i] - m;
        s += d * d;
    }
    return n > 1 ? sqrt(s / (n - 1)) : 0.0;
}

typedef struct {
    double min_ns, p50_ns, p90_ns, p99_ns, p999_ns, max_ns, mean_ns;
} latency_stats;

/* 从已排序的「每包周期」数组计算统计量（ns） */
static latency_stats stats_from_sorted(const uint64_t *cycles, uint32_t n) {
    latency_stats s;
    uint64_t sum = 0;
    for (uint32_t i = 0; i < n; i++)
        sum += cycles[i];

#define PICK(p)                                                                \
    ((double)cycles[((uint64_t)n * (p)) / 1000]) /* p 单位千分之一 */
    s.min_ns = (double)cycles[0] / cycles_per_ns;
    s.p50_ns = (double)PICK(500) / cycles_per_ns;
    s.p90_ns = (double)PICK(900) / cycles_per_ns;
    s.p99_ns = (double)PICK(990) / cycles_per_ns;
    s.p999_ns = (double)PICK(999) / cycles_per_ns;
    s.max_ns = (double)cycles[n - 1] / cycles_per_ns;
    s.mean_ns = (double)sum / (double)n / cycles_per_ns;
#undef PICK
    return s;
}

/* ===================== 主基准循环 ===================== */

/* 全局防优化标记：接收 lookup 返回值，防止编译器把查找优化掉 */
volatile uint64_t g_sink = 0;

/* 把输出目录和文件名拼成完整路径并打开 */
static FILE *fopen_join(const char *dir, const char *name, const char *mode) {
    char path[512];
    if (strcmp(dir, ".") == 0)
        snprintf(path, sizeof path, "%s", name);
    else
        snprintf(path, sizeof path, "%s/%s", dir, name);
    return fopen(path, mode);
}

/* 批量计时查找：R 批、每批 B 次查找，输出「每包平均周期」到
 * per_lookup_cycles[0..R)。 psns 至少要有 R*B 个元素。 */
static void bench_lookup(struct cache *c, const uint32_t *psns, uint32_t R,
                         uint32_t B, uint64_t *per_lookup_cycles) {
    volatile uint64_t sink = 0;
    uint32_t total = R * B;

    /* 预热：热身 cache/TLB/分支预测 */
    for (uint32_t i = 0; i < total; i++) {
        struct packet *p = cache_lookup(c, psns[i]);
        sink += (p ? (uint64_t)p->psn : 1ULL);
    }

    /* 正式测量：R 批，每批 B 次查找，只计一对 rdtsc */
    for (uint32_t r = 0; r < R; r++) {
        uint64_t acc = 0;
        uint64_t t0 = rdtsc_raw();
        for (uint32_t j = 0; j < B; j++) {
            struct packet *p = cache_lookup(c, psns[r * B + j]);
            acc += (p ? (uint64_t)p->psn : 1ULL);
        }
        uint64_t t1 = rdtsc_raw();
        per_lookup_cycles[r] = (t1 - t0) / B; /* 每包平均周期 */
        sink += acc;
    }
    g_sink = sink;
}

/* 生成查询 PSN 序列（写入 psns[0..total)） */
static void gen_psns(uint32_t *psns, uint32_t total, uint32_t base, uint32_t N,
                     int sequential) {
    if (sequential) {
        for (uint32_t i = 0; i < total; i++)
            psns[i] = base + (i % N);
    } else {
        for (uint32_t i = 0; i < total; i++)
            psns[i] = base + (xorshift32() % N);
    }
}

/* ===================== 输出 ===================== */

static void fmt_ns(double ns, char *buf, size_t len) {
    if (ns < 1000.0)
        snprintf(buf, len, "%.1f ns", ns);
    else if (ns < 1e6)
        snprintf(buf, len, "%.2f us", ns / 1e3);
    else if (ns < 1e9)
        snprintf(buf, len, "%.2f ms", ns / 1e6);
    else
        snprintf(buf, len, "%.2f s", ns / 1e9);
}

static void print_stats_row(method_t m, const char *pattern,
                            const latency_stats *s) {
    char mn[32], p50[32], p90[32], p99[32], p999[32], mx[32], mean[32];
    fmt_ns(s->min_ns, mn, sizeof mn);
    fmt_ns(s->p50_ns, p50, sizeof p50);
    fmt_ns(s->p90_ns, p90, sizeof p90);
    fmt_ns(s->p99_ns, p99, sizeof p99);
    fmt_ns(s->p999_ns, p999, sizeof p999);
    fmt_ns(s->max_ns, mx, sizeof mx);
    fmt_ns(s->mean_ns, mean, sizeof mean);
    printf("%-14s %-11s %12s %12s %12s %12s %12s %12s %12s\n", method_name[m],
           pattern, mn, p50, p90, p99, p999, mx, mean);
}

/* ===================== 主流程 ===================== */

static void usage(const char *prog) {
    fprintf(stderr,
            "用法: %s [选项]\n"
            "  -n N        缓存占用（报文数，默认 10240，<= %d）\n"
            "  -m M        每种方法总查找次数（默认 1000000）\n"
            "  -B B        每批查找次数（批量计时，默认 "
            "512；越大计时开销越小、样本越少）\n"
            "  -p PATTERN  访问模式: random | sequential | both (默认 both)\n"
            "  -s SEED     随机种子（默认 42）\n"
            "  --reps K    重复测量次数（默认 5，用于误差棒 / CDF 池化）\n"
            "  -o DIR      输出目录（默认 .）\n"
            "  --cpu N     绑定到 CPU N（默认不绑定）\n"
            "  --sweep     扫描不同 N，输出 scaling.csv\n"
            "  --mode MODE lookup(默认) | range(按序提取)\n"
            "  --range-len L     range 模式下每次提取的连续 PSN 数（默认 64）\n"
            "  --range-num R     range 模式下的随机区间总数（默认 5000）\n"
            "  --range-batch G   range 模式下每批计时包含的区间数（默认 32）\n"
            "  -h          帮助\n",
            prog, RING_SIZE);
}

int main(int argc, char **argv) {
    uint32_t N = RING_SIZE;
    uint32_t M = 1000000;
    uint32_t B = 512;
    const char *pattern = "both";
    const char *outdir = ".";
    const char *mode = "lookup";
    uint32_t range_len = 64;
    uint32_t range_num = 5000;
    uint32_t range_batch = 32;
    int sweep = 0;
    int cpu = -1;
    uint32_t seed = 42;
    uint32_t reps = 5;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-n") && i + 1 < argc)
            N = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(a, "-m") && i + 1 < argc)
            M = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(a, "-B") && i + 1 < argc)
            B = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(a, "-p") && i + 1 < argc)
            pattern = argv[++i];
        else if (!strcmp(a, "-s") && i + 1 < argc)
            seed = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(a, "--reps") && i + 1 < argc)
            reps = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(a, "-o") && i + 1 < argc)
            outdir = argv[++i];
        else if (!strcmp(a, "--cpu") && i + 1 < argc)
            cpu = atoi(argv[++i]);
        else if (!strcmp(a, "--sweep"))
            sweep = 1;
        else if (!strcmp(a, "--mode") && i + 1 < argc)
            mode = argv[++i];
        else if (!strcmp(a, "--range-len") && i + 1 < argc)
            range_len = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(a, "--range-num") && i + 1 < argc)
            range_num = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(a, "--range-batch") && i + 1 < argc)
            range_batch = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "未知选项: %s\n", a);
            usage(argv[0]);
            return 1;
        }
    }

    if (B == 0)
        B = 1;
    if (reps == 0)
        reps = 1;
    rng_state = seed;
    mkdir(outdir, 0755);

    if (cpu >= 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(cpu, &set);
        if (sched_setaffinity(0, sizeof(set), &set) == 0)
            printf("[CPU] 已绑定到 CPU %d\n", cpu);
        else
            printf("[CPU] 绑定 CPU %d 失败（忽略）\n", cpu);
    }

    calibrate_tsc();

    /* =================== 查找延迟 CDF / 统计 =================== */

    if (!sweep && !strcmp(mode, "lookup")) {
        int do_random = strcmp(pattern, "sequential") != 0;
        int do_seq = strcmp(pattern, "random") != 0;

        uint32_t R = M / B; /* 每 rep 批数 = 每 rep CDF 样本数 */
        if (R == 0)
            R = 1;
        uint32_t total = R * B;      /* 每 rep 实际总查找次数 */
        uint32_t R_total = R * reps; /* reps 次重复池化后的样本数 */
        uint32_t total_all = total * reps;

        uint64_t *cycles = malloc((size_t)R_total * sizeof(uint64_t));
        uint32_t *psns = malloc((size_t)total_all * sizeof(uint32_t));
        struct packet *pkts = malloc((size_t)N * sizeof(struct packet));
        if (!cycles || !psns || !pkts) {
            fprintf(stderr, "内存分配失败\n");
            return 1;
        }

        for (uint32_t i = 0; i < N; i++) {
            pkts[i].psn = i;
            pkts[i].len = PAYLOAD_LEN;
            pkts[i].stamp = 0;
            memset(pkts[i].data, (int)i, PAYLOAD_LEN);
        }

        printf("\n参数: N=%u, 总查找/rep=%u, 批大小 B=%u, 批数 R=%u, reps=%u, "
               "模式=%s, 种子=0x%08X\n",
               N, total, B, R, reps, mode, seed);
        printf("（延迟 = 每包平均周期，批量计时摊薄计时开销）\n\n");

        printf("%-14s %-11s %12s %12s %12s %12s %12s %12s %12s\n", "method",
               "pattern", "min", "p50", "p90", "p99", "p99.9", "max", "mean");
        printf("---------------------------------------------------------------"
               "-------------------------------\n");

        FILE *sum = fopen_join(outdir, "summary.csv", "w");
        fprintf(sum, "method,pattern,min_ns,p50_ns,p90_ns,p99_ns,p999_ns,max_"
                     "ns,mean_ns\n");

        const char *pats[2] = {"random", "sequential"};
        int use[2] = {do_random, do_seq};

        for (method_t m = 0; m < M_COUNT; m++) {
            for (int pi = 0; pi < 2; pi++) {
                if (!use[pi])
                    continue;
                const char *pat = pats[pi];
                int sequential = (pi == 1);

                gen_psns(psns, total_all, 0, N, sequential);

                struct cache c;
                cache_init(&c, m, N);
                for (uint32_t i = 0; i < N; i++)
                    cache_insert(&c, pkts[i].psn, &pkts[i]);

                for (uint32_t r = 0; r < reps; r++)
                    bench_lookup(&c, psns + (size_t)r * total, R, B,
                                 cycles + (size_t)r * R);
                qsort(cycles, R_total, sizeof(uint64_t), cmp_u64);
                latency_stats s = stats_from_sorted(cycles, R_total);

                print_stats_row(m, pat, &s);
                fprintf(sum, "%s,%s,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f\n",
                        method_name[m], pat, s.min_ns, s.p50_ns, s.p90_ns,
                        s.p99_ns, s.p999_ns, s.max_ns, s.mean_ns);

                char fname[256];
                snprintf(fname, sizeof fname, "%s/cdf_%s_%s.csv", outdir,
                         method_name[m], pat);
                FILE *fp = fopen(fname, "w");
                fprintf(fp, "latency_ns\n");
                for (uint32_t i = 0; i < R_total; i++)
                    fprintf(fp, "%.2f\n", (double)cycles[i] / cycles_per_ns);
                fclose(fp);

                cache_destroy(&c);
            }
        }
        fclose(sum);
        free(cycles);
        free(psns);
        free(pkts);
        printf("\n[OK] 结果已写入 %s/ (cdf_*.csv, summary.csv)\n", outdir);
    }

    /* =================== 按序提取（范围重传） =================== */

    else if (!strcmp(mode, "range")) {
        uint32_t R = range_num / range_batch; /* 批数 = 样本数 */
        if (R == 0)
            R = 1;
        uint32_t total_ranges = R * range_batch;

        struct packet *pkts = malloc((size_t)N * sizeof(struct packet));
        struct packet **out =
            malloc((size_t)range_len * sizeof(struct packet *));
        uint32_t *starts = malloc((size_t)total_ranges * sizeof(uint32_t));
        uint64_t *per_pkt = malloc((size_t)R * sizeof(uint64_t));
        if (!pkts || !out || !starts || !per_pkt) {
            fprintf(stderr, "内存分配失败\n");
            return 1;
        }

        for (uint32_t i = 0; i < N; i++) {
            pkts[i].psn = i;
            pkts[i].len = PAYLOAD_LEN;
            pkts[i].stamp = 0;
            memset(pkts[i].data, (int)i, PAYLOAD_LEN);
        }
        for (uint32_t i = 0; i < total_ranges; i++)
            starts[i] = xorshift32() % (N - range_len);

        printf("\n[range] N=%u, 区间总数=%u, 每区间长度=%u, 每批区间=%u, "
               "批数=%u\n",
               N, total_ranges, range_len, range_batch, R);
        printf("（延迟 = 每包平均周期，批量计时）\n");
        printf("%-14s %12s %12s %12s %12s %12s\n", "method", "min/pkt",
               "p50/pkt", "p90/pkt", "p99/pkt", "mean/pkt");
        printf("---------------------------------------------------------------"
               "-------\n");

        FILE *sum = fopen_join(outdir, "range_summary.csv", "w");
        fprintf(sum, "method,min_ns,p50_ns,p90_ns,p99_ns,mean_ns\n");

        for (method_t m = 0; m < M_COUNT; m++) {
            struct cache c;
            cache_init(&c, m, N);
            for (uint32_t i = 0; i < N; i++)
                cache_insert(&c, pkts[i].psn, &pkts[i]);

            /* 预热 */
            for (uint32_t r = 0; r < 200; r++)
                cache_extract(&c, starts[r % total_ranges], range_len, out);

            for (uint32_t r = 0; r < R; r++) {
                uint64_t t0 = rdtsc_raw();
                uint64_t found_total = 0;
                for (uint32_t j = 0; j < range_batch; j++)
                    found_total += cache_extract(
                        &c, starts[r * range_batch + j], range_len, out);
                uint64_t t1 = rdtsc_raw();
                per_pkt[r] = (t1 - t0) / ((uint64_t)range_batch *
                                          range_len); /* 每包平均周期 */
                (void)found_total;
            }

            qsort(per_pkt, R, sizeof(uint64_t), cmp_u64);
            latency_stats s;
            s.min_ns = (double)per_pkt[0] / cycles_per_ns;
            s.p50_ns = (double)per_pkt[R / 2] / cycles_per_ns;
            s.p90_ns = (double)per_pkt[(uint64_t)R * 9 / 10] / cycles_per_ns;
            s.p99_ns = (double)per_pkt[(uint64_t)R * 99 / 100] / cycles_per_ns;
            uint64_t sum_c = 0;
            for (uint32_t r = 0; r < R; r++)
                sum_c += per_pkt[r];
            s.mean_ns = (double)sum_c / R / cycles_per_ns;

            char mn[32], p50[32], p90[32], p99[32], mean[32];
            fmt_ns(s.min_ns, mn, sizeof mn);
            fmt_ns(s.p50_ns, p50, sizeof p50);
            fmt_ns(s.p90_ns, p90, sizeof p90);
            fmt_ns(s.p99_ns, p99, sizeof p99);
            fmt_ns(s.mean_ns, mean, sizeof mean);
            printf("%-14s %12s %12s %12s %12s %12s\n", method_name[m], mn, p50,
                   p90, p99, mean);
            fprintf(sum, "%s,%.2f,%.2f,%.2f,%.2f,%.2f\n", method_name[m],
                    s.min_ns, s.p50_ns, s.p90_ns, s.p99_ns, s.mean_ns);

            cache_destroy(&c);
        }
        fclose(sum);
        free(pkts);
        free(out);
        free(starts);
        free(per_pkt);
        printf("\n[OK] 结果已写入 %s/range_summary.csv\n", outdir);
    }

    /* =================== 占用扫描（复杂度曲线） =================== */

    else if (sweep) {
        uint32_t n_list[] = {512, 1024, 2048, 4096, 8192, RING_SIZE};
        int n_count = (int)(sizeof n_list / sizeof n_list[0]);
        uint32_t R = M / B;
        if (R == 0)
            R = 1;
        uint32_t total = R * B;

        FILE *sc = fopen_join(outdir, "scaling.csv", "w");
        fprintf(sc, "method,N,median_ns,median_std\n");

        printf("\n[scaling] 总查找/rep=%u, 批大小 B=%u, 批数 R=%u, reps=%u\n",
               total, B, R, reps);
        printf("%-14s", "N");
        for (method_t m = 0; m < M_COUNT; m++)
            printf("%14s", method_name[m]);
        printf("\n");

        uint64_t *cycles = malloc((size_t)R * sizeof(uint64_t));
        uint32_t *psns = malloc((size_t)total * sizeof(uint32_t));
        double *meds = malloc((size_t)reps * sizeof(double));
        if (!cycles || !psns || !meds) {
            fprintf(stderr, "内存分配失败\n");
            return 1;
        }

        for (int k = 0; k < n_count; k++) {
            uint32_t nn = n_list[k];
            struct packet *pkts = malloc((size_t)nn * sizeof(struct packet));
            for (uint32_t i = 0; i < nn; i++) {
                pkts[i].psn = i;
                pkts[i].len = PAYLOAD_LEN;
            }

            printf("%-14u", nn);
            for (method_t m = 0; m < M_COUNT; m++) {
                for (uint32_t r = 0; r < reps; r++) {
                    gen_psns(psns, total, 0, nn, 0); /* 随机，每 rep 不同 */
                    struct cache c;
                    cache_init(&c, m, nn);
                    for (uint32_t i = 0; i < nn; i++)
                        cache_insert(&c, pkts[i].psn, &pkts[i]);
                    bench_lookup(&c, psns, R, B, cycles);
                    qsort(cycles, R, sizeof(uint64_t), cmp_u64);
                    meds[r] = (double)cycles[R / 2] / cycles_per_ns;
                    cache_destroy(&c);
                }
                double med = mean_d(meds, reps);
                double sd = stddev_d(meds, reps, med);
                printf("%14.1f", med);
                fprintf(sc, "%s,%u,%.2f,%.4f\n", method_name[m], nn, med, sd);
            }
            printf("\n");
            free(pkts);
        }
        free(cycles);
        free(psns);
        free(meds);
        fclose(sc);
        printf("\n[OK] 结果已写入 %s/scaling.csv\n", outdir);
    }

    return 0;
}

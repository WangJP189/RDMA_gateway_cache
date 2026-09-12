/*
 * behavior_bench.c — 网关缓存「存/取行为」跨方法对比基准（任务 A–E + 分配行为）
 * -----------------------------------------------------------------------------
 * 6 种缓存方法（统一接口 store / retrieve / lose，统一计数器）：
 *
 *   fifo          到达顺序队列（传统"底层简单、上层复杂"）
 *   chained_hash  链式哈希（O(1) 均摊、但无序）
 *   balanced_tree AVL 树（O(log n)、按键有序）
 *   psn_fixed     PSN 映射 + 固定 5KB 块（现状）
 *   psn_dynamic   PSN 映射 + 按包 malloc 动态块（上一版）
 *   psn_tiered    PSN 映射 + 档位标记数组 + 分档环形数组（本文）
 *
 * 统一计数器：
 *   n_cmp      PSN 比较次数（"上层整理"工作量的直接度量；PSN 方法恒为 0）
 *   n_malloc   malloc 调用次数（分配器开销的度量）
 *
 * 任务：
 *   A  顺序存储（store 延迟 + 正确性）
 *   B  乱序存储（store 延迟）→ 按序取回（retrieve 延迟 + PSN 比较次数 + 正确性）
 *   C  随机丢包（命中取回 / 丢包判定延迟 + 判定准确率）
 *   D  突发丢包（同上）
 *   E  丢包率扫描（丢包判定延迟 + 准确率 vs 丢包率 0%~10%）
 *   附  覆盖写稳态 malloc 次数（仅 PSN 三变体：对比分档池复用）
 *
 * 计时：rdtsc 批量（每批 B 次摊薄 VM 计时噪声），calibrate_tsc 标定 TSC→ns。
 * 随机：xorshift32 固定种子，可复现。
 *
 * 用法: ./behavior_bench [-n N] [-l L] [-B B] [-R R] [-p P] [-d M] [-s S] [-o DIR]
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#define RING_SIZE 10240
#define MAX_L 4096
#define MEM_BLOCK_SIZE 5120 /* psn_fixed 的固定块大小（现状） */
#define TIER_COUNT 5
#define TIER_NONE 0xFF
#define HASH_BITS 14
#define HASH_BUCKETS (1u << HASH_BITS) /* 16384 桶，N=10240 时负载 ~0.6 */
#define FILLS 3 /* store 延迟的独立填充轮数（样本数 = FILLS * N / B） */
#define SZ (FILLS * RING_SIZE + 512)

/* ---------------- 公共：内存块头 ---------------- */
struct mem_block_header {
    int data_len;
    uint64_t recv_stamp;
    uint32_t psn;
};

/* ---------------- rdtsc 计时 ---------------- */
static inline uint64_t rdtsc_raw(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static double tsc_per_ns = 1.0;

static void calibrate_tsc(void)
{
    struct timespec a, b;
    uint64_t t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &a);
    t0 = rdtsc_raw();
    usleep(20000);
    t1 = rdtsc_raw();
    clock_gettime(CLOCK_MONOTONIC, &b);
    double ns = (double)(b.tv_sec - a.tv_sec) * 1e9 +
                (double)(b.tv_nsec - a.tv_nsec);
    if (ns > 0)
        tsc_per_ns = (double)(t1 - t0) / ns;
}

static inline double to_ns(uint64_t ticks)
{
    return (double)ticks / tsc_per_ns;
}

/* ---------------- xorshift32 固定种子随机 ---------------- */
static uint32_t rng_state = 0x9E3779B9;

static inline uint32_t xorshift32(void)
{
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    return x;
}

static void make_perm(uint32_t *p, int n)
{
    for (int i = 0; i < n; i++)
        p[i] = (uint32_t)i;
    for (int i = n - 1; i > 0; i--) {
        int j = (int)(xorshift32() % (uint32_t)(i + 1));
        uint32_t t = p[i];
        p[i] = p[j];
        p[j] = t;
    }
}

/*
 * 分配器预热：先 malloc/free 一批同尺寸块，让 glibc 空闲链表有存货、
 * 堆页已被触碰。否则计时阶段首次触碰堆页的缺页中断（每 4KB 一次）会
 * 污染 store 延迟测量——网关是长驻进程，稳态下不该付这笔一次性成本。
 */
static void warm_allocator(size_t size, int count)
{
    void **keep = malloc((size_t)count * sizeof(void *));
    if (!keep)
        return;
    for (int i = 0; i < count; i++)
        keep[i] = malloc(size);
    for (int i = 0; i < count; i++)
        free(keep[i]); /* 回到 glibc 空闲链表，供计时阶段的 malloc 弹出 */
    free(keep);
}

/* ---------------- 方法统一接口 ---------------- */
struct b_cache;

struct b_ops {
    void (*store)(struct b_cache *c, uint32_t psn,
                  const unsigned char *payload, int len);
    struct mem_block_header *(*retrieve)(struct b_cache *c, uint32_t psn);
    void (*lose)(struct b_cache *c, uint32_t psn); /* 移除（丢包模拟） */
    void (*destroy)(struct b_cache *c);
};

struct b_cache {
    const char *name;
    struct b_ops ops;
    uint64_t n_cmp;    /* PSN 比较次数 */
    uint64_t n_malloc; /* malloc 次数 */
};

/* ================= FIFO：到达顺序队列 ================= */
struct fifo_node {
    struct fifo_node *next;
    struct mem_block_header hdr;
    unsigned char data[];
};

struct fifo_cache {
    struct b_cache base;
    struct fifo_node *head, *tail;
};

static void fifo_store(struct b_cache *c, uint32_t psn,
                       const unsigned char *p, int len)
{
    struct fifo_cache *f = (struct fifo_cache *)c;
    struct fifo_node *n = malloc(sizeof(*n) + (size_t)len);
    c->n_malloc++;
    n->next = NULL;
    n->hdr.psn = psn;
    n->hdr.data_len = len;
    n->hdr.recv_stamp = 0;
    memcpy(n->data, p, (size_t)len);
    if (f->tail)
        f->tail->next = n;
    else
        f->head = n;
    f->tail = n;
}

static struct mem_block_header *fifo_retrieve(struct b_cache *c, uint32_t psn)
{
    struct fifo_cache *f = (struct fifo_cache *)c;
    for (struct fifo_node *n = f->head; n; n = n->next) {
        c->n_cmp++;
        if (n->hdr.psn == psn)
            return &n->hdr;
    }
    return NULL;
}

static void fifo_lose(struct b_cache *c, uint32_t psn)
{
    struct fifo_cache *f = (struct fifo_cache *)c;
    struct fifo_node *prev = NULL;
    for (struct fifo_node *n = f->head; n; n = n->next) {
        c->n_cmp++;
        if (n->hdr.psn == psn) {
            if (prev)
                prev->next = n->next;
            else
                f->head = n->next;
            if (f->tail == n)
                f->tail = prev;
            free(n);
            return;
        }
        prev = n;
    }
}

static void fifo_destroy(struct b_cache *c)
{
    struct fifo_cache *f = (struct fifo_cache *)c;
    struct fifo_node *n = f->head;
    while (n) {
        struct fifo_node *nx = n->next;
        free(n);
        n = nx;
    }
    free(f);
}

static struct b_cache *make_fifo(int N)
{
    (void)N;
    struct fifo_cache *f = calloc(1, sizeof(*f));
    f->base.name = "fifo";
    f->base.ops.store = fifo_store;
    f->base.ops.retrieve = fifo_retrieve;
    f->base.ops.lose = fifo_lose;
    f->base.ops.destroy = fifo_destroy;
    return &f->base;
}

/* ================= 链式哈希 ================= */
struct hash_node {
    struct hash_node *next;
    struct mem_block_header hdr;
    unsigned char data[];
};

struct hash_cache {
    struct b_cache base;
    struct hash_node **bkt;
    int nb;
};

static inline uint32_t hash_idx(uint32_t psn)
{
    return (psn * 2654435761u) >> (32 - HASH_BITS);
}

static void hash_store(struct b_cache *c, uint32_t psn,
                       const unsigned char *p, int len)
{
    struct hash_cache *h = (struct hash_cache *)c;
    struct hash_node *n = malloc(sizeof(*n) + (size_t)len);
    c->n_malloc++;
    n->hdr.psn = psn;
    n->hdr.data_len = len;
    n->hdr.recv_stamp = 0;
    memcpy(n->data, p, (size_t)len);
    uint32_t b = hash_idx(psn);
    n->next = h->bkt[b];
    h->bkt[b] = n;
}

static struct mem_block_header *hash_retrieve(struct b_cache *c, uint32_t psn)
{
    struct hash_cache *h = (struct hash_cache *)c;
    for (struct hash_node *n = h->bkt[hash_idx(psn)]; n; n = n->next) {
        c->n_cmp++;
        if (n->hdr.psn == psn)
            return &n->hdr;
    }
    return NULL;
}

static void hash_lose(struct b_cache *c, uint32_t psn)
{
    struct hash_cache *h = (struct hash_cache *)c;
    uint32_t b = hash_idx(psn);
    struct hash_node *prev = NULL;
    for (struct hash_node *n = h->bkt[b]; n; n = n->next) {
        c->n_cmp++;
        if (n->hdr.psn == psn) {
            if (prev)
                prev->next = n->next;
            else
                h->bkt[b] = n->next;
            free(n);
            return;
        }
        prev = n;
    }
}

static void hash_destroy(struct b_cache *c)
{
    struct hash_cache *h = (struct hash_cache *)c;
    for (int b = 0; b < h->nb; b++) {
        struct hash_node *n = h->bkt[b];
        while (n) {
            struct hash_node *nx = n->next;
            free(n);
            n = nx;
        }
    }
    free(h->bkt);
    free(h);
}

static struct b_cache *make_hash(int N)
{
    (void)N;
    struct hash_cache *h = calloc(1, sizeof(*h));
    h->base.name = "chained_hash";
    h->base.ops.store = hash_store;
    h->base.ops.retrieve = hash_retrieve;
    h->base.ops.lose = hash_lose;
    h->base.ops.destroy = hash_destroy;
    h->nb = HASH_BUCKETS;
    h->bkt = calloc((size_t)h->nb, sizeof(struct hash_node *));
    return &h->base;
}

/* ================= AVL 平衡树 ================= */
struct avl_node {
    struct avl_node *l, *r;
    int h;
    struct mem_block_header hdr;
    unsigned char data[];
};

struct tree_cache {
    struct b_cache base;
    struct avl_node *root;
};

static inline int avl_h(struct avl_node *n)
{
    return n ? n->h : 0;
}

static inline void avl_fix(struct avl_node *n)
{
    int hl = avl_h(n->l), hr = avl_h(n->r);
    n->h = 1 + (hl > hr ? hl : hr);
}

static inline int avl_bf(struct avl_node *n)
{
    return avl_h(n->l) - avl_h(n->r);
}

static struct avl_node *rot_r(struct avl_node *y)
{
    struct avl_node *x = y->l;
    y->l = x->r;
    x->r = y;
    avl_fix(y);
    avl_fix(x);
    return x;
}

static struct avl_node *rot_l(struct avl_node *x)
{
    struct avl_node *y = x->r;
    x->r = y->l;
    y->l = x;
    avl_fix(x);
    avl_fix(y);
    return y;
}

static struct avl_node *avl_balance(struct avl_node *n)
{
    avl_fix(n);
    int bf = avl_bf(n);
    if (bf > 1) {
        if (avl_bf(n->l) < 0)
            n->l = rot_l(n->l);
        return rot_r(n);
    }
    if (bf < -1) {
        if (avl_bf(n->r) > 0)
            n->r = rot_r(n->r);
        return rot_l(n);
    }
    return n;
}

static struct avl_node *avl_insert(struct b_cache *c, struct avl_node *t,
                                   uint32_t psn, int len,
                                   const unsigned char *p)
{
    if (!t) {
        struct avl_node *n = malloc(sizeof(*n) + (size_t)len);
        c->n_malloc++;
        n->l = n->r = NULL;
        n->h = 1;
        n->hdr.psn = psn;
        n->hdr.data_len = len;
        n->hdr.recv_stamp = 0;
        memcpy(n->data, p, (size_t)len);
        return n;
    }
    c->n_cmp++;
    if (psn < t->hdr.psn)
        t->l = avl_insert(c, t->l, psn, len, p);
    else
        t->r = avl_insert(c, t->r, psn, len, p);
    return avl_balance(t);
}

static struct avl_node *avl_erase(struct b_cache *c, struct avl_node *t,
                                  uint32_t psn, int *found)
{
    if (!t)
        return NULL;
    c->n_cmp++;
    if (psn == t->hdr.psn) {
        *found = 1;
        if (!t->l || !t->r) {
            struct avl_node *ch = t->l ? t->l : t->r;
            free(t);
            return ch;
        }
        /* 两个孩子：取右子树最小者顶替 */
        struct avl_node *m = t->r;
        while (m->l) {
            m = m->l;
            c->n_cmp++;
        }
        t->hdr = m->hdr;
        memcpy(t->data, m->data, (size_t)t->hdr.data_len);
        t->r = avl_erase(c, t->r, m->hdr.psn, found);
        return avl_balance(t);
    }
    if (psn < t->hdr.psn)
        t->l = avl_erase(c, t->l, psn, found);
    else
        t->r = avl_erase(c, t->r, psn, found);
    return avl_balance(t);
}

static void tree_store(struct b_cache *c, uint32_t psn,
                       const unsigned char *p, int len)
{
    struct tree_cache *t = (struct tree_cache *)c;
    t->root = avl_insert(c, t->root, psn, len, p);
}

static struct mem_block_header *tree_retrieve(struct b_cache *c, uint32_t psn)
{
    struct tree_cache *t = (struct tree_cache *)c;
    struct avl_node *n = t->root;
    while (n) {
        c->n_cmp++;
        if (psn == n->hdr.psn)
            return &n->hdr;
        n = (psn < n->hdr.psn) ? n->l : n->r;
    }
    return NULL;
}

static void tree_lose(struct b_cache *c, uint32_t psn)
{
    struct tree_cache *t = (struct tree_cache *)c;
    int found = 0;
    t->root = avl_erase(c, t->root, psn, &found);
}

static void tree_free_nodes(struct avl_node *n)
{
    if (!n)
        return;
    tree_free_nodes(n->l);
    tree_free_nodes(n->r);
    free(n);
}

static void tree_destroy(struct b_cache *c)
{
    struct tree_cache *t = (struct tree_cache *)c;
    tree_free_nodes(t->root);
    free(t);
}

static struct b_cache *make_tree(int N)
{
    (void)N;
    struct tree_cache *t = calloc(1, sizeof(*t));
    t->base.name = "balanced_tree";
    t->base.ops.store = tree_store;
    t->base.ops.retrieve = tree_retrieve;
    t->base.ops.lose = tree_lose;
    t->base.ops.destroy = tree_destroy;
    return &t->base;
}

/* ================= PSN 映射三变体（单结构 + mode 切换） ================= */
enum { PS_MODE_FIXED = 0, PS_MODE_DYNAMIC = 1, PS_MODE_TIERED = 2 };

static const uint32_t tier_boundary[TIER_COUNT] = {256, 512, 1024, 2048, 4096};

struct psn_cache {
    struct b_cache base;
    int mode;
    int ring_len;
    uintptr_t *ring;            /* fixed/dynamic 单环 */
    uint8_t *mark;              /* tiered：档位标记数组 1B/槽 */
    uintptr_t *tring[TIER_COUNT]; /* tiered：分档环形数组 */
    void *pool[TIER_COUNT];     /* tiered：每档空闲块池 free-list */
};

static int classify_tier(int len)
{
    for (int t = 0; t < TIER_COUNT - 1; t++)
        if ((uint32_t)len <= tier_boundary[t])
            return t;
    return TIER_COUNT - 1;
}

static size_t tier_block_size(int tier)
{
    size_t need = sizeof(struct mem_block_header) + (size_t)tier_boundary[tier];
    return (need + 15) & ~(size_t)15;
}

static void psn_store(struct b_cache *c, uint32_t psn,
                      const unsigned char *p, int len)
{
    struct psn_cache *s = (struct psn_cache *)c;
    uint32_t slot = psn % (uint32_t)s->ring_len;
    unsigned char *blk;

    if (s->mode == PS_MODE_TIERED) {
        int tier = classify_tier(len);
        /* 覆盖写：旧块先回池（先释放再弹出，保证复用） */
        uint8_t old_t = s->mark[slot];
        if (old_t < TIER_COUNT) {
            unsigned char *old =
                (unsigned char *)(uintptr_t)s->tring[old_t][slot];
            if (old) {
                *(void **)old = s->pool[old_t];
                s->pool[old_t] = old;
            }
        }
        if (s->pool[tier]) {
            blk = s->pool[tier];
            s->pool[tier] = *(void **)blk;
        } else {
            blk = malloc(tier_block_size(tier));
            c->n_malloc++;
        }
        s->tring[tier][slot] = (uintptr_t)blk;
        s->mark[slot] = (uint8_t)tier;
    } else {
        size_t bs = (s->mode == PS_MODE_FIXED)
                        ? MEM_BLOCK_SIZE
                        : ((sizeof(struct mem_block_header) + (size_t)len +
                            15) &
                           ~(size_t)15);
        blk = malloc(bs);
        c->n_malloc++;
        unsigned char *old = (unsigned char *)(uintptr_t)s->ring[slot];
        if (old)
            free(old);
        s->ring[slot] = (uintptr_t)blk;
    }

    struct mem_block_header *h = (struct mem_block_header *)blk;
    h->psn = psn;
    h->data_len = len;
    h->recv_stamp = 0;
    memcpy((unsigned char *)(h + 1), p, (size_t)len);
}

static struct mem_block_header *psn_retrieve(struct b_cache *c, uint32_t psn)
{
    struct psn_cache *s = (struct psn_cache *)c;
    uint32_t slot = psn % (uint32_t)s->ring_len;
    if (s->mode == PS_MODE_TIERED) {
        uint8_t t = s->mark[slot];
        if (t >= TIER_COUNT)
            return NULL;
        return (struct mem_block_header *)(uintptr_t)s->tring[t][slot];
    }
    return (struct mem_block_header *)(uintptr_t)s->ring[slot];
}

static void psn_lose(struct b_cache *c, uint32_t psn)
{
    struct psn_cache *s = (struct psn_cache *)c;
    uint32_t slot = psn % (uint32_t)s->ring_len;
    if (s->mode == PS_MODE_TIERED) {
        uint8_t t = s->mark[slot];
        if (t >= TIER_COUNT)
            return;
        unsigned char *blk = (unsigned char *)(uintptr_t)s->tring[t][slot];
        if (blk) {
            *(void **)blk = s->pool[t];
            s->pool[t] = blk;
        }
        s->tring[t][slot] = 0;
        s->mark[slot] = TIER_NONE;
    } else {
        unsigned char *blk = (unsigned char *)(uintptr_t)s->ring[slot];
        if (blk)
            free(blk);
        s->ring[slot] = 0;
    }
}

static void psn_destroy(struct b_cache *c)
{
    struct psn_cache *s = (struct psn_cache *)c;
    if (s->mode == PS_MODE_TIERED) {
        for (int t = 0; t < TIER_COUNT; t++) {
            for (int i = 0; i < s->ring_len; i++) {
                unsigned char *blk =
                    (unsigned char *)(uintptr_t)s->tring[t][i];
                if (blk)
                    free(blk);
            }
            void *p = s->pool[t];
            while (p) {
                void *nx = *(void **)p;
                free(p);
                p = nx;
            }
            free(s->tring[t]);
        }
        free(s->mark);
    } else {
        for (int i = 0; i < s->ring_len; i++) {
            unsigned char *blk = (unsigned char *)(uintptr_t)s->ring[i];
            if (blk)
                free(blk);
        }
        free(s->ring);
    }
    free(s);
}

static struct b_cache *make_psn(int mode, int N)
{
    struct psn_cache *s = calloc(1, sizeof(*s));
    s->base.name = (mode == PS_MODE_FIXED)   ? "psn_fixed"
                   : (mode == PS_MODE_DYNAMIC) ? "psn_dynamic"
                                               : "psn_tiered";
    s->base.ops.store = psn_store;
    s->base.ops.retrieve = psn_retrieve;
    s->base.ops.lose = psn_lose;
    s->base.ops.destroy = psn_destroy;
    s->mode = mode;
    s->ring_len = N;
    if (mode == PS_MODE_TIERED) {
        s->mark = malloc((size_t)N);
        memset(s->mark, TIER_NONE, (size_t)N);
        for (int t = 0; t < TIER_COUNT; t++)
            s->tring[t] = calloc((size_t)N, sizeof(uintptr_t));
    } else {
        s->ring = calloc((size_t)N, sizeof(uintptr_t));
    }
    return &s->base;
}

static struct b_cache *make_psn_fixed(int N)
{
    return make_psn(PS_MODE_FIXED, N);
}
static struct b_cache *make_psn_dynamic(int N)
{
    return make_psn(PS_MODE_DYNAMIC, N);
}
static struct b_cache *make_psn_tiered(int N)
{
    return make_psn(PS_MODE_TIERED, N);
}

/* ---------------- payload 生成/校验 ----------------
 * 只在头部 64B 写 psn 派生内容（足够校验错误落槽），其余保持 0。
 * memcpy 仍拷贝完整 len——计时正确性来自 memcpy 的长度，而非内容。
 */
static void fill_payload(unsigned char *p, int len, uint32_t psn)
{
    uint64_t seed = (uint64_t)psn * 0x9E3779B97F4A7C15ULL;
    int m = len < 64 ? len : 64;
    for (int i = 0; i < m; i++)
        p[i] = (unsigned char)((seed >> ((i & 7) * 8)) & 0xFF);
}

static int check_payload(const unsigned char *p, int len, uint32_t psn)
{
    uint64_t seed = (uint64_t)psn * 0x9E3779B97F4A7C15ULL;
    int m = len < 64 ? len : 64;
    for (int i = 0; i < m; i++)
        if (p[i] != (unsigned char)((seed >> ((i & 7) * 8)) & 0xFF))
            return 0;
    return 1;
}

/* 每次 store 前把该 psn 的内容写入热 scratch，再让方法 memcpy——模拟
 * "NIC 刚 DMA 进来的包数据是热的"，避免 10MB 冷源污染 store 计时。 */
static inline void store_payload(struct b_cache *c, uint32_t psn,
                                 unsigned char *scratch, int L)
{
    fill_payload(scratch, L, psn);
    c->ops.store(c, psn, scratch, L);
}

/* ---------------- 统计 ---------------- */
static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static double pct(double *v, int n, double p)
{
    if (n <= 0)
        return 0.0;
    qsort(v, n, sizeof(double), cmp_double);
    int i = (int)(p / 100.0 * (double)(n - 1) + 0.5);
    if (i < 0)
        i = 0;
    if (i >= n)
        i = n - 1;
    return v[i];
}

static void stats4(double *v, int n, double *p50, double *p90, double *p99,
                   double *mean)
{
    double sum = 0.0;
    for (int i = 0; i < n; i++)
        sum += v[i];
    *p50 = pct(v, n, 50.0);
    *p90 = pct(v, n, 90.0);
    *p99 = pct(v, n, 99.0);
    *mean = n ? sum / n : 0.0;
}

/* ---------------- 结果汇总 ---------------- */
struct method_result {
    const char *name;
    double store_seq_p50, store_seq_p90, store_seq_p99, store_seq_mean;
    double store_ooo_p50, store_ooo_p90, store_ooo_p99, store_ooo_mean;
    double retr_ord_p50, retr_ord_p90, retr_ord_p99, retr_ord_mean;
    double found_p50, miss_p50, cmp_per, store_small_p50;
    int okA, nA, okB, nB;
    double accC, accD, alloc_per_store;
    int alloc_stores;
    uint64_t alloc_mallocs;
};

static volatile uint64_t g_sink;

static double S_seq[SZ], S_ooo[SZ], S_ord[SZ], S_found[SZ], S_miss[SZ],
    S_small[SZ];

static struct method_result run_method(
    const char *name, struct b_cache *(*make)(int), int N, int L, int B, int R,
    double loss_rate, int burst_len, const uint32_t *perm,
    unsigned char *scratch, size_t warm_size, FILE *fsum, FILE *fsweep,
    FILE *falloc, int do_alloc)
{
    struct method_result r;
    memset(&r, 0, sizeof(r));
    r.name = name;
    r.alloc_per_store = -1.0;

    const int nB = N / B; /* 每轮填充的批数 */

    /* 分配器预热：模拟网关长驻进程的稳态（计时前完成，不计入样本） */
    warm_allocator(warm_size, 2 * N);

    /* ---- 任务 A：顺序存储（延迟 + 正确性） ---- */
    int ns = 0;
    for (int f = 0; f < FILLS; f++) {
        struct b_cache *c = make(N);
        for (int k = 0; k < nB; k++) {
            uint64_t t0 = rdtsc_raw();
            for (int j = 0; j < B; j++) {
                uint32_t psn = (uint32_t)(k * B + j);
                store_payload(c, psn, scratch, L);
            }
            S_seq[ns++] = to_ns(rdtsc_raw() - t0) / B;
        }
        for (uint32_t psn = 0; psn < (uint32_t)N; psn++) {
            struct mem_block_header *h = c->ops.retrieve(c, psn);
            r.nA++;
            if (h && h->psn == psn && h->data_len == L &&
                check_payload((const unsigned char *)(h + 1), L, psn))
                r.okA++;
        }
        c->ops.destroy(c);
    }
    stats4(S_seq, ns, &r.store_seq_p50, &r.store_seq_p90, &r.store_seq_p99,
           &r.store_seq_mean);

    /* ---- 任务 B：乱序存储（延迟）→ 按序取回（延迟 + 比较次数 + 正确性） ---- */
    int no = 0;
    for (int f = 0; f < FILLS; f++) {
        struct b_cache *c = make(N);
        for (int k = 0; k < nB; k++) {
            uint64_t t0 = rdtsc_raw();
            for (int j = 0; j < B; j++) {
                uint32_t psn = perm[k * B + j];
                store_payload(c, psn, scratch, L);
            }
            S_ooo[no++] = to_ns(rdtsc_raw() - t0) / B;
        }
        if (f == FILLS - 1) {
            c->n_cmp = 0;
            int nr = 0;
            uint64_t acc = 0;
            for (int k = 0; k < nB; k++) {
                uint64_t t0 = rdtsc_raw();
                for (int j = 0; j < B; j++) {
                    uint32_t psn = (uint32_t)(k * B + j);
                    struct mem_block_header *h = c->ops.retrieve(c, psn);
                    acc += (uint64_t)(h ? h->psn : 1u);
                }
                S_ord[nr++] = to_ns(rdtsc_raw() - t0) / B;
            }
            g_sink ^= acc;
            r.cmp_per = (double)c->n_cmp / (double)N;
            for (uint32_t psn = 0; psn < (uint32_t)N; psn++) {
                struct mem_block_header *h = c->ops.retrieve(c, psn);
                r.nB++;
                if (h && h->psn == psn && h->data_len == L &&
                    check_payload((const unsigned char *)(h + 1), L, psn))
                    r.okB++;
            }
        }
        c->ops.destroy(c);
    }
    stats4(S_ooo, no, &r.store_ooo_p50, &r.store_ooo_p90, &r.store_ooo_p99,
           &r.store_ooo_mean);
    stats4(S_ord, nB, &r.retr_ord_p50, &r.retr_ord_p90, &r.retr_ord_p99,
           &r.retr_ord_mean);

    /* ---- 任务 C：随机丢包 ---- */
    {
        int nl = (int)(loss_rate * (double)N + 0.5);
        if (nl < 1)
            nl = 1;
        if (nl > N)
            nl = N;
        struct b_cache *c = make(N);
        for (uint32_t psn = 0; psn < (uint32_t)N; psn++)
            store_payload(c, psn, scratch, L); /* 未计时填充 */
        uint32_t *lost = malloc((size_t)N * sizeof(uint32_t));
        make_perm(lost, N);
        for (int i = 0; i < nl; i++)
            c->ops.lose(c, lost[i]);

        unsigned char *flag = calloc((size_t)N, 1);
        for (int i = 0; i < nl; i++)
            flag[lost[i]] = 1;
        uint32_t *surv = malloc((size_t)(N - nl) * sizeof(uint32_t));
        int nsv = 0;
        for (uint32_t i = 0; i < (uint32_t)N; i++)
            if (!flag[i])
                surv[nsv++] = i;

        /* 命中取回（随机幸存包） */
        int nf = 0, ok = 0, tot = 0;
        uint64_t acc = 0;
        for (int k = 0; k < R; k++) {
            uint64_t t0 = rdtsc_raw();
            for (int j = 0; j < B; j++) {
                uint32_t psn = surv[xorshift32() % (uint32_t)nsv];
                struct mem_block_header *h = c->ops.retrieve(c, psn);
                tot++;
                if (h && h->psn == psn)
                    ok++;
                acc += (uint64_t)(h ? h->psn : 1u);
            }
            S_found[nf++] = to_ns(rdtsc_raw() - t0) / B;
        }
        g_sink ^= acc;

        /* 丢包判定（检索被丢的包，应全部 miss） */
        int nm = 0;
        int total_m = nl > B ? nl : B;
        int packs_m = (total_m + B - 1) / B;
        for (int k = 0; k < packs_m; k++) {
            uint64_t t0 = rdtsc_raw();
            for (int j = 0; j < B; j++) {
                uint32_t psn = lost[(k * B + j) % nl];
                struct mem_block_header *h = c->ops.retrieve(c, psn);
                tot++;
                if (h == NULL)
                    ok++;
                acc += (uint64_t)(h ? h->psn : 1u);
            }
            S_miss[nm++] = to_ns(rdtsc_raw() - t0) / B;
        }
        g_sink ^= acc;
        r.found_p50 = pct(S_found, nf, 50.0);
        r.miss_p50 = pct(S_miss, nm, 50.0);
        r.accC = tot ? 100.0 * (double)ok / (double)tot : 100.0;

        free(lost);
        free(flag);
        free(surv);
        c->ops.destroy(c);
    }

    /* ---- 任务 D：突发丢包 ---- */
    {
        int M = burst_len;
        if (M < 1)
            M = 1;
        if (M > N)
            M = N;
        int a = (int)(xorshift32() % (uint32_t)(N - M + 1));
        struct b_cache *c = make(N);
        for (uint32_t psn = 0; psn < (uint32_t)N; psn++)
            store_payload(c, psn, scratch, L);
        for (int i = 0; i < M; i++)
            c->ops.lose(c, (uint32_t)(a + i));

        unsigned char *flag = calloc((size_t)N, 1);
        for (int i = 0; i < M; i++)
            flag[a + i] = 1;
        uint32_t *surv = malloc((size_t)(N - M) * sizeof(uint32_t));
        int nsv = 0;
        for (uint32_t i = 0; i < (uint32_t)N; i++)
            if (!flag[i])
                surv[nsv++] = i;

        int ok = 0, tot = 0;
        uint64_t acc = 0;
        for (int k = 0; k < R; k++) {
            uint64_t t0 = rdtsc_raw();
            for (int j = 0; j < B; j++) {
                uint32_t psn = surv[xorshift32() % (uint32_t)nsv];
                struct mem_block_header *h = c->ops.retrieve(c, psn);
                tot++;
                if (h && h->psn == psn)
                    ok++;
                acc += (uint64_t)(h ? h->psn : 1u);
            }
            S_found[k] = to_ns(rdtsc_raw() - t0) / B;
        }
        g_sink ^= acc;
        int nm = 0;
        int total_m = M > B ? M : B;
        int packs_m = (total_m + B - 1) / B;
        for (int k = 0; k < packs_m; k++) {
            uint64_t t0 = rdtsc_raw();
            for (int j = 0; j < B; j++) {
                uint32_t psn = (uint32_t)(a + (k * B + j) % M);
                struct mem_block_header *h = c->ops.retrieve(c, psn);
                tot++;
                if (h == NULL)
                    ok++;
                acc += (uint64_t)(h ? h->psn : 1u);
            }
            S_miss[nm++] = to_ns(rdtsc_raw() - t0) / B;
        }
        g_sink ^= acc;
        r.accD = tot ? 100.0 * (double)ok / (double)tot : 100.0;

        free(flag);
        free(surv);
        c->ops.destroy(c);
    }

    /* ---- 任务 E：丢包率扫描 ---- */
    {
        static const double rates[] = {0.0, 0.001, 0.005, 0.01,
                                       0.02, 0.05,  0.10};
        for (size_t ri = 0; ri < sizeof(rates) / sizeof(rates[0]); ri++) {
            double rate = rates[ri];
            int nl = (int)(rate * (double)N + 0.5);
            double miss_p50 = 0.0, accu = 100.0;
            if (nl > 0) {
                struct b_cache *c = make(N);
                for (uint32_t psn = 0; psn < (uint32_t)N; psn++)
                    store_payload(c, psn, scratch, L);
                uint32_t *lost = malloc((size_t)N * sizeof(uint32_t));
                make_perm(lost, N);
                for (int i = 0; i < nl; i++)
                    c->ops.lose(c, lost[i]);
                int nm = 0, ok = 0, tot = 0;
                uint64_t acc = 0;
                int total_m = nl > B ? nl : B;
                int packs_m = (total_m + B - 1) / B;
                for (int k = 0; k < packs_m; k++) {
                    uint64_t t0 = rdtsc_raw();
                    for (int j = 0; j < B; j++) {
                        uint32_t psn = lost[(k * B + j) % nl];
                        struct mem_block_header *h =
                            c->ops.retrieve(c, psn);
                        tot++;
                        if (h == NULL)
                            ok++;
                        acc += (uint64_t)(h ? h->psn : 1u);
                    }
                    S_miss[nm++] = to_ns(rdtsc_raw() - t0) / B;
                }
                g_sink ^= acc;
                miss_p50 = pct(S_miss, nm, 50.0);
                accu = tot ? 100.0 * (double)ok / (double)tot : 100.0;
                free(lost);
                c->ops.destroy(c);
            }
            fprintf(fsweep, "%.3f,%s,%.2f,%.2f\n", rate, name, miss_p50,
                    accu);
        }
    }

    /* ---- 附：store 控制面开销（小负载 64B，隔离分配/索引/指针，不含大 memcpy） ---- */
    {
        const int SL = 64; /* 最小 RDMA 控制包负载：memcpy 仅 1 cacheline，忽略 */
        struct b_cache *c = make(N);
        for (uint32_t psn = 0; psn < (uint32_t)N; psn++)
            store_payload(c, psn, scratch, SL); /* 预热：填满 + 页表 warm */
        int ns64 = 0;
        for (int k = 0; k < R; k++) {
            uint64_t t0 = rdtsc_raw();
            for (int j = 0; j < B; j++) {
                uint32_t psn = xorshift32() % (uint32_t)N;
                store_payload(c, psn, scratch, SL);
            }
            S_small[ns64++] = to_ns(rdtsc_raw() - t0) / B;
        }
        g_sink ^= (uint64_t)(uintptr_t)c->ops.retrieve(c,
                                                       xorshift32() % (uint32_t)N);
        r.store_small_p50 = pct(S_small, ns64, 50.0);
        c->ops.destroy(c);
    }

    /* ---- 附：覆盖写稳态 malloc 次数（仅 PSN 三变体） ---- */
    if (do_alloc) {
        struct b_cache *c = make(N);
        for (uint32_t psn = 0; psn < (uint32_t)N; psn++)
            store_payload(c, psn, scratch, L);
        c->n_malloc = 0;
        int O = R;
        uint64_t acc = 0;
        for (int k = 0; k < O; k++)
            for (int j = 0; j < B; j++) {
                uint32_t psn = xorshift32() % (uint32_t)N;
                store_payload(c, psn, scratch, L);
                acc += psn;
            }
        g_sink ^= acc;
        r.alloc_per_store = (double)c->n_malloc / ((double)O * (double)B);
        r.alloc_stores = O * B;
        r.alloc_mallocs = c->n_malloc;
        fprintf(falloc, "%s,%d,%" PRIu64 ",%.4f\n", name, O * B, c->n_malloc,
                r.alloc_per_store);
        c->ops.destroy(c);
    }

    /* ---- 打印本方法结果 ---- */
    printf("\n===== 方法：%s =====\n", name);
    printf("  store (顺序到达)    P50=%7.1f ns  P90=%7.1f  P99=%7.1f  mean=%7.1f\n",
           r.store_seq_p50, r.store_seq_p90, r.store_seq_p99,
           r.store_seq_mean);
    printf("  store (乱序到达)    P50=%7.1f ns  P90=%7.1f  P99=%7.1f  mean=%7.1f\n",
           r.store_ooo_p50, r.store_ooo_p90, r.store_ooo_p99,
           r.store_ooo_mean);
    printf("  retrieve (按序取回) P50=%7.1f ns  P90=%7.1f  P99=%7.1f  mean=%7.1f  （每取回平均 %.2f 次 PSN 比较）\n",
           r.retr_ord_p50, r.retr_ord_p90, r.retr_ord_p99, r.retr_ord_mean,
           r.cmp_per);
    printf("  store (控制面开销,64B) P50=%7.1f ns\n", r.store_small_p50);
    printf("  retrieve (命中取回) P50=%7.1f ns\n", r.found_p50);
    printf("  retrieve (丢包判定) P50=%7.1f ns\n", r.miss_p50);
    printf("  正确性：顺序 fill 取回 %d/%d；乱序 fill 按序取回 %d/%d；判定准确率 C=%.2f%% D=%.2f%%\n",
           r.okA, r.nA, r.okB, r.nB, r.accC, r.accD);

    /* ---- 写汇总 CSV ---- */
    fprintf(fsum,
            "%s,%.2f,%.2f,%.2f,%.2f,"
            "%.2f,%.2f,%.2f,%.2f,"
            "%.2f,%.2f,%.2f,%.2f,"
            "%.2f,%.2f,%.2f,%.4f,"
            "%d,%d,%d,%d,%.4f,%.4f,%.4f\n",
            name, r.store_seq_p50, r.store_seq_p90, r.store_seq_p99,
            r.store_seq_mean, r.store_ooo_p50, r.store_ooo_p90,
            r.store_ooo_p99, r.store_ooo_mean, r.retr_ord_p50,
            r.retr_ord_p90, r.retr_ord_p99, r.retr_ord_mean, r.found_p50,
            r.miss_p50, r.store_small_p50, r.cmp_per, r.okA, r.nA, r.okB,
            r.nB, r.accC, r.accD, r.alloc_per_store);

    return r;
}

/* ---------------- 入口 ---------------- */
static void usage(const char *prog)
{
    fprintf(stderr,
            "用法: %s [选项]\n"
            "  -n N   缓存规模/环长（默认 %d）\n"
            "  -l L   单包 payload 大小（默认 1024，<= %d）\n"
            "  -B B   计时批量（默认 128，N 需能被 B 整除）\n"
            "  -R R   命中取回轮数（默认 100）\n"
            "  -p P   任务 C 随机丢包率（默认 0.01）\n"
            "  -d M   任务 D 突发丢包长度（默认 512）\n"
            "  -s S   随机种子（默认 0x9E3779B9）\n"
            "  -o DIR 输出目录（默认 .）\n"
            "  -h     帮助\n",
            prog, RING_SIZE, MAX_L);
}

enum warm_kind { WARM_NODE, WARM_FIXED, WARM_DYNAMIC, WARM_TIERED };

struct method_desc {
    const char *name;
    struct b_cache *(*make)(int N);
    int do_alloc;
    enum warm_kind warm; /* 分配器预热块类型（该方法 store 时的 malloc 尺寸） */
};

static size_t dynamic_block_bytes(int len)
{
    return (sizeof(struct mem_block_header) + (size_t)len + 15) &
           ~(size_t)15;
}

static size_t method_warm_size(const struct method_desc *d, int L)
{
    switch (d->warm) {
    case WARM_NODE:
        return sizeof(struct fifo_node) + (size_t)L; /* 三类节点大小近似 */
    case WARM_FIXED:
        return MEM_BLOCK_SIZE;
    case WARM_DYNAMIC:
        return dynamic_block_bytes(L);
    case WARM_TIERED:
        return tier_block_size(classify_tier(L));
    }
    return 1024;
}

static const struct method_desc METHODS[] = {
    {"fifo", make_fifo, 0, WARM_NODE},
    {"chained_hash", make_hash, 0, WARM_NODE},
    {"balanced_tree", make_tree, 0, WARM_NODE},
    {"psn_fixed", make_psn_fixed, 1, WARM_FIXED},
    {"psn_dynamic", make_psn_dynamic, 1, WARM_DYNAMIC},
    {"psn_tiered", make_psn_tiered, 1, WARM_TIERED},
};

int main(int argc, char **argv)
{
    int N = RING_SIZE;
    int L = 1024;
    int B = 128;
    int R = 100;
    double loss_rate = 0.01;
    int burst_len = 512;
    const char *outdir = ".";

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-n") && i + 1 < argc)
            N = (int)strtol(argv[++i], NULL, 10);
        else if (!strcmp(a, "-l") && i + 1 < argc)
            L = (int)strtol(argv[++i], NULL, 10);
        else if (!strcmp(a, "-B") && i + 1 < argc)
            B = (int)strtol(argv[++i], NULL, 10);
        else if (!strcmp(a, "-R") && i + 1 < argc)
            R = (int)strtol(argv[++i], NULL, 10);
        else if (!strcmp(a, "-p") && i + 1 < argc)
            loss_rate = strtod(argv[++i], NULL);
        else if (!strcmp(a, "-d") && i + 1 < argc)
            burst_len = (int)strtol(argv[++i], NULL, 10);
        else if (!strcmp(a, "-s") && i + 1 < argc)
            rng_state = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(a, "-o") && i + 1 < argc)
            outdir = argv[++i];
        else {
            usage(argv[0]);
            return 1;
        }
    }
    if (N <= 0 || N > RING_SIZE) {
        fprintf(stderr, "[错误] -n 需在 1..%d 之间\n", RING_SIZE);
        return 1;
    }
    if (L <= 0 || L > MAX_L) {
        fprintf(stderr, "[错误] -l 需在 1..%d 之间\n", MAX_L);
        return 1;
    }
    if (B <= 0 || N % B != 0) {
        fprintf(stderr, "[错误] N=%d 需能被 B=%d 整除\n", N, B);
        return 1;
    }
    if (strcmp(outdir, ".") != 0)
        mkdir(outdir, 0755);

    calibrate_tsc();
    printf("参数：N=%d 包, L=%d B, 批大小 B=%d, 命中轮数 R=%d, 种子=0x%08X, "
           "任务C丢包率=%.1f%%, 任务D突发=%d\n",
           N, L, B, R, rng_state, loss_rate * 100.0, burst_len);
    printf("方法：fifo / chained_hash / balanced_tree / psn_fixed / "
           "psn_dynamic / psn_tiered\n");

    /* 公共数据：热 scratch（单块，store 前写入该 psn 内容）+ 乱序置换 */
    unsigned char *scratch = calloc((size_t)L, 1);
    if (!scratch) {
        fprintf(stderr, "[错误] scratch 内存分配失败\n");
        return 1;
    }
    uint32_t *perm = malloc((size_t)N * sizeof(uint32_t));
    make_perm(perm, N);

    /* 输出 CSV */
    char path[512];
    snprintf(path, sizeof(path), "%s/behavior_summary.csv", outdir);
    FILE *fsum = fopen(path, "w");
    snprintf(path, sizeof(path), "%s/behavior_sweep.csv", outdir);
    FILE *fsweep = fopen(path, "w");
    snprintf(path, sizeof(path), "%s/behavior_alloc.csv", outdir);
    FILE *falloc = fopen(path, "w");
    if (!fsum || !fsweep || !falloc) {
        fprintf(stderr, "[错误] 无法打开输出文件（目录 %s 存在吗？）\n",
                outdir);
        return 1;
    }
    fprintf(fsum,
            "method,store_seq_p50,store_seq_p90,store_seq_p99,store_seq_mean,"
            "store_ooo_p50,store_ooo_p90,store_ooo_p99,store_ooo_mean,"
            "retr_ord_p50,retr_ord_p90,retr_ord_p99,retr_ord_mean,"
            "found_p50,miss_p50,store_small_p50,cmp_per_retr,"
            "okA,nA,okB,nB,accC,accD,alloc_per_store\n");
    fprintf(fsweep, "rate,method,miss_p50,accuracy\n");
    fprintf(falloc, "method,overwrite_stores,mallocs,mallocs_per_store\n");

    struct method_result res[6];
    for (size_t mi = 0; mi < sizeof(METHODS) / sizeof(METHODS[0]); mi++) {
        printf("\n######## %s ########\n", METHODS[mi].name);
        res[mi] = run_method(METHODS[mi].name, METHODS[mi].make, N, L, B, R,
                             loss_rate, burst_len, perm, scratch,
                             method_warm_size(&METHODS[mi], L), fsum, fsweep,
                             falloc, METHODS[mi].do_alloc);
    }

    fclose(fsum);
    fclose(fsweep);
    fclose(falloc);

    /* ---- 总览表 ---- */
    printf("\n===== 总览：6 种方法 存/取处理延迟（ns，P50；端到端=store乱序+按序取回 mean） =====\n");
    printf("%-16s %10s %10s %10s %10s %10s %10s %12s %8s %12s\n", "方法",
           "store顺序", "store乱序", "按序取回", "命中取回", "丢包判定",
           "控制面store", "比较/取回", "准确率", "端到端/包");
    for (size_t mi = 0; mi < 6; mi++) {
        const struct method_result *r = &res[mi];
        double acc = (r->accC + r->accD) / 2.0;
        double e2e = r->store_ooo_mean + r->retr_ord_mean;
        printf("%-16s %10.1f %10.1f %10.1f %10.1f %10.1f %10.1f %12.2f "
               "%7.2f%% %12.1f\n",
               r->name, r->store_seq_p50, r->store_ooo_p50, r->retr_ord_p50,
               r->found_p50, r->miss_p50, r->store_small_p50, r->cmp_per, acc,
               e2e);
    }
    printf("\n===== 分配行为（覆盖写每 store 的 malloc 次数；越低越好） =====\n");
    printf("%-16s %12s %12s %14s\n", "方法", "覆盖写次数", "malloc 次数",
           "malloc/store");
    for (size_t mi = 0; mi < 6; mi++)
        if (res[mi].alloc_per_store >= 0.0)
            printf("%-16s %12d %12" PRIu64 " %14.4f\n", res[mi].name,
                   res[mi].alloc_stores, res[mi].alloc_mallocs,
                   res[mi].alloc_per_store);

    printf("\n[OK] 已写入 %s/behavior_summary.csv、behavior_sweep.csv、"
           "behavior_alloc.csv\n",
           outdir);
    printf("提示：运行 python3 ../paper_figures/plot_figures.py 生成论文组合图\n");

    free(scratch);
    free(perm);
    return 0;
}

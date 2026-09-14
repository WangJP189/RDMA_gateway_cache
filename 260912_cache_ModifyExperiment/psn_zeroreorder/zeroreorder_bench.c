/*
 * zeroreorder_bench.c — average PSN comparisons vs cache depth N (Experiment 3a)
 * ----------------------------------------------------------------------------
 * Measures, for four cache structures, the average number of PSN *equality*
 * comparisons performed inside a single retrieval, as the cache depth N grows
 * {512, 1024, 2048, 4096, 8192, 10240}:
 *
 *   fifo           arrival-order linked list   -> O(N) linear scan
 *   chained_hash   4096-bucket chained hash     -> ~1 + load/2, slow rise
 *   balanced_tree  AVL tree                     -> O(log N)
 *   psn_tiered     PSN-modulo tiered ring       -> 0 comparisons (direct index)
 *
 * Per (N, method) point: insert N out-of-order PSNs (shuffled), then run N
 * random queries (uniform over the inserted PSNs, with replacement), counting
 * PSN *equality* comparisons inside each retrieve (n_cmp),
 * avg_cmp = n_cmp / N, REPS=5 independent repeats -> mean + std.
 *
 * Only PSN equality comparisons (==) are counted; pointer / bound / ordering
 * tests are not. psn_tiered retrieve is a direct slot index (psn % ring_len):
 * it never compares a PSN, so its count is exactly 0.
 *
 * Random: xorshift32, seed=42 (distinct stream per N/rep). Reproducible.
 * This is NOT a timing benchmark: the metric is the comparison count.
 *
 * Data structures are reused verbatim from psn_behavior_bench/behavior_bench.c
 * (fifo / chained_hash / balanced_tree / psn_tiered), with the timing framework
 * dropped and the n_cmp counter retained.
 */

#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define RING_MAX 10240
#define MAX_L 4096
#define TIER_COUNT 6
#define TIER_NONE 0xFF
#define HASH_BITS 12
#define HASH_BUCKETS (1u << HASH_BITS) /* 4096 buckets, load up to 2.5 @N=10240 */
#define REPS 5                        /* independent repeats for error bars */

/* cache-depth sweep (matches Fig-1 lookup scaling for cross-comparison) */
static const int N_SWEEP[] = {512, 1024, 2048, 4096, 8192, 10240};
#define N_POINTS ((int)(sizeof(N_SWEEP) / sizeof(N_SWEEP[0])))

/* ---------------- common: memory block header ---------------- */
struct mem_block_header {
    int data_len;
    uint64_t recv_stamp;
    uint32_t psn;
};

/* ---------------- xorshift32 (fixed seed, reproducible) ---------------- */
static uint32_t rng_state = 42;

static inline uint32_t xorshift32(void)
{
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    return x;
}

/* Fisher-Yates shuffle -> random arrival / retrieval order (WAN reorder) */
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

/* ---------------- unified method interface ---------------- */
struct b_cache;

struct b_ops {
    void (*store)(struct b_cache *c, uint32_t psn,
                  const unsigned char *payload, int len);
    struct mem_block_header *(*retrieve)(struct b_cache *c, uint32_t psn);
    void (*destroy)(struct b_cache *c);
};

struct b_cache {
    const char *name;
    struct b_ops ops;
    uint64_t n_cmp; /* PSN equality comparisons */
};

/* ---------------- payload pattern (PSN-derived, used for correctness) ------ */
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
    for (int i = 0; i < m; i++) {
        unsigned char want = (unsigned char)((seed >> ((i & 7) * 8)) & 0xFF);
        if (p[i] != want)
            return 0;
    }
    return 1;
}

static inline void store_payload(struct b_cache *c, uint32_t psn,
                                 unsigned char *scratch, int L)
{
    fill_payload(scratch, L, psn);
    c->ops.store(c, psn, scratch, L);
}

/* ================= FIFO: arrival-order queue ================= */
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
    f->base.ops.destroy = fifo_destroy;
    return &f->base;
}

/* ================= Chained hash ================= */
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
    h->base.ops.destroy = hash_destroy;
    h->nb = HASH_BUCKETS;
    h->bkt = calloc((size_t)h->nb, sizeof(struct hash_node *));
    return &h->base;
}

/* ================= AVL balanced tree ================= */
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

static struct avl_node *avl_insert(struct avl_node *t, uint32_t psn, int len,
                                   const unsigned char *p)
{
    if (!t) {
        struct avl_node *n = malloc(sizeof(*n) + (size_t)len);
        n->l = n->r = NULL;
        n->h = 1;
        n->hdr.psn = psn;
        n->hdr.data_len = len;
        n->hdr.recv_stamp = 0;
        memcpy(n->data, p, (size_t)len);
        return n;
    }
    if (psn < t->hdr.psn)
        t->l = avl_insert(t->l, psn, len, p);
    else
        t->r = avl_insert(t->r, psn, len, p);
    return avl_balance(t);
}

static void tree_store(struct b_cache *c, uint32_t psn,
                       const unsigned char *p, int len)
{
    struct tree_cache *t = (struct tree_cache *)c;
    t->root = avl_insert(t->root, psn, len, p);
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
    t->base.ops.destroy = tree_destroy;
    return &t->base;
}

/* ================= PSN tiered mapping (ours) ================= */
/* RDMA-MTU size classes {256,512,1024,1536,2048,4096} B; the 1536 B tier
 * smooths the internal-fragmentation dip for common 1280-1500 B frames. */
static const uint32_t tier_boundary[TIER_COUNT] = {256, 512, 1024, 1536,
                                                   2048, 4096};

struct tiered_cache {
    struct b_cache base;
    int ring_len;
    uint8_t *mark;                  /* 1 B/slot tier marker */
    uintptr_t *ring[TIER_COUNT];    /* per-tier ring arrays */
    void *pool[TIER_COUNT];         /* per-tier free-list */
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

static void tiered_store(struct b_cache *c, uint32_t psn,
                         const unsigned char *p, int len)
{
    struct tiered_cache *s = (struct tiered_cache *)c;
    uint32_t slot = psn % (uint32_t)s->ring_len;
    int tier = classify_tier(len);
    uint8_t old_t = s->mark[slot];
    if (old_t < TIER_COUNT) {
        unsigned char *old = (unsigned char *)(uintptr_t)s->ring[old_t][slot];
        if (old) {
            *(void **)old = s->pool[old_t];
            s->pool[old_t] = old;
        }
    }
    unsigned char *blk;
    if (s->pool[tier]) {
        blk = s->pool[tier];
        s->pool[tier] = *(void **)blk;
    } else {
        blk = malloc(tier_block_size(tier));
    }
    s->ring[tier][slot] = (uintptr_t)blk;
    s->mark[slot] = (uint8_t)tier;
    struct mem_block_header *h = (struct mem_block_header *)blk;
    h->psn = psn;
    h->data_len = len;
    h->recv_stamp = 0;
    memcpy((unsigned char *)(h + 1), p, (size_t)len);
}

/* Direct slot index: no PSN comparison, so n_cmp stays 0. */
static struct mem_block_header *tiered_retrieve(struct b_cache *c, uint32_t psn)
{
    struct tiered_cache *s = (struct tiered_cache *)c;
    uint32_t slot = psn % (uint32_t)s->ring_len;
    uint8_t t = s->mark[slot];
    if (t >= TIER_COUNT)
        return NULL;
    return (struct mem_block_header *)(uintptr_t)s->ring[t][slot];
}

static void tiered_destroy(struct b_cache *c)
{
    struct tiered_cache *s = (struct tiered_cache *)c;
    for (int t = 0; t < TIER_COUNT; t++) {
        for (int i = 0; i < s->ring_len; i++) {
            unsigned char *blk = (unsigned char *)(uintptr_t)s->ring[t][i];
            if (blk)
                free(blk);
        }
        void *p = s->pool[t];
        while (p) {
            void *nx = *(void **)p;
            free(p);
            p = nx;
        }
        free(s->ring[t]);
    }
    free(s->mark);
    free(s);
}

static struct b_cache *make_psn_tiered(int N)
{
    struct tiered_cache *s = calloc(1, sizeof(*s));
    s->base.name = "psn_tiered";
    s->base.ops.store = tiered_store;
    s->base.ops.retrieve = tiered_retrieve;
    s->base.ops.destroy = tiered_destroy;
    s->ring_len = N;
    s->mark = malloc((size_t)N);
    memset(s->mark, TIER_NONE, (size_t)N);
    for (int t = 0; t < TIER_COUNT; t++)
        s->ring[t] = calloc((size_t)N, sizeof(uintptr_t));
    return &s->base;
}

/* ---------------- method table ---------------- */
struct method_desc {
    const char *name;
    struct b_cache *(*make)(int N);
};

static const struct method_desc METHODS[] = {
    {"fifo", make_fifo},
    {"chained_hash", make_hash},
    {"balanced_tree", make_tree},
    {"psn_tiered", make_psn_tiered},
};
#define NMETHODS ((int)(sizeof(METHODS) / sizeof(METHODS[0])))

/* ---------------- stats helpers ---------------- */
static double mean(const double *v, int n)
{
    double s = 0;
    for (int i = 0; i < n; i++)
        s += v[i];
    return n ? s / n : 0.0;
}

static double stddev(const double *v, int n, double m)
{
    double s = 0;
    for (int i = 0; i < n; i++) {
        double d = v[i] - m;
        s += d * d;
    }
    return n > 1 ? sqrt(s / (n - 1)) : 0.0;
}

static FILE *fopen_join(const char *dir, const char *name, const char *mode)
{
    char path[512];
    if (strcmp(dir, ".") == 0)
        snprintf(path, sizeof path, "%s", name);
    else
        snprintf(path, sizeof path, "%s/%s", dir, name);
    return fopen(path, mode);
}

/* ================= scaling driver =================
 * For every N in the sweep and every method, run REPS independent passes:
 *   - insert N PSNs in a shuffled order (out-of-order arrival),
 *   - run N random queries (uniform, with replacement), counting equality
 *     comparisons inside each retrieve,
 *   - avg_cmp = n_cmp / N.
 * Output one row method,N,avg_cmp,std_cmp per (N, method). */
static void run_scaling(int L, const char *outdir)
{
    FILE *f = fopen_join(outdir, "zeroreorder_scaling.csv", "w");
    if (!f) {
        fprintf(stderr, "[error] cannot open %s/zeroreorder_scaling.csv\n",
                outdir);
        return;
    }
    fprintf(f, "method,N,avg_cmp,std_cmp\n");

    uint32_t *perm = malloc((size_t)RING_MAX * sizeof(uint32_t));
    unsigned char *scratch = calloc((size_t)L, 1);
    int c_ok = 0, c_tot = 0;

    for (int ni = 0; ni < N_POINTS; ni++) {
        int N = N_SWEEP[ni];
        printf("\nN = %d\n", N);
        for (int mi = 0; mi < NMETHODS; mi++) {
            const struct method_desc *md = &METHODS[mi];
            double cmps[REPS];
            for (int rep = 0; rep < REPS; rep++) {
                struct b_cache *c = md->make(N);
                rng_state = 42 + (uint32_t)(ni * 100 + rep);
                make_perm(perm, N); /* shuffled arrival (multipath reorder) */
                for (int i = 0; i < N; i++)
                    store_payload(c, perm[i], scratch, L);
                c->n_cmp = 0;
                for (int q = 0; q < N; q++) {
                    uint32_t psn = xorshift32() % (uint32_t)N; /* random query */
                    struct mem_block_header *h = c->ops.retrieve(c, psn);
                    c_tot++;
                    if (h && h->psn == psn && h->data_len == L &&
                        check_payload((const unsigned char *)(h + 1), L, psn))
                        c_ok++;
                }
                cmps[rep] = (double)c->n_cmp / (double)N;
                c->ops.destroy(c);
            }
            double m = mean(cmps, REPS), s = stddev(cmps, REPS, m);
            fprintf(f, "%s,%d,%.6f,%.6f\n", md->name, N, m, s);
            printf("  %-16s avg_cmp = %10.4f +/- %.4f\n", md->name, m, s);
        }
    }
    fclose(f);
    free(perm);
    free(scratch);
    printf("\n[correctness] %d/%d retrieves returned correct PSN+len+payload\n",
           c_ok, c_tot);
    printf("[OK] wrote %s/zeroreorder_scaling.csv\n", outdir);
}

/* ---------------- entry ---------------- */
static void usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s [options]\n"
            "  -l L      payload size (default 64, <= %d)\n"
            "  -o DIR    output dir (default .)\n",
            prog, MAX_L);
}

int main(int argc, char **argv)
{
    int L = 64;
    const char *outdir = ".";

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-l") && i + 1 < argc)
            L = (int)strtol(argv[++i], NULL, 10);
        else if (!strcmp(a, "-o") && i + 1 < argc)
            outdir = argv[++i];
        else {
            usage(argv[0]);
            return 1;
        }
    }

    if (L <= 0 || L > MAX_L) {
        fprintf(stderr, "[error] -l must be in 1..%d\n", MAX_L);
        return 1;
    }
    if (strcmp(outdir, ".") != 0)
        mkdir(outdir, 0755);

    printf("zeroreorder scaling: methods = fifo / chained_hash / "
           "balanced_tree / psn_tiered\n");
    printf("params: L=%d B, reps=%d, seed=42, N = {512,1024,2048,4096,8192,"
           "10240}\n", L, REPS);
    printf("metric: average PSN *equality* comparisons per retrieval\n");

    run_scaling(L, outdir);

    printf("\n[done] output: %s/zeroreorder_scaling.csv\n", outdir);
    printf("       plot with: python3 plot_zeroreorder.py\n");
    return 0;
}

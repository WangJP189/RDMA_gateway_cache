/*
 * behavior_bench.c — gateway cache "store / retrieve behavior" benchmark
 * ---------------------------------------------------------------------------
 * Redesigned for the ICASSP-2027 paper: three focused experiments, each proving
 * one core claim, plus a store-latency sampler feeding the Fig-1(c) box plot.
 *
 *   6 cache methods (unified store / retrieve / lose, unified counters):
 *     fifo          arrival-order linked list   (traditional "simple bottom,
 *                                                  complex top": reorder at read)
 *     chained_hash  16384-bucket chained hash    (O(1) avg, but unordered)
 *     balanced_tree AVL tree                     (O(log n), key-ordered)
 *     psn_fixed     PSN map + fixed 5 KB block   (naive baseline)
 *     psn_dynamic   PSN map + per-packet malloc  (simple dynamic)
 *     psn_tiered    PSN map + tier-marker array + tiered ring arrays (ours)
 *
 *   Counters:  n_cmp    #PSN comparisons (the "reorder" work metric)
 *              n_malloc #malloc calls    (the allocator-churn metric)
 *
 *   Experiment A  zero-reorder  : out-of-order store, ordered retrieve,
 *                                 count comparisons -> zero_reorder.csv
 *   Experiment B  retrieve CDF   : ordered retrieve latency samples
 *                                 -> retrieve_samples.csv
 *   Experiment C  deterministic memory : N*10 overwrite stores, count malloc
 *                                 -> deterministic_mem.csv
 *   Store boxplot (Fig 1c)       : store latency samples @64B (isolating the
 *                                 data-structure overhead) -> store_samples.csv
 *
 * Timing: batched rdtsc (B ops per tick pair) to amortize VM rdtsc noise,
 * calibrated to ns. Random: xorshift32 with fixed seed (42), reproducible.
 * Every timing loop is measured in steady state (after a warm-up fill) so the
 * allocator is warm — a gateway is a long-lived process, not a cold start.
 */

#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define RING_SIZE 10240
#define MAX_L 4096
#define MEM_BLOCK_SIZE 5120 /* psn_fixed fixed block (status quo) */
#define TIER_COUNT 6        /* {256,512,1024,1536,2048,4096} */
#define TIER_NONE 0xFF
#define HASH_BITS 14
#define HASH_BUCKETS (1u << HASH_BITS) /* 16384 buckets, load ~0.6 @ N=10240 */
#define REPS 5                        /* error-bar repetitions */
#define MAX_SAMP 8192

/* ---------------- common: memory block header ---------------- */
struct mem_block_header {
    int data_len;
    uint64_t recv_stamp;
    uint32_t psn;
};

/* ---------------- rdtsc timing ---------------- */
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

/* Fisher-Yates shuffle -> random arrival order (WAN multipath reorder) */
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

/* warm the allocator so timed mallocs hit warm bins, not fresh pages */
static void warm_allocator(size_t size, int count)
{
    void **keep = malloc((size_t)count * sizeof(void *));
    if (!keep)
        return;
    for (int i = 0; i < count; i++)
        keep[i] = malloc(size);
    for (int i = 0; i < count; i++)
        free(keep[i]);
    free(keep);
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
    uint64_t n_cmp;
    uint64_t n_malloc;
};

/* ---------------- payload pattern (64-byte header pattern, PSN-derived) ----
 * The store source is a single hot scratch buffer re-filled per packet with a
 * PSN-derived 64-byte pattern. This keeps the payload copy cheap and avoids a
 * cold multi-MB source array dominating the store latency. */
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

/* ================= PSN mapping (3 variants) ================= */
enum { PS_MODE_FIXED = 0, PS_MODE_DYNAMIC = 1, PS_MODE_TIERED = 2 };

/* tier boundaries: the fixed RDMA MTU tiers {256,512,1024,2048,4096} plus an
 * intermediate 1536 B tier to smooth the internal-fragmentation dip for the
 * common 1280-1500 B (non-power-of-two) Ethernet-frame sizes. */
static const uint32_t tier_boundary[TIER_COUNT] = {256, 512, 1024, 1536,
                                                   2048, 4096};

struct psn_cache {
    struct b_cache base;
    int mode;
    int ring_len;
    uintptr_t *ring;             /* fixed/dynamic single ring */
    uint8_t *mark;               /* tiered: 1B/slot tier-marker array */
    uintptr_t *tring[TIER_COUNT];/* tiered: per-tier ring arrays */
    void *pool[TIER_COUNT];      /* tiered: per-tier free-list */
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

/* ---------------- method table ---------------- */
struct method_desc {
    const char *name;
    struct b_cache *(*make)(int N);
    int is_psn;
};

static const struct method_desc METHODS[] = {
    {"fifo", make_fifo, 0},
    {"chained_hash", make_hash, 0},
    {"balanced_tree", make_tree, 0},
    {"psn_fixed", make_psn_fixed, 1},
    {"psn_dynamic", make_psn_dynamic, 1},
    {"psn_tiered", make_psn_tiered, 1},
};
#define NMETHODS ((int)(sizeof(METHODS) / sizeof(METHODS[0])))

/* ---------------- stats helpers ---------------- */
static volatile uint64_t g_sink;

static int cmp_d(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static double pct_sorted(const double *v, int n, double p)
{
    if (n <= 0)
        return 0.0;
    int idx = (int)((double)n * p / 100.0);
    if (idx < 0)
        idx = 0;
    if (idx >= n)
        idx = n - 1;
    return v[idx];
}

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

/* ================= Experiment A: zero-reorder =================
 * Store N packets in random arrival order, retrieve in PSN order, count PSN
 * comparisons. Ours needs 0 comparisons (retrieve touches exactly the mapped
 * slot); FIFO scans ~N/2, tree ~log2 N, hash ~1. */
static void run_zero_reorder(int N, int L, const char *outdir)
{
    FILE *f = fopen_join(outdir, "zero_reorder.csv", "w");
    fprintf(f, "method,avg_cmp,std_cmp\n");
    uint32_t *perm = malloc((size_t)N * sizeof(uint32_t));
    unsigned char *scratch = calloc((size_t)L, 1);

    printf("\n===== Experiment A: zero-reorder (avg PSN comparisons per ordered "
           "retrieve) =====\n");
    for (int mi = 0; mi < NMETHODS; mi++) {
        const struct method_desc *md = &METHODS[mi];
        double cmps[REPS];
        int ok = 0, tot = 0;
        for (int rep = 0; rep < REPS; rep++) {
            struct b_cache *c = md->make(N);
            rng_state = 42 + (uint32_t)rep; /* distinct permutation per rep */
            make_perm(perm, N);
            for (int i = 0; i < N; i++)
                store_payload(c, perm[i], scratch, L);
            c->n_cmp = 0;
            for (uint32_t psn = 0; psn < (uint32_t)N; psn++) {
                struct mem_block_header *h = c->ops.retrieve(c, psn);
                tot++;
                if (h && h->psn == psn && h->data_len == L &&
                    check_payload((const unsigned char *)(h + 1), L, psn))
                    ok++;
            }
            cmps[rep] = (double)c->n_cmp / (double)N;
            c->ops.destroy(c);
        }
        double m = mean(cmps, REPS), s = stddev(cmps, REPS, m);
        fprintf(f, "%s,%.4f,%.4f\n", md->name, m, s);
        printf("  %-16s avg_cmp = %8.2f +/- %.3f   (correctness %d/%d)\n",
               md->name, m, s, ok, tot);
    }
    fclose(f);
    free(perm);
    free(scratch);
    printf("[OK] wrote %s/zero_reorder.csv\n", outdir);
}

/* ================= Experiment B: ordered-retrieve latency CDF ============
 * Same out-of-order store, then timed ordered retrieve (batched), pooled over
 * REPS passes. Latency is payload-independent (returns a pointer). */
static void run_retrieve_cdf(int N, int L, int B, const char *outdir)
{
    FILE *fs = fopen_join(outdir, "retrieve_samples.csv", "w");
    fprintf(fs, "method,latency_ns\n");
    uint32_t *perm = malloc((size_t)N * sizeof(uint32_t));
    unsigned char *scratch = calloc((size_t)L, 1);

    printf("\n===== Experiment B: ordered-retrieve latency CDF =====\n");
    int nB = N / B;
    for (int mi = 0; mi < NMETHODS; mi++) {
        const struct method_desc *md = &METHODS[mi];
        struct b_cache *c = md->make(N);
        rng_state = 42;
        make_perm(perm, N);
        for (int i = 0; i < N; i++)
            store_payload(c, perm[i], scratch, L);

        double samp[MAX_SAMP];
        int ns = 0;
        for (int rep = 0; rep < REPS; rep++) {
            for (int k = 0; k < nB; k++) {
                uint64_t t0 = rdtsc_raw();
                for (int j = 0; j < B; j++) {
                    uint32_t psn = (uint32_t)(k * B + j);
                    struct mem_block_header *h = c->ops.retrieve(c, psn);
                    g_sink ^= (uint64_t)(h ? h->psn : 1u);
                }
                uint64_t t1 = rdtsc_raw();
                double lat = to_ns(t1 - t0) / B;
                samp[ns++] = lat;
                fprintf(fs, "%s,%.2f\n", md->name, lat);
            }
        }
        qsort(samp, ns, sizeof(double), cmp_d);
        printf("  %-16s retrieve P50=%.2f P90=%.2f P99=%.2f ns (%d samples)\n",
               md->name, pct_sorted(samp, ns, 50), pct_sorted(samp, ns, 90),
               pct_sorted(samp, ns, 99), ns);
        c->ops.destroy(c);
    }
    fclose(fs);
    free(perm);
    free(scratch);
    printf("[OK] wrote %s/retrieve_samples.csv\n", outdir);
}

/* ================= Experiment C: deterministic memory ================
 * Long-run simulation: N*10 overwrite stores; count malloc per store. The
 * tiered pool recycles blocks (0 malloc); fixed/dynamic malloc every store. */
static void run_deterministic_mem(int N, int L, const char *outdir)
{
    FILE *f = fopen_join(outdir, "deterministic_mem.csv", "w");
    fprintf(f, "method,malloc_per_store,std\n");
    uint32_t *perm = malloc((size_t)N * sizeof(uint32_t));
    unsigned char *scratch = calloc((size_t)L, 1);
    long O = (long)N * 10; /* 102400 overwrite stores */

    printf("\n===== Experiment C: deterministic memory (malloc per overwrite "
           "store, N*10 ops) =====\n");
    for (int mi = 0; mi < NMETHODS; mi++) {
        const struct method_desc *md = &METHODS[mi];
        if (!md->is_psn)
            continue;
        double mps[REPS];
        for (int rep = 0; rep < REPS; rep++) {
            struct b_cache *c = md->make(N);
            rng_state = 42 + (uint32_t)rep;
            make_perm(perm, N);
            for (int i = 0; i < N; i++)
                store_payload(c, perm[i], scratch, L);
            c->n_malloc = 0;
            for (long i = 0; i < O; i++) {
                uint32_t psn = xorshift32() % (uint32_t)N;
                store_payload(c, psn, scratch, L);
            }
            mps[rep] = (double)c->n_malloc / (double)O;
            c->ops.destroy(c);
        }
        double m = mean(mps, REPS), s = stddev(mps, REPS, m);
        fprintf(f, "%s,%.4f,%.4f\n", md->name, m, s);
        printf("  %-16s malloc/store = %.4f +/- %.4f\n", md->name, m, s);
    }
    fclose(f);
    free(perm);
    free(scratch);
    printf("[OK] wrote %s/deterministic_mem.csv\n", outdir);
}

/* ================= Store-latency samples (Fig 1c box plot) ============
 * Steady-state store latency at a 64 B payload, isolating the data-structure
 * overhead (allocation + index + pointer write) from the payload copy. */
static void run_store_samples(int N, int SL, int B, int R, const char *outdir)
{
    FILE *fs = fopen_join(outdir, "store_samples.csv", "w");
    fprintf(fs, "method,latency_ns\n");
    uint32_t *perm = malloc((size_t)N * sizeof(uint32_t));
    unsigned char *scratch = calloc((size_t)SL, 1);

    printf("\n===== Store-latency samples (64B payload, steady state) =====\n");
    for (int mi = 0; mi < NMETHODS; mi++) {
        const struct method_desc *md = &METHODS[mi];
        struct b_cache *c = md->make(N);
        rng_state = 42;
        make_perm(perm, N);
        for (int i = 0; i < N; i++)
            store_payload(c, perm[i], scratch, SL); /* warm-up fill */

        double samp[MAX_SAMP];
        int ns = 0;
        for (int rep = 0; rep < REPS; rep++) {
            for (int k = 0; k < R; k++) {
                uint64_t t0 = rdtsc_raw();
                for (int j = 0; j < B; j++) {
                    uint32_t psn = xorshift32() % (uint32_t)N;
                    store_payload(c, psn, scratch, SL);
                }
                uint64_t t1 = rdtsc_raw();
                double lat = to_ns(t1 - t0) / B;
                samp[ns++] = lat;
                fprintf(fs, "%s,%.2f\n", md->name, lat);
            }
        }
        qsort(samp, ns, sizeof(double), cmp_d);
        printf("  %-16s store P25=%.2f P50=%.2f P75=%.2f P90=%.2f P99=%.2f ns\n",
               md->name, pct_sorted(samp, ns, 25), pct_sorted(samp, ns, 50),
               pct_sorted(samp, ns, 75), pct_sorted(samp, ns, 90),
               pct_sorted(samp, ns, 99));
        c->ops.destroy(c);
    }
    fclose(fs);
    free(perm);
    free(scratch);
    printf("[OK] wrote %s/store_samples.csv\n", outdir);
}

/* ================= PSN wraparound correctness =============== */
static void run_wraptest(int L)
{
    /* Two wraparound scenarios the ring mapping must survive:
     *  (1) ring-index wrap: PSNs spanning RING_SIZE, so ring_index =
     *      psn % RING_SIZE wraps (slot 10239 -> 0). Store RING_SIZE packets at
     *      an offset base = RING_SIZE/2.
     *  (2) 24-bit PSN wrap: adjacent PSNs across the 2^24 boundary
     *      (0xFFFFFF -> 0x000000) must land on distinct ring slots and retrieve
     *      correctly — no collision from the 24-bit field wrapping. */
    int n = RING_SIZE;
    unsigned char *scratch = calloc((size_t)L, 1);
    printf("\n===== PSN wraparound correctness =====\n");

    /* (1) ring-index wraparound */
    for (int mi = 0; mi < NMETHODS; mi++) {
        const struct method_desc *md = &METHODS[mi];
        if (!md->is_psn)
            continue;
        struct b_cache *c = md->make(n);
        uint32_t base = (uint32_t)(RING_SIZE / 2);
        for (int i = 0; i < n; i++)
            store_payload(c, base + (uint32_t)i, scratch, L);
        int ok = 0;
        for (int i = 0; i < n; i++) {
            uint32_t psn = base + (uint32_t)i;
            struct mem_block_header *h = c->ops.retrieve(c, psn);
            if (h && h->psn == psn && h->data_len == L &&
                check_payload((const unsigned char *)(h + 1), L, psn))
                ok++;
        }
        printf("  ring-wrap   %-12s %d/%d\n", md->name, ok, n);
        c->ops.destroy(c);
    }

    /* (2) 24-bit PSN wrap: adjacent PSNs across the 2^24 boundary */
    for (int mi = 0; mi < NMETHODS; mi++) {
        const struct method_desc *md = &METHODS[mi];
        if (!md->is_psn)
            continue;
        struct b_cache *c = md->make(n);
        uint32_t around[2] = {0xFFFFFFu, 0x000000u};
        store_payload(c, around[0], scratch, L);
        store_payload(c, around[1], scratch, L);
        int ok = 0;
        for (int k = 0; k < 2; k++) {
            struct mem_block_header *h = c->ops.retrieve(c, around[k]);
            if (h && h->psn == around[k] && h->data_len == L &&
                check_payload((const unsigned char *)(h + 1), L, around[k]))
                ok++;
        }
        printf("  psn24-wrap  %-12s %d/%d\n", md->name, ok, 2);
        c->ops.destroy(c);
    }
    free(scratch);
}

/* ---------------- entry ---------------- */
static void usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s [options]\n"
            "  -n N      cache depth / ring length (default %d)\n"
            "  -l L      main payload size (default 1024, <= %d)\n"
            "  -B B      timing batch (default 128)\n"
            "  -R R      store-sampler batches per rep (default 50)\n"
            "  -o DIR    output dir (default .)\n"
            "  --zero-reorder       run only Experiment A\n"
            "  --retrieve-cdf       run only Experiment B\n"
            "  --deterministic-mem  run only Experiment C\n"
            "  (no selector: run all experiments + store sampler)\n",
            prog, RING_SIZE, MAX_L);
}

int main(int argc, char **argv)
{
    int N = RING_SIZE, L = 1024, B = 128, R = 50;
    const char *outdir = ".";
    int mode = 0; /* 0 = all; 1 = A; 2 = B; 3 = C */

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
        else if (!strcmp(a, "-o") && i + 1 < argc)
            outdir = argv[++i];
        else if (!strcmp(a, "--zero-reorder"))
            mode = 1;
        else if (!strcmp(a, "--retrieve-cdf"))
            mode = 2;
        else if (!strcmp(a, "--deterministic-mem"))
            mode = 3;
        else {
            usage(argv[0]);
            return 1;
        }
    }

    if (N <= 0 || N > RING_SIZE) {
        fprintf(stderr, "[error] -n must be in 1..%d\n", RING_SIZE);
        return 1;
    }
    if (L <= 0 || L > MAX_L) {
        fprintf(stderr, "[error] -l must be in 1..%d\n", MAX_L);
        return 1;
    }
    if (B <= 0 || N % B != 0) {
        fprintf(stderr, "[error] N=%d must be divisible by B=%d\n", N, B);
        return 1;
    }
    if (strcmp(outdir, ".") != 0)
        mkdir(outdir, 0755);

    calibrate_tsc();
    warm_allocator(MEM_BLOCK_SIZE, 2 * N);
    warm_allocator(1056, 2 * N);
    warm_allocator(512, 2 * N);

    printf("params: N=%d, L=%d B, batch B=%d, reps=%d, seed=42\n", N, L, B,
           REPS);
    printf("methods: fifo / chained_hash / balanced_tree / psn_fixed / "
           "psn_dynamic / psn_tiered\n");

    int SL = 64; /* store-sampler payload: isolate data-structure overhead */
    if (mode == 0 || mode == 1)
        run_zero_reorder(N, L, outdir);
    if (mode == 0 || mode == 2)
        run_retrieve_cdf(N, L, B, outdir);
    if (mode == 0 || mode == 3)
        run_deterministic_mem(N, L, outdir);
    if (mode == 0)
        run_store_samples(N, SL, B, R, outdir);
    run_wraptest(L);

    printf("\n[done] outputs in %s/ : zero_reorder.csv, retrieve_samples.csv, "
           "deterministic_mem.csv, store_samples.csv\n",
           outdir);
    printf("       plot with: python3 ../paper_figures/plot_figures.py\n");
    return 0;
}

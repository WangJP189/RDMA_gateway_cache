/*
 * baselines/baseline.c — fifo / chained_hash / balanced_tree（无界 + *_bounded）+ dynblock 适配
 * ------------------------------------------------------------------------------------------------
 * 无界基线逐字段复制自 260912 psn_behavior_bench/behavior_bench.c（出处见
 * docs/baseline_param_provenance.md）。*_bounded 是本项目新增（对照 D）：
 *   预分配空闲链表 + 容量 N + 滑动淘汰，使「活集恒为 N ⇒ hash 负载恒 N/nbuckets=0.625」，
 *   与 psn_dynblock 唯一差异回到「索引结构」本身（消除分配策略与活集大小两个 confound）。
 */
#include "baseline.h"

#include <stdlib.h>
#include <string.h>

/* ================= 预分配空闲链表 =================
 * 复用节点首 8 字节（fifo/hash 的 next、avl 的 l）作空闲链表 next 指针，节点结构不改
 * （fifo/hash 仍 32B、avl 56B（本版加父指针）⇒ warmup 尺寸表用 sizeof 自适应，不硬编码）。 */

typedef struct {
    uint8_t *mem;    /* 一次性大块（node_pool_free 用） */
    void    *head;   /* 空闲链表头 */
} node_pool_t;

static void pool_init(node_pool_t *p, uint32_t n, size_t node_size) {
    p->mem = (uint8_t *)malloc((size_t)n * node_size);
    if (!p->mem) { p->head = NULL; return; }
    p->head = NULL;
    for (uint32_t i = 0; i < n; i++) {
        void **slot = (void **)(p->mem + (size_t)i * node_size);
        *slot = p->head;
        p->head = slot;
    }
}
static void *pool_get(node_pool_t *p) {
    void *n = p->head;
    if (n) p->head = *(void **)n;
    return n;
}
static void pool_put(node_pool_t *p, void *node) {
    *(void **)node = p->head;
    p->head = node;
}
static void node_pool_free(node_pool_t *p) { free(p->mem); p->mem = NULL; p->head = NULL; }

/* ================= 通用 GBN/SR 重传（定位 + memcpy 全过程） =================
 * 所有无界与 *_bounded 基线共用：按 PSN 逐包 retrieve + memcpy。
 * psn_dynblock 用自己的 conn_retransmit_range/set（同为逐包 conn_lookup 循环，公平）。 */

static uint32_t generic_retrieve_range(b_cache_t *bc, uint32_t start, uint32_t count,
                                       uint8_t *out, uint32_t out_cap) {
    uint32_t w = 0;
    for (uint32_t k = 0; k < count; k++) {
        uint32_t len = 0;
        const uint8_t *p = bc->ops.retrieve(bc, start + k, &len);
        if (!p) continue;
        if (w + len > out_cap) break;
        memcpy(out + w, p, len);
        w += len;
    }
    return w;
}
static uint32_t generic_retrieve_set(b_cache_t *bc, const uint32_t *psns, uint32_t n,
                                     uint8_t *out, uint32_t out_cap) {
    uint32_t w = 0;
    for (uint32_t k = 0; k < n; k++) {
        uint32_t len = 0;
        const uint8_t *p = bc->ops.retrieve(bc, psns[k], &len);
        if (!p) continue;
        if (w + len > out_cap) break;
        memcpy(out + w, p, len);
        w += len;
    }
    return w;
}

/* ================= FIFO（到货序队列） ================= */
typedef struct fifo_node {
    struct fifo_node *next;
    struct mem_block_header hdr;
    uint8_t data[];
} fifo_node_t;

typedef struct {
    b_cache_t base;
    fifo_node_t *head, *tail;
} fifo_t;

_Static_assert(sizeof(fifo_node_t) == 32, "fifo_node must be 32B");

static void fifo_store(b_cache_t *bc, uint32_t psn, const uint8_t *p, uint32_t len) {
    fifo_t *f = (fifo_t *)bc;
    fifo_node_t *n = (fifo_node_t *)malloc(sizeof(*n) + (size_t)len);
    bc->n_malloc++;
    n->next = NULL;
    n->hdr.psn = psn;
    n->hdr.data_len = (int32_t)len;
    n->hdr.recv_stamp = 0;
    memcpy(n->data, p, len);
    if (f->tail) f->tail->next = n; else f->head = n;
    f->tail = n;
}

static const uint8_t *fifo_retrieve(b_cache_t *bc, uint32_t psn, uint32_t *out_len) {
    fifo_t *f = (fifo_t *)bc;
    for (fifo_node_t *n = f->head; n; n = n->next) {
        bc->n_cmp++;
        if (n->hdr.psn == psn) { if (out_len) *out_len = (uint32_t)n->hdr.data_len; return n->data; }
    }
    return NULL;
}

static void fifo_destroy(b_cache_t *bc) {
    fifo_t *f = (fifo_t *)bc;
    fifo_node_t *n = f->head;
    while (n) { fifo_node_t *nx = n->next; free(n); n = nx; }
    free(f);
}

b_cache_t *make_fifo(void) {
    fifo_t *f = (fifo_t *)calloc(1, sizeof(*f));
    f->base.name = "fifo";
    f->base.ops.store = fifo_store;
    f->base.ops.retrieve = fifo_retrieve;
    f->base.ops.retrieve_range = generic_retrieve_range;
    f->base.ops.retrieve_set = generic_retrieve_set;
    f->base.ops.destroy = fifo_destroy;
    return &f->base;
}

/* ---- fifo_bounded：容量 N，满则淘汰 head（FIFO 滑动淘汰） ---- */
typedef struct {
    b_cache_t base;
    uint32_t cap;
    uint32_t payload_len;      /* exp2 空间利用率：footprint 计算用 */
    fifo_node_t *head, *tail;
    node_pool_t pool;
} fifo_bounded_t;

static void fifo_bounded_store(b_cache_t *bc, uint32_t psn, const uint8_t *p, uint32_t len) {
    fifo_bounded_t *f = (fifo_bounded_t *)bc;
    if (bc->n_live == f->cap) {                     /* 满：淘汰 head */
        fifo_node_t *old = f->head;
        f->head = old->next;
        if (!f->head) f->tail = NULL;
        pool_put(&f->pool, old);
        bc->n_live--;
    }
    fifo_node_t *n = (fifo_node_t *)pool_get(&f->pool);
    n->next = NULL;
    n->hdr.psn = psn;
    n->hdr.data_len = (int32_t)len;
    n->hdr.recv_stamp = 0;
    memcpy(n->data, p, len);
    if (f->tail) f->tail->next = n; else f->head = n;
    f->tail = n;
    bc->n_live++;
}

static const uint8_t *fifo_bounded_retrieve(b_cache_t *bc, uint32_t psn, uint32_t *out_len) {
    fifo_bounded_t *f = (fifo_bounded_t *)bc;
    for (fifo_node_t *n = f->head; n; n = n->next) {
        bc->n_cmp++;
        if (n->hdr.psn == psn) { if (out_len) *out_len = (uint32_t)n->hdr.data_len; return n->data; }
    }
    return NULL;
}

static void fifo_bounded_destroy(b_cache_t *bc) {
    fifo_bounded_t *f = (fifo_bounded_t *)bc;
    node_pool_free(&f->pool);
    free(f);
}

b_cache_t *make_fifo_bounded(uint32_t capacity, uint32_t payload_len) {
    fifo_bounded_t *f = (fifo_bounded_t *)calloc(1, sizeof(*f));
    f->base.name = "fifo_bounded";
    f->base.ops.store = fifo_bounded_store;
    f->base.ops.retrieve = fifo_bounded_retrieve; /* 同为线性扫描（但 head 偏移不同，需专用实现） */
    f->base.ops.retrieve_range = generic_retrieve_range;
    f->base.ops.retrieve_set = generic_retrieve_set;
    f->base.ops.destroy = fifo_bounded_destroy;
    f->cap = capacity;
    f->payload_len = payload_len;
    pool_init(&f->pool, capacity, sizeof(fifo_node_t) + (size_t)payload_len);
    return &f->base;
}

/* ================= Chained hash（Knuth 乘法哈希，取高位） ================= */
typedef struct hash_node {
    struct hash_node *next;
    struct mem_block_header hdr;
    uint8_t data[];
} hash_node_t;

typedef struct {
    b_cache_t base;
    hash_node_t **bkt;
    uint32_t nb;
} hash_t;

_Static_assert(sizeof(hash_node_t) == 32, "hash_node must be 32B");

/* behavior_bench.c:255-258：(psn * 2654435761u) >> (32 - HASH_BITS)；nb 必须是 2 的幂。 */
static inline uint32_t hash_idx(uint32_t psn, uint32_t nb) {
    return (psn * 2654435761u) >> (32 - __builtin_ctz(nb));
}

static void hash_store(b_cache_t *bc, uint32_t psn, const uint8_t *p, uint32_t len) {
    hash_t *h = (hash_t *)bc;
    hash_node_t *n = (hash_node_t *)malloc(sizeof(*n) + (size_t)len);
    bc->n_malloc++;
    n->hdr.psn = psn;
    n->hdr.data_len = (int32_t)len;
    n->hdr.recv_stamp = 0;
    memcpy(n->data, p, len);
    uint32_t b = hash_idx(psn, h->nb);
    n->next = h->bkt[b];
    h->bkt[b] = n;
}

static const uint8_t *hash_retrieve(b_cache_t *bc, uint32_t psn, uint32_t *out_len) {
    hash_t *h = (hash_t *)bc;
    for (hash_node_t *n = h->bkt[hash_idx(psn, h->nb)]; n; n = n->next) {
        bc->n_cmp++;
        if (n->hdr.psn == psn) { if (out_len) *out_len = (uint32_t)n->hdr.data_len; return n->data; }
    }
    return NULL;
}

static void hash_destroy(b_cache_t *bc) {
    hash_t *h = (hash_t *)bc;
    for (uint32_t b = 0; b < h->nb; b++) {
        hash_node_t *n = h->bkt[b];
        while (n) { hash_node_t *nx = n->next; free(n); n = nx; }
    }
    free(h->bkt);
    free(h);
}

b_cache_t *make_chained_hash(uint32_t nbuckets) {
    hash_t *h = (hash_t *)calloc(1, sizeof(*h));
    h->base.name = "chained_hash";
    h->base.ops.store = hash_store;
    h->base.ops.retrieve = hash_retrieve;
    h->base.ops.retrieve_range = generic_retrieve_range;
    h->base.ops.retrieve_set = generic_retrieve_set;
    h->base.ops.destroy = hash_destroy;
    h->nb = nbuckets;
    h->bkt = (hash_node_t **)calloc(nbuckets, sizeof(hash_node_t *));
    return &h->base;
}

/* ---- chained_hash_bounded：容量 N，位置淘汰（slot = psn % N，与 dynblock Φ 一致） ---- */
typedef struct {
    b_cache_t base;
    hash_node_t **bkt;
    uint32_t nb, cap;
    uint32_t payload_len;       /* exp2 空间利用率：footprint 计算用 */
    hash_node_t **slot_owner;   /* [cap]：slot i 当前节点（NULL=空） */
    node_pool_t pool;
} hash_bounded_t;

static void hash_bounded_store(b_cache_t *bc, uint32_t psn, const uint8_t *p, uint32_t len) {
    hash_bounded_t *h = (hash_bounded_t *)bc;
    uint32_t i = psn % h->cap;                   /* 位置淘汰：与 dynblock phi 同一 eviction 语义 */
    hash_node_t *old = h->slot_owner[i];
    if (old) {                                   /* 从桶链摘下旧节点（负载 0.625 ⇒ 链 ~1 节点） */
        uint32_t b = hash_idx(old->hdr.psn, h->nb);
        hash_node_t **pp = &h->bkt[b];
        while (*pp && *pp != old) pp = &(*pp)->next;
        if (*pp == old) *pp = old->next;
        pool_put(&h->pool, old);
    } else {
        bc->n_live++;                            /* 位置空：首次填充，驻留 +1（替换路径驻留不变） */
    }
    hash_node_t *n = (hash_node_t *)pool_get(&h->pool);
    n->hdr.psn = psn;
    n->hdr.data_len = (int32_t)len;
    n->hdr.recv_stamp = 0;
    memcpy(n->data, p, len);
    uint32_t b = hash_idx(psn, h->nb);
    n->next = h->bkt[b];
    h->bkt[b] = n;
    h->slot_owner[i] = n;
}

static const uint8_t *hash_bounded_retrieve(b_cache_t *bc, uint32_t psn, uint32_t *out_len) {
    hash_bounded_t *h = (hash_bounded_t *)bc;
    for (hash_node_t *n = h->bkt[hash_idx(psn, h->nb)]; n; n = n->next) {
        bc->n_cmp++;
        if (n->hdr.psn == psn) { if (out_len) *out_len = (uint32_t)n->hdr.data_len; return n->data; }
    }
    return NULL;
}

static void hash_bounded_destroy(b_cache_t *bc) {
    hash_bounded_t *h = (hash_bounded_t *)bc;
    node_pool_free(&h->pool);
    free(h->slot_owner);
    free(h->bkt);
    free(h);
}

b_cache_t *make_chained_hash_bounded(uint32_t capacity, uint32_t nbuckets, uint32_t payload_len) {
    hash_bounded_t *h = (hash_bounded_t *)calloc(1, sizeof(*h));
    h->base.name = "chained_hash_bounded";
    h->base.ops.store = hash_bounded_store;
    h->base.ops.retrieve = hash_bounded_retrieve; /* 同为桶查找（专用实现，避免布局耦合） */
    h->base.ops.retrieve_range = generic_retrieve_range;
    h->base.ops.retrieve_set = generic_retrieve_set;
    h->base.ops.destroy = hash_bounded_destroy;
    h->nb = nbuckets;
    h->cap = capacity;
    h->payload_len = payload_len;
    h->bkt = (hash_node_t **)calloc(nbuckets, sizeof(hash_node_t *));
    h->slot_owner = (hash_node_t **)calloc(capacity, sizeof(hash_node_t *));
    pool_init(&h->pool, capacity, sizeof(hash_node_t) + (size_t)payload_len);
    return &h->base;
}

/* ================= AVL balanced tree ================= */
typedef struct avl_node {
    struct avl_node *l, *r, *p;   /* p：父指针——tree_bounded FIFO 淘汰按节点指针直接摘除（O(1) 定位，不再按 key 搜索） */
    int h;
    struct mem_block_header hdr;
    uint8_t data[];
} avl_node_t;

typedef struct {
    b_cache_t base;
    avl_node_t *root;
} tree_t;

_Static_assert(sizeof(avl_node_t) == 56, "avl_node must be 56B (l+r+p+h+hdr)");

static inline int avl_h(avl_node_t *n) { return n ? n->h : 0; }
static inline void avl_fix(avl_node_t *n) {
    int hl = avl_h(n->l), hr = avl_h(n->r);
    n->h = 1 + (hl > hr ? hl : hr);
}
static inline int avl_bf(avl_node_t *n) { return avl_h(n->l) - avl_h(n->r); }

static avl_node_t *rot_r(avl_node_t *y) {          /* 右旋：y 的 l 上提；维护父指针与 gp 子链接 */
    avl_node_t *x = y->l;
    avl_node_t *gp = y->p;
    avl_node_t *b = x->r;
    x->r = y; y->p = x;
    y->l = b; if (b) b->p = y;
    x->p = gp;
    if (gp) { if (gp->l == y) gp->l = x; else gp->r = x; }
    avl_fix(y); avl_fix(x);
    return x;
}
static avl_node_t *rot_l(avl_node_t *x) {          /* 左旋：x 的 r 上提；维护父指针与 gp 子链接 */
    avl_node_t *y = x->r;
    avl_node_t *gp = x->p;
    avl_node_t *b = y->l;
    y->l = x; x->p = y;
    x->r = b; if (b) b->p = x;
    y->p = gp;
    if (gp) { if (gp->l == x) gp->l = y; else gp->r = y; }
    avl_fix(x); avl_fix(y);
    return y;
}
static avl_node_t *avl_balance(avl_node_t *n) {
    avl_fix(n);
    int bf = avl_bf(n);
    if (bf > 1) { if (avl_bf(n->l) < 0) n->l = rot_l(n->l); return rot_r(n); }
    if (bf < -1) { if (avl_bf(n->r) > 0) n->r = rot_r(n->r); return rot_l(n); }
    return n;
}

/* 插入新分配节点（无界基线 store 用） */
static avl_node_t *avl_insert_new(b_cache_t *bc, avl_node_t *t,
                                  uint32_t psn, uint32_t len, const uint8_t *p) {
    if (!t) {
        avl_node_t *n = (avl_node_t *)malloc(sizeof(*n) + (size_t)len);
        bc->n_malloc++;
        n->l = n->r = n->p = NULL; n->h = 1;
        n->hdr.psn = psn;
        n->hdr.data_len = (int32_t)len;
        n->hdr.recv_stamp = 0;
        memcpy(n->data, p, len);
        return n;
    }
    bc->n_cmp++;
    if (psn < t->hdr.psn) { t->l = avl_insert_new(bc, t->l, psn, len, p); t->l->p = t; }
    else                  { t->r = avl_insert_new(bc, t->r, psn, len, p); t->r->p = t; }
    return avl_balance(t);
}

/* 插入已分配节点（*_bounded store 用，不 malloc、不计数） */
static avl_node_t *avl_insert_node(b_cache_t *bc, avl_node_t *t, avl_node_t *n) {
    if (!t) { n->l = n->r = n->p = NULL; n->h = 1; return n; }
    bc->n_cmp++;
    if (n->hdr.psn < t->hdr.psn) { t->l = avl_insert_node(bc, t->l, n); t->l->p = t; }
    else                          { t->r = avl_insert_node(bc, t->r, n); t->r->p = t; }
    return avl_balance(t);
}

/* 按节点指针直接摘除 z（tree_bounded 的 FIFO 淘汰用）：O(1) 定位（指针由 FIFO 队列给出），
 * 不再按 key O(log N) 搜索——这是公平性修复（旧版 fifo_psn 存 PSN、每次 avl_delete_key 全树搜索）。
 * 0/1 子：直接 splice；2 子：用中序后继 succ 物理顶替（零数据拷贝，payload 可达 4096B）。
 * 再平衡沿父链向上（父指针维护），返回新 root。 */
static avl_node_t *avl_remove_ptr(avl_node_t *root, avl_node_t *z) {
    avl_node_t *y, *yp, *child;

    /* 实际摘除的节点 y：z 有 2 子取后继（必无左子 ⇒ 至多 1 子），否则 z 本身 */
    if (z->l && z->r) {
        y = z->r;
        while (y->l) y = y->l;
    } else {
        y = z;
    }
    yp = y->p;
    child = y->l ? y->l : y->r;

    /* splice y 出去：child 顶替 y 在 yp 下的位置 */
    if (child) child->p = yp;
    if (!yp)             root = child;
    else if (yp->l == y) yp->l = child;
    else                 yp->r = child;

    /* 若 y 是后继（y != z），把 y 物理搬到 z 的位置 */
    if (y != z) {
        y->l = z->l; if (z->l) z->l->p = y;
        y->r = z->r; if (z->r) z->r->p = y;
        y->p = z->p;
        if (!z->p)              root = y;
        else if (z->p->l == z)  z->p->l = y;
        else                    z->p->r = y;
        y->h = z->h;
    }
    z->l = z->r = z->p = NULL;

    /* 再平衡：从 splice 点沿父链向上。y==z->r 时 yp==z 已被顶替/摘除，改从移植后的 y 起。
     * avl_balance 可能旋转出新的子树根；旋转已修正 gp 子链接，仅当 w 是根时才需更新 root。 */
    avl_node_t *w = (y != z && yp == z) ? y : yp;
    while (w) {
        avl_node_t *par = w->p;
        avl_node_t *sub = avl_balance(w);
        if (!par) root = sub;    /* w 是整树根：旋转后新根替换 root */
        w = par;
    }
    return root;
}

static void tree_store(b_cache_t *bc, uint32_t psn, const uint8_t *p, uint32_t len) {
    tree_t *t = (tree_t *)bc;
    t->root = avl_insert_new(bc, t->root, psn, len, p);
}

static const uint8_t *tree_retrieve(b_cache_t *bc, uint32_t psn, uint32_t *out_len) {
    tree_t *t = (tree_t *)bc;
    avl_node_t *n = t->root;
    while (n) {
        bc->n_cmp++;
        if (psn == n->hdr.psn) { if (out_len) *out_len = (uint32_t)n->hdr.data_len; return n->data; }
        n = (psn < n->hdr.psn) ? n->l : n->r;
    }
    return NULL;
}

static void tree_free_nodes(avl_node_t *n) {
    if (!n) return;
    tree_free_nodes(n->l);
    tree_free_nodes(n->r);
    free(n);
}
static void tree_destroy(b_cache_t *bc) {
    tree_t *t = (tree_t *)bc;
    tree_free_nodes(t->root);
    free(t);
}

b_cache_t *make_balanced_tree(void) {
    tree_t *t = (tree_t *)calloc(1, sizeof(*t));
    t->base.name = "balanced_tree";
    t->base.ops.store = tree_store;
    t->base.ops.retrieve = tree_retrieve;
    t->base.ops.retrieve_range = generic_retrieve_range;
    t->base.ops.retrieve_set = generic_retrieve_set;
    t->base.ops.destroy = tree_destroy;
    return &t->base;
}

/* ---- balanced_tree_bounded：容量 N，满则按插入序 FIFO 淘汰最早进入者（滑动淘汰） ---- */
typedef struct {
    b_cache_t base;
    avl_node_t *root;
    uint32_t cap;
    uint32_t payload_len;     /* exp2 空间利用率：footprint 计算用 */
    avl_node_t **fifo_node;   /* 插入序 FIFO 环（容量 cap，存节点指针；head 为队头） */
    uint32_t head;
    node_pool_t pool;
} tree_bounded_t;

static void tree_bounded_store(b_cache_t *bc, uint32_t psn, const uint8_t *p, uint32_t len) {
    tree_bounded_t *t = (tree_bounded_t *)bc;
    if (bc->n_live == t->cap) {                  /* 满：FIFO 淘汰最早插入（按节点指针直接摘除） */
        avl_node_t *old = t->fifo_node[t->head];
        t->head = (t->head + 1) % t->cap;
        t->root = avl_remove_ptr(t->root, old);
        pool_put(&t->pool, old);
        bc->n_live--;
    }
    avl_node_t *n = (avl_node_t *)pool_get(&t->pool);
    n->l = n->r = n->p = NULL; n->h = 1;
    n->hdr.psn = psn;
    n->hdr.data_len = (int32_t)len;
    n->hdr.recv_stamp = 0;
    memcpy(n->data, p, len);
    t->fifo_node[(t->head + bc->n_live) % t->cap] = n;  /* 入队（活集 < cap 时 head==0） */
    t->root = avl_insert_node(bc, t->root, n);
    bc->n_live++;
}

static const uint8_t *tree_bounded_retrieve(b_cache_t *bc, uint32_t psn, uint32_t *out_len) {
    tree_bounded_t *t = (tree_bounded_t *)bc;
    avl_node_t *n = t->root;
    while (n) {
        bc->n_cmp++;
        if (psn == n->hdr.psn) { if (out_len) *out_len = (uint32_t)n->hdr.data_len; return n->data; }
        n = (psn < n->hdr.psn) ? n->l : n->r;
    }
    return NULL;
}

static void tree_bounded_destroy(b_cache_t *bc) {
    tree_bounded_t *t = (tree_bounded_t *)bc;
    free(t->fifo_node);
    node_pool_free(&t->pool);
    free(t);
}

b_cache_t *make_balanced_tree_bounded(uint32_t capacity, uint32_t payload_len) {
    tree_bounded_t *t = (tree_bounded_t *)calloc(1, sizeof(*t));
    t->base.name = "balanced_tree_bounded";
    t->base.ops.store = tree_bounded_store;
    t->base.ops.retrieve = tree_bounded_retrieve; /* 同为 AVL 查找（专用实现，避免布局耦合） */
    t->base.ops.retrieve_range = generic_retrieve_range;
    t->base.ops.retrieve_set = generic_retrieve_set;
    t->base.ops.destroy = tree_bounded_destroy;
    t->cap = capacity;
    t->payload_len = payload_len;
    t->fifo_node = (avl_node_t **)calloc(capacity, sizeof(avl_node_t *));
    pool_init(&t->pool, capacity, sizeof(avl_node_t) + (size_t)payload_len);
    return &t->base;
}

/* ================= 测试辅助：AVL 不变式校验 =================
 * 校验 tree_bounded 的两条不变式：中序严格升序（BST 性质）+ 所有节点 |balance factor|≤1。
 * 供 smoke 在每次 store（含 FIFO 淘汰/双子删除）后调用，挡住「删除把树结构弄坏、
 * 某些查找仍碰巧通过」这类隐蔽 bug。 */
static void verify_avl_inorder(avl_node_t *n, uint32_t *prev, int *first, int *ok) {
    if (!n) return;
    if (n->l && n->l->p != n) *ok = 0;          /* 父指针一致性 */
    if (n->r && n->r->p != n) *ok = 0;
    verify_avl_inorder(n->l, prev, first, ok);
    if (*first) *first = 0;
    else if (n->hdr.psn <= *prev) *ok = 0;      /* 中序必须严格升序 */
    *prev = n->hdr.psn;
    verify_avl_inorder(n->r, prev, first, ok);
    int bf = avl_h(n->l) - avl_h(n->r);
    if (bf > 1 || bf < -1) *ok = 0;             /* |BF| ≤ 1 */
}

int baseline_tree_verify(b_cache_t *bc) {
    if (strcmp(bc->name, "balanced_tree_bounded") != 0) return 0;  /* 非 tree：无不变式 */
    tree_bounded_t *t = (tree_bounded_t *)bc;
    if (t->root && t->root->p != NULL) return -1;  /* 根父指针必须为 NULL */
    uint32_t prev = 0; int first = 1, ok = 1;
    verify_avl_inorder(t->root, &prev, &first, &ok);
    return ok ? 0 : -1;
}

/* ================= psn_dynblock 适配器（我们的方法） ================= */
typedef struct {
    b_cache_t base;
    conn_t conn;
} dynblock_t;

static void dynblock_store(b_cache_t *bc, uint32_t psn, const uint8_t *p, uint32_t len) {
    dynblock_t *d = (dynblock_t *)bc;
    uint64_t mb = d->conn.n_ovf_malloc, rb = d->conn.n_resize;
    conn_store(&d->conn, psn, p, (uint16_t)len);
    bc->n_malloc += d->conn.n_ovf_malloc - mb;   /* exp1a 环路径：恒 0 */
    bc->n_resize += d->conn.n_resize - rb;       /* exp1a adaptive_enable=0：恒 0 */
}

static const uint8_t *dynblock_retrieve(b_cache_t *bc, uint32_t psn, uint32_t *out_len) {
    dynblock_t *d = (dynblock_t *)bc;
    uint16_t len = 0;
    const uint8_t *p = conn_lookup(&d->conn, psn, &len);
    if (p && out_len) *out_len = len;
    return p;
}

static uint32_t dynblock_retrieve_range(b_cache_t *bc, uint32_t start, uint32_t count,
                                        uint8_t *out, uint32_t out_cap) {
    dynblock_t *d = (dynblock_t *)bc;
    return (uint32_t)conn_retransmit_range(&d->conn, start, count, out, out_cap);
}

static uint32_t dynblock_retrieve_set(b_cache_t *bc, const uint32_t *psns, uint32_t n,
                                      uint8_t *out, uint32_t out_cap) {
    dynblock_t *d = (dynblock_t *)bc;
    return (uint32_t)conn_retransmit_set(&d->conn, psns, n, out, out_cap);
}

static void dynblock_destroy(b_cache_t *bc) {
    dynblock_t *d = (dynblock_t *)bc;
    conn_destroy(&d->conn);
    free(d);
}

b_cache_t *make_dynblock(const cfg_t *cfg, uint32_t mtu_from_cm) {
    dynblock_t *d = (dynblock_t *)calloc(1, sizeof(*d));
    d->base.name = "psn_dynblock";
    d->base.ops.store = dynblock_store;
    d->base.ops.retrieve = dynblock_retrieve;
    d->base.ops.retrieve_range = dynblock_retrieve_range;
    d->base.ops.retrieve_set = dynblock_retrieve_set;
    d->base.ops.destroy = dynblock_destroy;
    conn_init(&d->conn, cfg, mtu_from_cm);
    return &d->base;
}

/* 读 dynblock 当前块大小 S（收敛阶段记录用；非 dynblock 返回 0）。 */
uint32_t dynblock_cur_S(const b_cache_t *bc) {
    if (strcmp(bc->name, "psn_dynblock") != 0) return 0;
    return ((dynblock_t *)bc)->conn.S;
}

/* ================= 对照 E：index_only（只算 Φ + 一次槽写/读，不拷贝 payload） =================
 * 回答审稿人必问的「取模不是免费的」：给出位置映射的不可约成本（映射 + 槽命中）。
 *   store    = Φ(psn) + 一次槽写（psn 只写一个 u32，无 header、无 memcpy）；
 *   retrieve = Φ(psn) + 一次槽读 + psn 比较（返回静态 dummy，len=0）。
 * n_malloc=0（store 路径零分配）、n_cmp=0（Φ 纯算术，无 PSN 比较），与 psn_dynblock 同口径。 */
typedef struct {
    b_cache_t base;
    uint32_t  N;
    uint32_t *meta;   /* [N]：只存 psn；槽命中即命中（无 flag/gen/len） */
} index_only_t;

static void index_only_store(b_cache_t *bc, uint32_t psn, const uint8_t *p, uint32_t len) {
    (void)p; (void)len;
    index_only_t *io = (index_only_t *)bc;
    io->meta[(psn & 0xFFFFFFu) % io->N] = psn;   /* 取模 + 一次槽写 */
}

static const uint8_t *index_only_retrieve(b_cache_t *bc, uint32_t psn, uint32_t *out_len) {
    index_only_t *io = (index_only_t *)bc;
    static uint8_t dummy;
    if (io->meta[(psn & 0xFFFFFFu) % io->N] == psn) { if (out_len) *out_len = 0; return &dummy; }
    return NULL;
}

static void index_only_destroy(b_cache_t *bc) {
    index_only_t *io = (index_only_t *)bc;
    free(io->meta);
    free(io);
}

b_cache_t *make_index_only(uint32_t N) {
    index_only_t *io = (index_only_t *)calloc(1, sizeof(*io));
    io->base.name = "index_only";
    io->base.ops.store = index_only_store;
    io->base.ops.retrieve = index_only_retrieve;
    io->base.ops.retrieve_range = generic_retrieve_range;
    io->base.ops.retrieve_set = generic_retrieve_set;
    io->base.ops.destroy = index_only_destroy;
    io->N = N;
    io->meta = (uint32_t *)calloc(N, sizeof(uint32_t));
    return &io->base;
}

/* ================= exp2 空间利用率：footprint（分配总量，sizeof 实测） =================
 * 语义：把「为容纳满窗 N 条、包长 pl」所需的全部预分配字节（池 + 索引/桶 + 元数据）加总，
 *       不含运行时才 malloc 的溢出块（tier 包下恒 0，此处仍如实累加）。
 * 无界基线（每 store malloc）不参与 exp2，返回 0。 */
uint64_t cache_footprint_bytes(const b_cache_t *bc) {
    if (strcmp(bc->name, "fifo_bounded") == 0) {
        fifo_bounded_t *f = (fifo_bounded_t *)bc;
        return (uint64_t)f->cap * (sizeof(fifo_node_t) + f->payload_len);
    }
    if (strcmp(bc->name, "chained_hash_bounded") == 0) {
        hash_bounded_t *h = (hash_bounded_t *)bc;
        return (uint64_t)h->cap * (sizeof(hash_node_t) + h->payload_len)
             + (uint64_t)h->nb * sizeof(hash_node_t *)
             + (uint64_t)h->cap * sizeof(hash_node_t *);
    }
    if (strcmp(bc->name, "balanced_tree_bounded") == 0) {
        tree_bounded_t *t = (tree_bounded_t *)bc;
        return (uint64_t)t->cap * (sizeof(avl_node_t) + t->payload_len)
             + (uint64_t)t->cap * sizeof(avl_node_t *);
    }
    if (strcmp(bc->name, "psn_dynblock") == 0) {
        dynblock_t *d = (dynblock_t *)bc;
        conn_t *c = &d->conn;
        uint64_t bytes = (uint64_t)c->cur.N * c->cur.stride;              /* 环池（含旧代） */
        if (c->has_old) bytes += (uint64_t)c->old.N * c->old.stride;
        bytes += (uint64_t)c->N * sizeof(slot_meta_t);                    /* meta */
        bytes += (uint64_t)c->cfg->ovf_cap * sizeof(ovf_entry_t);         /* 溢出数组 */
        for (uint32_t oi = 0; oi < c->cfg->ovf_cap; oi++)                 /* 溢出块实占（tier 包=0） */
            if (c->ovf[oi].used) bytes += align16(HDR_SZ + c->ovf[oi].len);
        return bytes;
    }
    if (strcmp(bc->name, "index_only") == 0) {
        index_only_t *io = (index_only_t *)bc;
        return (uint64_t)io->N * sizeof(uint32_t);
    }
    return 0;   /* 无界基线：exp2 不使用 */
}

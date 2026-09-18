/*
 * src/pool.c — 环池 + 溢出变长块（内存层）
 * ------------------------------------------------------------------
 * 裁决 ⑪：pool_alloc 用 mmap(MAP_NORESERVE) 惰性提交，失败回落 malloc；
 *          控制位 cfg.pool_use_mmap（默认 1）。
 * 裁决 ④：溢出 = 每项 malloc 变长块（无 arena）；环路径 malloc/store == 0，
 *          溢出路径 malloc 按次计数（n_ovf_alloc / n_ovf_malloc）。
 * 裁决 ⑦：溢出块也写 mem_block_header（hdr_set，stamp = psn）；blk 存块基址，
 *          与环路径 slot() 同构（payload 用 ovf_payload(e) 取，全库唯一指针约定）。
 */
#include "dynblock.h"

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/mman.h>

/* ==================== 环池 ==================== */

int pool_alloc(pool_t *p, uint32_t N, uint32_t S, int prefer_mmap) {
    uint32_t stride = stride_of(S);
    size_t total = (size_t)N * stride;
    uint8_t *base = NULL;
    int backing = 0;

    if (prefer_mmap) {
#ifdef MAP_NORESERVE
        void *m = mmap(NULL, total, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
#else
        void *m = mmap(NULL, total, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#endif
        if (m != MAP_FAILED) { base = (uint8_t *)m; backing = 1; }
    }
    if (!base) {
        base = (uint8_t *)malloc(total);   /* 回落 malloc（或 use_mmap==0） */
        backing = 0;
    }
    if (!base) return -1;

    p->base = base;
    p->N = N;
    p->S = S;
    p->stride = stride;
    p->gen = 0;
    p->use_mmap = (uint8_t)backing;
    return 0;
}

void pool_free(pool_t *p) {
    if (!p->base) return;
    if (p->use_mmap)
        munmap(p->base, (size_t)p->N * p->stride);
    else
        free(p->base);
    p->base = NULL;
}

/* ==================== 溢出变长块 ==================== */

void ovf_reset(conn_t *c) {
    memset(c->ovf, 0, (size_t)c->cfg->ovf_cap * sizeof(ovf_entry_t));
    c->ovf_count = 0;
    c->ovf_max_hist = 0;
}

int ovf_alloc(conn_t *c, uint32_t psn, const uint8_t *payload, uint16_t len) {
    if (c->ovf_count >= c->cfg->ovf_cap) return -1;   /* 满 */
    uint32_t oi;
    for (oi = 0; oi < c->cfg->ovf_cap; oi++)
        if (!c->ovf[oi].used) break;
    if (oi >= c->cfg->ovf_cap) return -1;            /* 防御 */

    uint8_t *base = (uint8_t *)malloc(align16(HDR_SZ + len));
    if (!base) return -1;
    hdr_set(base, len, psn, (uint64_t)psn);        /* stamp = psn（统一规则；淘汰是位置淘汰，不读 stamp） */
    memcpy(base + HDR_SZ, payload, len);

    c->ovf[oi].psn = psn;
    c->ovf[oi].len = len;
    c->ovf[oi].used = 1;
    c->ovf[oi].blk = base;                         /* blk = 块基址，与环路径 slot() 同构 */
    c->ovf_count++;
    c->n_ovf_alloc++; c->n_ovf_malloc++;   /* n_ovf_ins 由 conn_store 记（算法层） */
    return (int)oi;
}

void ovf_release(conn_t *c, uint32_t ovf_idx) {
    ovf_entry_t *e = &c->ovf[ovf_idx];
    assert(e->used && "ovf_release: double free or bad index");
    free(e->blk);                                  /* blk 是块基址，直接 free */
    e->used = 0; e->blk = NULL;
    c->ovf_count--;
}

uint32_t ovf_scan_max(const conn_t *c) {
    uint32_t m = 0;
    for (uint32_t oi = 0; oi < c->cfg->ovf_cap; oi++)
        if (c->ovf[oi].used && c->ovf[oi].len > m) m = c->ovf[oi].len;
    return m;
}

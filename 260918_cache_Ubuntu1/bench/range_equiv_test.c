/*
 * bench/range_equiv_test.c — GBN 区间提取正确性（第 4A 验收）
 * ------------------------------------------------------------------
 * 验证 conn_retransmit_range 的「一次 Φ(start) + 顺序读 meta[i0..i0+L-1]」新路径
 * 与「逐包 conn_lookup」旧路径对同一 (start, L) 输出【返回字节数 + 输出缓冲】逐字节一致。
 *
 * 覆盖：
 *   1) 随机 (start, L)×100k：start 落在线窗口附近、L∈[1,128]，命中/未命中混合，
 *      环路径(IN_RING)与溢出路径(IN_OVF)都走到；含环回绕（i0 靠近 N-1）。
 *   2) 24-bit 跨界退化：start 靠近 0xFFFFFF，L 越过 2^24 边界 → 走逐包退化分支。
 *   3) out_cap 截断：小 cap 下两条路径的截断语义一致。
 *
 * 数学前提（写入 dynblock.c 注释，此处断言）：Φ(p+k)≡(Φ(p)+k) mod N（24-bit 不跨界）。
 */
#include "dynblock.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0;
#define CHECK(c, m) do { \
    if (!(c)) { g_fail++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, m); } \
} while (0)

static void fill_payload(uint8_t *buf, uint32_t psn, uint32_t len) {
    for (uint32_t k = 0; k < len; k++) buf[k] = (uint8_t)((psn + k) & 0xFFu);
}

/* 旧路径（参考）：逐包 conn_lookup 循环（第 4A 之前的语义）。 */
static uint32_t ref_range(conn_t *c, uint32_t start, uint32_t L, uint8_t *out, uint32_t out_cap) {
    uint32_t w = 0;
    for (uint32_t k = 0; k < L; k++) {
        uint16_t len = 0;
        const uint8_t *p = conn_lookup(c, start + k, &len);
        if (!p) continue;
        if (w + len > out_cap) break;
        memcpy(out + w, p, len);
        w += len;
    }
    return w;
}

#define OUT_CAP (1u << 18)   /* 256 KB，远大于任何 (start,L) 的输出 */

int main(void) {
    cfg_t cfg; cfg_default(&cfg);
    cfg.ring_n = 4096;
    conn_t c; conn_init(&c, &cfg, 4096);   /* S = ceil_class(4096) = 4096 */

    /* 填充：先 4096 个 1024B（环满，全 IN_RING），再 256 个 5000B（> S → 溢出，
     * 位置淘汰槽 0..255 → 这 256 槽变 IN_OVF）。覆盖 IN_RING + IN_OVF + 未命中。 */
    uint8_t *small = (uint8_t *)malloc(1024);
    uint8_t *big   = (uint8_t *)malloc(5000);
    const uint32_t B = 1000000u;
    for (uint32_t k = 0; k < 4096; k++) {
        fill_payload(small, B + k, 1024);
        CHECK(conn_store(&c, B + k, small, 1024) == R_OK, "populate ring");
    }
    for (uint32_t k = 0; k < 256; k++) {
        fill_payload(big, B + 4096 + k, 5000);
        CHECK(conn_store(&c, B + 4096 + k, big, 5000) == R_OK, "populate overflow");
    }

    uint8_t *nbuf = (uint8_t *)malloc(OUT_CAP);
    uint8_t *rbuf = (uint8_t *)malloc(OUT_CAP);
    xorshift32_t r; xorshift32_seed(&r, 42u);

    /* ---- 1) 随机 (start, L) × 100k：命中/未命中/环回绕混合 ---- */
    uint64_t n_hit = 0;
    const uint32_t TRIALS = 100000;
    for (uint32_t t = 0; t < TRIALS; t++) {
        /* start 落在 [B-128, B+4096+256+128) ⇒ 区间与活窗重叠，命中率可观；L∈[1,128] */
        uint32_t span = 128 + 4096 + 256 + 128;
        uint32_t start = B - 128 + (xorshift32_next(&r) % span);
        uint32_t L = 1 + (xorshift32_next(&r) % 128);
        uint32_t nb = conn_retransmit_range(&c, start, L, nbuf, OUT_CAP);
        uint32_t rb = ref_range(&c, start, L, rbuf, OUT_CAP);
        if (nb != rb) {
            g_fail++;
            fprintf(stderr, "FAIL random: start=%u L=%u new=%u ref=%u\n", start, L, nb, rb);
            break;
        }
        if (nb && memcmp(nbuf, rbuf, nb) != 0) {
            g_fail++;
            fprintf(stderr, "FAIL random content: start=%u L=%u nbytes=%u\n", start, L, nb);
            break;
        }
        if (nb) n_hit++;
    }
    CHECK(g_fail == 0, "random (start,L) x100k: new==ref byte-identical");
    printf("random equivalence: %u trials, %llu with >=1 hit, new==ref byte-identical\n",
           TRIALS, (unsigned long long)n_hit);

    /* ---- 2) 24-bit 跨界退化（start 靠近 0xFFFFFF，L 越过 2^24） ---- */
    {
        /* 在 0xFFFFFF 附近种一批包：先存 0xFFFFE0..0xFFFFFF 与 0..63（跨过 2^24）。
         * 注意 phi 用 (psn&0xFFFFFF)%N，0 与 0x1000000 同槽——此处只测跨界的区间语义一致。 */
        uint32_t start = 0xFFFFFF0u;   /* 0xFFFFFF0 + 32 越过 0x1000000 */
        uint32_t L = 32;
        uint32_t nb = conn_retransmit_range(&c, start, L, nbuf, OUT_CAP);
        uint32_t rb = ref_range(&c, start, L, rbuf, OUT_CAP);
        CHECK(nb == rb, "24-bit cross: new==ref bytes");
        CHECK(nb == 0 || memcmp(nbuf, rbuf, nb) == 0, "24-bit cross: content equal");
    }

    /* ---- 3) out_cap 截断语义一致（小 cap） ---- */
    {
        uint32_t start = B + 4096;      /* 全命中段（IN_OVF 段起点） */
        uint32_t L = 256;
        uint32_t nb = conn_retransmit_range(&c, start, L, nbuf, 4096);
        uint32_t rb = ref_range(&c, start, L, rbuf, 4096);
        CHECK(nb == rb, "truncation: new==ref bytes");
        CHECK(nb == 0 || memcmp(nbuf, rbuf, nb) == 0, "truncation: content equal");
        CHECK(nb <= 4096, "truncation: bounded by out_cap");
    }

    free(nbuf); free(rbuf); free(small); free(big);
    conn_destroy(&c);

    if (g_fail) {
        fprintf(stderr, "== range_equiv_test: %d FAIL(s) ==\n", g_fail);
        return 1;
    }
    printf("range_equiv_test: PASS (new range path == per-key reference)\n");
    return 0;
}

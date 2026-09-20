/*
 * bench/sr_equiv_test.c — SR 位图快路径等价性闸门（第 4B/P0 验收）
 * ------------------------------------------------------------------
 * 验证 conn_retransmit_set（位图快路径）与 conn_retransmit_set_ref（O(N) 参考路径）
 * 对同一请求 PSN 集输出【返回字节数 + 输出缓冲】逐字节一致、升序、相同截断语义。
 *
 * 覆盖：
 *   主闸门：10^5 组随机 (N ∈ {128,256,512,4096,5120} × n ∈ {1,2,16,64} ×
 *           out_cap ∈ {足够, 截断到一半})，psn 集在 [0,N) 随机；满窗 [0,N) 全命中。
 *   全 miss：请求 [N, 2N)，位图扫槽全部 psn 不匹配 → 0 输出，验证 miss 跳过路径。
 *   四类边界：① span == N；② n == 1；③ out_cap 中途截断；④ 卫条件触发（24-bit 跨界）。
 *
 * 打印「快路径命中率」：随机 [0,N) 集下 span≤N 且无 24-bit 跨界 ⇒ 应 100% 走位图快路径。
 */
#include "dynblock.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0;
#define CHECK(c, m) do { \
    if (!(c)) { g_fail++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, m); } \
} while (0)

#define PL 256u                   /* 定长包：输出可逐块解码 + memcmp 可比 */
#define OUT_CAP_BIG (1u << 20)    /* 足够，无截断 */

static void fill(uint8_t *b, uint32_t psn) {
    memcpy(b, &psn, 4);                              /* 前 4 字节 = psn（诊断用） */
    for (uint32_t k = 4; k < PL; k++) b[k] = (uint8_t)((psn + k) & 0xFFu);
}

/* 复刻 conn_retransmit_set 的卫条件，用于统计「快路径命中率」（不介入被测代码）。 */
static int fast_path_ok(uint32_t lo, uint32_t span, uint32_t N) {
    if (span > CFG_SR_BM_MAX_N) return 0;
    if (span > N) return 0;
    if (((lo & 0xFFFFFFu) + span) > 0x1000000u) return 0;
    return 1;
}

/* 对一组 psn 跑 new 与 ref，逐字节比较输出 + 返回字节数。 */
static void compare_once(conn_t *c, const uint32_t *psns, uint32_t n,
                         uint8_t *nbuf, uint8_t *rbuf, uint32_t out_cap,
                         const char *tag) {
    uint32_t nw = (uint32_t)conn_retransmit_set(c, psns, n, nbuf, out_cap);
    uint32_t rw = (uint32_t)conn_retransmit_set_ref(c, psns, n, rbuf, out_cap);
    if (nw != rw) {
        g_fail++;
        fprintf(stderr, "FAIL [%s] bytes new=%u ref=%u\n", tag, nw, rw);
        return;
    }
    if (nw && memcmp(nbuf, rbuf, nw) != 0) {
        g_fail++;
        fprintf(stderr, "FAIL [%s] content differs (%u bytes)\n", tag, nw);
        return;
    }
}

int main(void) {
    cfg_t cfg; cfg_default(&cfg);
    const uint32_t N_LIST[] = {128, 256, 512, 4096, 5120};
    const uint32_t N_N   = (uint32_t)(sizeof(N_LIST) / sizeof(N_LIST[0]));
    const uint32_t n_LIST[] = {1, 2, 16, 64};
    const uint32_t N_N2  = (uint32_t)(sizeof(n_LIST) / sizeof(n_LIST[0]));
    const uint32_t TRIALS_PER = 5000;   /* 每 (N, n, capmode) 组；5×4×2×5000 = 200k 组 */

    uint8_t  *nbuf = (uint8_t *)malloc(OUT_CAP_BIG);
    uint8_t  *rbuf = (uint8_t *)malloc(OUT_CAP_BIG);
    uint8_t  *pbuf = (uint8_t *)malloc(PL);
    uint32_t *psns = (uint32_t *)malloc(64 * sizeof(uint32_t));
    if (!nbuf || !rbuf || !pbuf || !psns) { fprintf(stderr, "OOM\n"); return 1; }

    uint64_t fast_cnt = 0, total_cnt = 0;

    for (uint32_t ni = 0; ni < N_N; ni++) {
        uint32_t N = N_LIST[ni];
        cfg.ring_n = N;
        cfg.adaptive_enable = 0;
        conn_t c; conn_init(&c, &cfg, PL);   /* S = ceil_class(PL)，len=PL ≤ S ⇒ 全 IN_RING */

        /* 满窗 [0, N)：逐槽命中，无淘汰、无溢出 */
        for (uint32_t psn = 0; psn < N; psn++) {
            fill(pbuf, psn);
            CHECK(conn_store(&c, psn, pbuf, PL) == R_OK, "populate [0,N)");
        }

        for (uint32_t mi = 0; mi < N_N2; mi++) {
            uint32_t n = n_LIST[mi];
            for (uint32_t capmode = 0; capmode < 2; capmode++) {
                uint32_t out_cap = (capmode == 0) ? OUT_CAP_BIG : (n * PL) / 2u;
                if (out_cap == 0) out_cap = 1u;
                xorshift32_t r; xorshift32_seed(&r, 42u + N * 131u + n * 17u + capmode);
                for (uint32_t t = 0; t < TRIALS_PER; t++) {
                    for (uint32_t k = 0; k < n; k++) psns[k] = xorshift32_next(&r) % N;
                    uint32_t lo = psns[0], hi = psns[0];
                    for (uint32_t k = 1; k < n; k++) {
                        if (psns[k] < lo) lo = psns[k];
                        if (psns[k] > hi) hi = psns[k];
                    }
                    uint32_t span = hi - lo + 1u;
                    total_cnt++;
                    if (fast_path_ok(lo, span, N)) fast_cnt++;
                    compare_once(&c, psns, n, nbuf, rbuf, out_cap, "random");
                }
            }
            /* 全 miss：请求 [N, 2N)，位图扫槽全部 psn 不匹配 → 0 输出（验证 miss 跳过路径） */
            {
                xorshift32_t r; xorshift32_seed(&r, 7u + N + n);
                for (uint32_t k = 0; k < n; k++) psns[k] = N + (xorshift32_next(&r) % N);
                compare_once(&c, psns, n, nbuf, rbuf, OUT_CAP_BIG, "all-miss");
            }
        }

        /* 边界① span == N：psns = {0, N-1} */
        { uint32_t r2[2] = {0u, N - 1u}; compare_once(&c, r2, 2, nbuf, rbuf, OUT_CAP_BIG, "span==N"); }
        /* 边界② n == 1：psns = {N/2} */
        { uint32_t r1[1] = {N / 2u}; compare_once(&c, r1, 1, nbuf, rbuf, OUT_CAP_BIG, "n==1"); }
        /* 边界③ out_cap 中途截断：psns 升序 {0..63}，out_cap=32*PL（一半） */
        {
            uint32_t r3[64];
            for (uint32_t k = 0; k < 64; k++) r3[k] = k;
            compare_once(&c, r3, 64, nbuf, rbuf, 32u * PL, "truncate");
        }

        conn_destroy(&c);
    }

    /* 边界④ 卫条件触发：lo 取 0xFFFFF0 附近使 24-bit 跨界（N=4096）。
     * 在 2^24 边界两侧各种 16 个包（0xFFFFF0..0xFFFFFF 与 0x1000000..0x100000F），
     * 请求 span=32 跨越 2^24 ⇒ (lo&0xFFFFFF)+span > 0x1000000，卫触发退 ref。 */
    {
        uint32_t N = 4096u;
        cfg.ring_n = N; cfg.adaptive_enable = 0;
        conn_t c; conn_init(&c, &cfg, PL);
        for (uint32_t psn = 0; psn < N; psn++) { fill(pbuf, psn); conn_store(&c, psn, pbuf, PL); }
        for (uint32_t k = 0; k < 16; k++) { fill(pbuf, 0xFFFFF0u + k); conn_store(&c, 0xFFFFF0u + k, pbuf, PL); }
        for (uint32_t k = 0; k < 16; k++) { fill(pbuf, 0x1000000u + k); conn_store(&c, 0x1000000u + k, pbuf, PL); }
        uint32_t r4[32];
        for (uint32_t k = 0; k < 16; k++) r4[k]      = 0xFFFFF0u + k;
        for (uint32_t k = 0; k < 16; k++) r4[16 + k] = 0x1000000u + k;
        CHECK(!fast_path_ok(0xFFFFF0u, 32u, N), "guard: 24-bit cross must NOT take fast path");
        compare_once(&c, r4, 32, nbuf, rbuf, OUT_CAP_BIG, "guard-24bit");
        conn_destroy(&c);
    }

    free(nbuf); free(rbuf); free(pbuf); free(psns);

    double fp_rate = total_cnt ? 100.0 * (double)fast_cnt / (double)total_cnt : 0.0;
    printf("sr_equiv_test: %llu random trials, fast-path hit rate = %.2f%%\n",
           (unsigned long long)total_cnt, fp_rate);

    if (g_fail) {
        fprintf(stderr, "== sr_equiv_test: %d FAIL(s) ==\n", g_fail);
        return 1;
    }
    printf("sr_equiv_test: PASS (conn_retransmit_set == conn_retransmit_set_ref byte-identical)\n");
    return 0;
}

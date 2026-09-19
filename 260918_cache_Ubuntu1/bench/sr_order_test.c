/*
 * bench/sr_order_test.c — SR 统一交付契约（第 4B 验收）
 * ------------------------------------------------------------------
 * 验证 retrieve_set 输出「按 PSN 升序」：对 fifo_bounded / chained_hash_bounded /
 * balanced_tree_bounded / psn_dynblock 各存 3N 个定长包（PSN 0..3N-1，驻留窗 [2N,3N)），
 * 再用乱序 PSN 集（8 下界 miss + 16 降序 hit + 8 上界 miss）调 retrieve_set，
 * 断言：输出字节数 == 命中数 × PL，且逐 PL 字节块解码出的 PSN 严格升序、恰为命中集的升序排列。
 *
 * 三种口径应产出同一契约（升序）：
 *   fifo/hash  显式 qsort（sorted_retrieve_set）
 *   tree       中序遍历零排序（tree_retrieve_set / tree_bounded_retrieve_set）
 *   psn_dynblock 位置映射零排序扫槽（conn_retransmit_set）
 */
#include "baseline.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0;

#define PL 256u                 /* 定长包：输出可逐块解码出 PSN */
#define OUT_CAP (1u << 20)

static void fill(uint8_t *b, uint32_t psn) {
    memcpy(b, &psn, 4);                          /* 前 4 字节 = psn（解码用） */
    for (uint32_t k = 4; k < PL; k++) b[k] = (uint8_t)((psn + k) & 0xFFu);
}
static uint32_t decode_psn(const uint8_t *b) {
    uint32_t p; memcpy(&p, b, 4); return p;
}

static void check_sr_ascending(const char *tag, b_cache_t *c, uint32_t N) {
    uint8_t payload[PL];
    for (uint32_t psn = 0; psn < 3 * N; psn++) {   /* 驻留窗 = [2N, 3N) */
        fill(payload, psn);
        c->ops.store(c, psn, payload, PL);
    }

    /* 乱序请求：8 miss(下界 [2N-8,2N)) + 16 hit(降序 [3N-1..3N-16]) + 8 miss(上界 [3N,3N+8)) */
    uint32_t req[32]; uint32_t idx = 0;
    for (uint32_t k = 0; k < 8; k++)  req[idx++] = 2 * N - 8 + k;
    for (uint32_t k = 0; k < 16; k++) req[idx++] = 3 * N - 1 - k;
    for (uint32_t k = 0; k < 8; k++)  req[idx++] = 3 * N + k;

    uint8_t *out = (uint8_t *)malloc(OUT_CAP);
    uint32_t w = c->ops.retrieve_set(c, req, 32, out, OUT_CAP);

    const uint32_t expect_hits = 16;
    if (w != expect_hits * PL) {
        g_fail++; fprintf(stderr, "FAIL [%s] output len %u != %u\n", tag, w, expect_hits * PL);
        free(out); c->ops.destroy(c); return;
    }
    for (uint32_t i = 0; i < expect_hits; i++) {
        uint32_t p = decode_psn(out + (size_t)i * PL);
        uint32_t want = 3 * N - 16 + i;           /* 升序 [3N-16, 3N-1] */
        if (p != want) {
            g_fail++; fprintf(stderr, "FAIL [%s] chunk %u psn=%u want=%u (升序契约违反)\n",
                              tag, i, p, want);
            break;
        }
    }
    free(out);
    c->ops.destroy(c);
}

int main(void) {
    const uint32_t N = 256u;

    check_sr_ascending("fifo_bounded",         make_fifo_bounded(N, PL), N);
    check_sr_ascending("chained_hash_bounded", make_chained_hash_bounded(N, 512u, PL), N);
    check_sr_ascending("balanced_tree_bounded", make_balanced_tree_bounded(N, PL), N);

    cfg_t cfg; cfg_default(&cfg);
    cfg.ring_n = N;
    cfg.adaptive_enable = 0;                     /* 固定 S=256，无 resize 非确定 */
    check_sr_ascending("psn_dynblock", make_dynblock(&cfg, PL), N);

    if (g_fail) {
        fprintf(stderr, "== sr_order_test: %d FAIL(s) ==\n", g_fail);
        return 1;
    }
    printf("sr_order_test: PASS (retrieve_set 按 PSN 升序交付：fifo/hash/tree/dynblock)\n");
    return 0;
}

/*
 * bench/selftest.c — psn_dynblock 自测
 * ------------------------------------------------------------------
 * Step 3：pool 段 —— 环池两路径 round-trip、溢出块分配/释放/stamp、pool_free 幂等。
 * Step 4/5：算法段 ——
 *   A1(phi 一致) A2(store/lookup O(1)) A3(环路径 0 malloc) A4(cur_live+old_live==环内有效槽)
 *   A5(池数≤2 + 旧池自然排空) A6(resize 后 S>=当前代环内包长) A7(drain 清空+可查)
 *   A8(24-bit 回绕) A9(lookup 不删) A10(缩块后大包可查) A11(order_guard)
 *   A16(drain 不覆盖) + ③(len>SC_MAX 残留) + ⑥(护栏成本微测量)。
 *
 * 编译（Step 11 前的临时命令行）：
 *   gcc -O2 -Wall -Wextra -Wmissing-field-initializers -std=gnu11 -Iinclude -pthread \
 *       src/config.c src/util.c src/pool.c src/dynblock.c bench/selftest.c -o build/selftest -lm
 */
#include "dynblock.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { g_fail++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); } \
} while (0)

/* ---- 通用辅助 ---- */
static void fill_payload(uint8_t *buf, uint32_t psn, uint32_t len) {
    for (uint32_t k = 0; k < len; k++) buf[k] = (uint8_t)((psn + k) & 0xFFu);
}
static int check_lookup(conn_t *c, uint32_t psn, uint32_t expect_len) {
    uint16_t len = 0;
    const uint8_t *p = conn_lookup(c, psn, &len);
    if (!p || len != expect_len) return 0;
    for (uint32_t k = 0; k < expect_len; k++)
        if (p[k] != (uint8_t)((psn + k) & 0xFFu)) return 0;
    return 1;
}
static int live_invariant_ok(const conn_t *c) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < c->N; i++)
        if (c->meta[i].flag == SLOT_IN_RING) n++;
    return n == c->cur_live + c->old_live;
}
/* A6：当前代(cur.gen)环内活包 len 是否全部 <= S（旧池包用旧 stride，不受新 S 约束）。
 * 注意：只查 flag==SLOT_IN_RING —— IN_OVF 项（含 drain 后残留/跳过的）不经 pool_for，
 * 其 meta.gen 可能是旧代；若把 flag 放宽到「所有活槽」会误报。将来改动须保持「只查 IN_RING」。 */
static int curgen_len_ok(const conn_t *c) {
    for (uint32_t i = 0; i < c->N; i++)
        if (c->meta[i].flag == SLOT_IN_RING && c->meta[i].gen == c->cur.gen && c->meta[i].len > c->S)
            return 0;
    return 1;
}

/* ---- 环池：mmap / malloc 两路径 + 读写 round-trip ---- */
static void test_pool(void) {
    pool_t p1, p2;
    const uint32_t N = 1024, S = 1500;

    CHECK(sizeof(struct mem_block_header) == 24, "hdr struct historical 24 B (no longer in stride)");
    CHECK(HDR_SZ == 24, "HDR_SZ == 24 (historical; stride excludes it)");
    CHECK(sizeof(slot_meta_t) == 12, "slot_meta_t natural 12 B");
    CHECK(stride_of(S) == 1504, "stride_of(1500) == align16(1500) == 1504");

    CHECK(pool_alloc(&p1, N, S, 1) == 0, "pool_alloc mmap");
    CHECK(p1.use_mmap == 1, "mmap backing recorded");
    CHECK(p1.N == N && p1.S == S && p1.stride == stride_of(S), "pool fields");
    CHECK(pool_alloc(&p2, N, 40, 0) == 0, "pool_alloc malloc");
    CHECK(p2.use_mmap == 0, "malloc backing recorded");
    CHECK(p2.stride == stride_of(40) && stride_of(40) == 48, "stride_of(40)==48");

    for (uint32_t i = 0; i < N; i++) {
        uint8_t *pl = payload_of(&p1, i);
        memset(pl, (int)(i * 7u & 0xFF), S);
    }
    int ok = 1;
    for (uint32_t i = 0; i < N; i++) {
        uint8_t *pl = payload_of(&p1, i);
        if (pl[0] != (int)(i * 7u & 0xFF)) { ok = 0; break; }
        if (pl != slot(&p1, i)) { ok = 0; break; }   /* payload 即槽基址（无 24 B 头） */
    }
    CHECK(ok, "ring round-trip + payload==slot 同构");

    pool_free(&p1);
    pool_free(&p2);
    CHECK(p1.base == NULL && p2.base == NULL, "pool_free clears base");
}

/* ---- 溢出块：分配 / 释放 / stamp / 复用 / scan_max ---- */
static void test_ovf(const cfg_t *cfg) {
    conn_t c;
    memset(&c, 0, sizeof(c));
    c.cfg = cfg;
    c.ovf = (ovf_entry_t *)calloc(cfg->ovf_cap, sizeof(ovf_entry_t));
    CHECK(c.ovf != NULL, "ovf array alloc");

    ovf_reset(&c);
    CHECK(c.ovf_count == 0 && c.ovf_max_hist == 0, "ovf_reset");

    uint8_t payload[600];
    for (int i = 0; i < 600; i++) payload[i] = (uint8_t)(i * 13u & 0xFF);

    int oi0 = ovf_alloc(&c, 100, payload, 300);
    int oi1 = ovf_alloc(&c, 101, payload, 500);
    int oi2 = ovf_alloc(&c, 102, payload, 128);
    CHECK(oi0 == 0 && oi1 == 1 && oi2 == 2, "ovf_alloc indices sequential");
    CHECK(c.ovf_count == 3, "ovf_count==3");
    CHECK(c.n_ovf_alloc == 3 && c.n_ovf_malloc == 3, "n_ovf_alloc==n_ovf_malloc==3");
    CHECK(ovf_scan_max(&c) == 500, "ovf_scan_max==500");

    {
        ovf_entry_t *e = &c.ovf[oi1];
        CHECK(ovf_payload(e) == e->blk, "ovf payload == blk（无 24 B 头）");
        CHECK(memcmp(ovf_payload(e), payload, e->len) == 0, "ovf payload round-trip");
    }

    ovf_release(&c, (uint32_t)oi1);
    CHECK(c.ovf_count == 2 && c.ovf[oi1].used == 0 && c.ovf[oi1].blk == NULL,
          "ovf_release clears used/blk");
    CHECK(ovf_scan_max(&c) == 300, "scan_max skips released slot");

    int oi3 = ovf_alloc(&c, 103, payload, 64);
    CHECK(oi3 == oi1, "reuse released slot");
    CHECK(c.ovf_count == 3, "ovf_count back to 3");

    for (uint32_t oi = 0; oi < cfg->ovf_cap; oi++)
        if (c.ovf[oi].used) ovf_release(&c, oi);
    CHECK(c.ovf_count == 0, "all released");
    free(c.ovf);
}

/* ---- pool_free 幂等 + base==NULL 不崩 ---- */
static void test_pool_free_robust(void) {
    pool_t p;
    memset(&p, 0, sizeof(p));
    pool_free(&p);
    CHECK(p.base == NULL, "pool_free(NULL base) no-op");

    CHECK(pool_alloc(&p, 256, 64, 1) == 0, "pool_alloc for double-free test");
    pool_free(&p);
    pool_free(&p);
    CHECK(p.base == NULL, "pool_free idempotent");
}

/* ==================== 算法自测 ==================== */

/* A1：phi 一致（随机 10^6 次，与 (psn & 0xFFFFFF) % N 相等） */
static void test_A1_phi(void) {
    cfg_t cfg; cfg_default(&cfg);
    conn_t c; conn_init(&c, &cfg, 4096);

    xorshift32_t r; xorshift32_seed(&r, 42u);
    int ok = 1;
    for (uint32_t k = 0; k < 1000000; k++) {
        uint32_t psn = xorshift32_next(&r);
        if (phi(&c, psn) != (psn & 0xFFFFFFu) % c.N) { ok = 0; break; }
    }
    CHECK(ok, "A1: phi == (psn & 0xFFFFFF) %% N for 10^6 random psn");

    conn_destroy(&c);
}

/* A8：24-bit PSN 回绕 —— p 与 p+2^24 同槽，后者覆盖前者 */
static void test_A8_wrap(void) {
    cfg_t cfg; cfg_default(&cfg);
    conn_t c; conn_init(&c, &cfg, 4096);

    uint32_t wp = 0x00ABCDEFu;
    uint8_t b[1024];
    fill_payload(b, wp, 1024);
    CHECK(conn_store(&c, wp, b, 1024) == R_OK, "A8: store pre-wrap");
    fill_payload(b, wp + 0x1000000u, 1024);
    CHECK(conn_store(&c, wp + 0x1000000u, b, 1024) == R_OK, "A8: store post-wrap (same slot)");
    uint16_t tl;
    CHECK(conn_lookup(&c, wp, &tl) == NULL, "A8: pre-wrap evicted by post-wrap");
    CHECK(check_lookup(&c, wp + 0x1000000u, 1024), "A8: post-wrap retrievable");

    conn_destroy(&c);
}

/* A2：store/lookup O(1)（不随 N 增长）。
 * O(1) 是静态性质（conn_store/conn_lookup 内无任何随 N 的循环，Step 6 静态核对确认），
 * 此处只报告计时；N=256→10240 的延迟比是 L3 容量效应（10240 槽环 ≈42MB 超出 L3），非算法量级。 */
static double bench_store_ns(uint32_t N, uint32_t guard) {
    cfg_t cfg; cfg_default(&cfg);
    cfg.ring_n = N;
    cfg.adaptive_enable = 0;   /* 隔离自适应检查，纯测 store 热路径 */
    cfg.order_guard = guard;
    conn_t c; conn_init(&c, &cfg, 4096);
    uint8_t b[1024]; fill_payload(b, 0, 1024);
    uint32_t psn = 0;
    for (int k = 0; k < 200000; k++) conn_store(&c, psn++, b, 1024);   /* 预热 */
    uint64_t t0 = rdtsc_raw();
    uint64_t acc = 0;
    for (int k = 0; k < 1000000; k++) acc ^= (uint32_t)conn_store(&c, psn++, b, 1024);
    uint64_t t1 = rdtsc_raw();
    g_sink ^= acc;
    conn_destroy(&c);
    return to_ns(t1 - t0) / 1000000.0;
}
static void test_A2_oid(void) {
    tsc_calibrate();
    double t_small = 1e9, t_large = 1e9;
    for (int r = 0; r < 3; r++) {
        double a = bench_store_ns(256, 0);
        double b = bench_store_ns(10240, 0);
        if (a < t_small) t_small = a;
        if (b < t_large) t_large = b;
    }
    printf("A2: store ns/op (best-of-3, 1024B, 全环扫)  N=256=%.2f  N=10240=%.2f  ratio=%.2f\n"
           "    (ratio>1 为 L3 容量效应；store 无随 N 的循环，算法量级 O(1))\n",
           t_small, t_large, t_large / t_small);
}

/* A3：稳态环路径 malloc/store == 0（对照：溢出路径确实 malloc） */
static void test_A3_malloc_zero(void) {
    cfg_t cfg; cfg_default(&cfg);
    cfg.ring_n = 256;
    conn_t c; conn_init(&c, &cfg, 4096);

    uint8_t b[4096];
    for (uint32_t k = 0; k < 256; k++) {
        fill_payload(b, k, 1024);
        conn_store(&c, k, b, 1024);
    }
    CHECK(c.n_ovf_alloc == 0 && c.n_ovf_malloc == 0, "A3: ring path 0 malloc/0 ovf_alloc");

    for (uint32_t k = 0; k < 256; k++) {   /* 回绕淘汰一圈，仍是稳态 */
        fill_payload(b, k + 256, 1024);
        conn_store(&c, k + 256, b, 1024);
    }
    CHECK(c.n_ovf_alloc == 0 && c.n_ovf_malloc == 0, "A3: steady-state wrap still 0 malloc");

    uint8_t big[8192];
    fill_payload(big, 512, 8192);
    conn_store(&c, 512, big, 8192);
    CHECK(c.n_ovf_alloc == 1 && c.n_ovf_malloc == 1, "A3: overflow path DOES malloc (sanity)");

    conn_destroy(&c);
}

/* A4：cur_live + old_live == 环内有效槽数（IN_RING 扫描） */
static void test_A4_live(void) {
    cfg_t cfg; cfg_default(&cfg);
    cfg.ring_n = 256;
    conn_t c; conn_init(&c, &cfg, 4096);

    uint8_t b[8192];
    uint32_t psn = 0;

    for (uint32_t k = 0; k < 200; k++) {
        fill_payload(b, psn, 1024);
        conn_store(&c, psn++, b, 1024);
    }
    CHECK(live_invariant_ok(&c), "A4: ring-only cur_live+old_live == ring_live");

    for (uint32_t k = 0; k < 50; k++) {
        fill_payload(b, psn, 8192);
        conn_store(&c, psn++, b, 8192);
    }
    CHECK(live_invariant_ok(&c), "A4: mixed ring+ovf cur_live+old_live == ring_live");

    conn_destroy(&c);
}

/* A5/A6/A7：溢出达阈值 → 扩 + drain（本场景溢出包全 8192 <= SC_MAX，drain 后无残留） */
static void test_grow_drain(void) {
    cfg_t cfg; cfg_default(&cfg);
    cfg.ring_n = 256;
    conn_t c; conn_init(&c, &cfg, 4096);
    CHECK(c.S == 4096, "grow: init S == 4096");

    uint8_t b[8192];
    uint32_t psn = 0;

    /* 两纪元 8192 包（> S=4096）→ 溢出达阈值 → 扩 + drain。
     * A7 构造说明：纪元1 存 psn 0..255（各占槽 0..255 溢出）；
     * 纪元2 存 psn 256..511（psn 256+k 与 psn k 同槽 → 位置淘汰 psn k）。 */
    for (uint32_t e = 0; e < 2; e++)
        for (uint32_t k = 0; k < 256; k++) {
            fill_payload(b, psn, 8192);
            conn_store(&c, psn++, b, 8192);
        }

    CHECK(c.S == 8192, "A6: S grew to 8192");
    CHECK(c.n_resize == 1, "B-6: n_resize == 1 (real switch only)");
    CHECK(c.ovf_count == 0, "A7: ovf_count == 0 (all 8192 <= SC_MAX, no residual)");
    CHECK(c.has_old == 1 && c.old_live == 0, "A5: 2 pools, old drained (not yet freed)");
    CHECK(live_invariant_ok(&c), "A4: live invariant after drain");

    /* A7：drain 搬回的纪元2 溢出包（psn 256..511）都能查到 */
    int ok = 1;
    for (uint32_t p = 256; p < 512; p++)
        if (!check_lookup(&c, p, 8192)) { ok = 0; break; }
    CHECK(ok, "A7: drained packets (psn 256..511) all retrievable");

    /* A7 反例：psn 0..255 查不到，因纪元2 的位置淘汰（psn 256+k 覆盖同槽 psn k），
     * 与 drain 无关（drain 只搬回、不淘汰任何包）。 */
    ok = 1;
    for (uint32_t p = 0; p < 256; p++) {
        uint16_t tl;
        if (conn_lookup(&c, p, &tl) != NULL) { ok = 0; break; }
    }
    CHECK(ok, "A7: epoch-1 packets gone (positional eviction, not drain)");

    CHECK(curgen_len_ok(&c), "A6: S >= all cur.gen ring lengths after resize");

    fill_payload(b, psn, 8192);
    conn_store(&c, psn++, b, 8192);
    CHECK(c.has_old == 0, "A5: old pool freed → 1 pool");

    conn_destroy(&c);
}

/* A5 真考验：切换瞬间环内有活包 → old_live>0 → 随覆盖递减 → 到 0 才释放旧池 */
static void test_A5_old_drains(void) {
    cfg_t cfg; cfg_default(&cfg);
    cfg.ring_n = 1024;
    cfg.k_dwell = 1;          /* 一次检查即切换，受控 */
    conn_t c; conn_init(&c, &cfg, 4096);

    uint8_t b[8192];
    uint32_t psn = 0;

    for (uint32_t k = 0; k < 1024; k++) {   /* 填满一圈 1024 包 → cur_live=1024 */
        fill_payload(b, psn, 1024);
        conn_store(&c, psn++, b, 1024);
    }
    CHECK(c.cur_live == 1024 && c.has_old == 0, "A5: ring full, 1 pool");

    for (uint32_t k = 0; k < 256; k++) {    /* 256 个 8192 包淘汰槽 0..255 */
        fill_payload(b, psn, 8192);
        conn_store(&c, psn++, b, 8192);
    }
    CHECK(c.ovf_count == 256 && c.cur_live == 768, "A5: 256 overflow, cur_live=768");

    conn_force_check(&c);                    /* 扩（k_dwell=1） */
    CHECK(c.S == 8192, "A5: grew to 8192");
    CHECK(c.n_resize == 1, "A5: one resize");
    CHECK(c.has_old == 1 && c.old_live == 768, "A5: old_live == cur_live at switch (768)");
    CHECK(c.cur_live == 256, "A5: new pool holds 256 drained overflow pkts");

    for (uint32_t k = 0; k < 767; k++) {    /* 覆盖旧池槽 256..1022 */
        fill_payload(b, psn, 1024);
        conn_store(&c, psn++, b, 1024);
    }
    CHECK(c.has_old == 1 && c.old_live == 1, "A5: old_live==1, pool not yet freed");

    fill_payload(b, psn, 1024);             /* 第 768 个覆盖 → old_live 到 0 → 释放旧池 */
    conn_store(&c, psn++, b, 1024);
    CHECK(c.has_old == 0 && c.old_live == 0, "A5: old pool freed only when drained to 0");

    conn_destroy(&c);
}

/* A10：缩块后大包仍可查（A6 限定 cur.gen） */
static void test_A10_shrink(void) {
    cfg_t cfg; cfg_default(&cfg);
    cfg.ring_n = 256;
    conn_t c; conn_init(&c, &cfg, 4096);
    CHECK(c.S == 4096, "shrink: init S == 4096");

    uint8_t b[8192];
    uint32_t psn = 0;

    /* 5 个安静纪元全 1024 → 缩到 1024（j_quiet=4, k_dwell=2） */
    for (uint32_t e = 0; e < 5; e++)
        for (uint32_t k = 0; k < 256; k++) {
            fill_payload(b, psn, 1024);
            conn_store(&c, psn++, b, 1024);
        }
    CHECK(c.S == 1024, "A10: S shrank to 1024");
    CHECK(c.n_resize == 1, "A10: exactly one shrink");
    CHECK(live_invariant_ok(&c), "A4: live invariant after shrink");
    CHECK(curgen_len_ok(&c), "A6: S >= all cur.gen ring lengths after shrink");

    fill_payload(b, psn, 4096);
    CHECK(conn_store(&c, psn, b, 4096) == R_OK, "A10: big packet stored to overflow");
    CHECK(check_lookup(&c, psn, 4096), "A10: big packet retrievable after shrink");

    conn_destroy(&c);
}

/* A9：同一 PSN 反复 lookup 结果一致且不被删除 */
static void test_A9_lookup(void) {
    cfg_t cfg; cfg_default(&cfg);
    conn_t c; conn_init(&c, &cfg, 4096);

    uint8_t b[2048];
    uint32_t psn = 1000;
    fill_payload(b, psn, 1024);
    CHECK(conn_store(&c, psn, b, 1024) == R_OK, "A9: store");

    for (int rep = 0; rep < 5; rep++)
        CHECK(check_lookup(&c, psn, 1024), "A9: repeated lookup consistent");

    uint32_t i = phi(&c, psn);
    CHECK(c.meta[i].flag == SLOT_IN_RING && c.meta[i].psn == psn,
          "A9: still cached (lookup does not delete)");

    conn_destroy(&c);
}

/* A11：order_guard —— p+N 先到、p 后到 → p 丢弃、p+N 保留 */
static void test_A11_order_guard(void) {
    cfg_t cfg; cfg_default(&cfg);
    cfg.ring_n = 256;
    cfg.order_guard = 1;
    conn_t c; conn_init(&c, &cfg, 4096);

    uint8_t b[2048];
    uint32_t p = 1000;
    uint32_t pN = p + cfg.ring_n;   /* 同一槽 */

    fill_payload(b, pN, 1024);
    CHECK(conn_store(&c, pN, b, 1024) == R_OK, "A11: store p+N");
    CHECK(check_lookup(&c, pN, 1024), "A11: p+N retrievable");

    fill_payload(b, p, 1024);
    CHECK(conn_store(&c, p, b, 1024) == R_DROPPED, "A11: older p dropped");
    CHECK(c.n_ooo_drop == 1, "A11: n_ooo_drop == 1");
    CHECK(c.epoch_stores == 2, "A11: drop path still advances epoch_stores (1 store + 1 drop)");
    CHECK(check_lookup(&c, pN, 1024), "A11: p+N still retrievable");
    uint16_t tl;
    CHECK(conn_lookup(&c, p, &tl) == NULL, "A11: p returns NULL");

    conn_destroy(&c);
}

/* A16（② 选项 a）：drain 绝不覆盖新池中更新的包 / 其他包。
 * 选项(a)下 gen_switch 与 drain 同临界区、新池空 ⇒ 「更新包被覆盖」不可能发生；
 * 此处验证搬回后新池每槽 psn 精确等于对应溢出项、内容正确，且旧池未被 drain 触碰的槽仍可读。 */
static void test_A16_drain_no_overwrite(void) {
    cfg_t cfg; cfg_default(&cfg);
    cfg.ring_n = 1024;
    cfg.k_dwell = 1;
    conn_t c; conn_init(&c, &cfg, 4096);

    uint8_t b[8192];
    uint32_t psn = 0;

    for (uint32_t k = 0; k < 1024; k++) {   /* 填满一圈 1024 包（全 IN_RING） */
        fill_payload(b, psn, 1024);
        conn_store(&c, psn++, b, 1024);
    }
    for (uint32_t k = 0; k < 256; k++) {    /* 前 256 槽改成溢出（8192） */
        fill_payload(b, psn, 8192);
        conn_store(&c, psn++, b, 8192);
    }
    conn_force_check(&c);                    /* 扩 + drain：溢出包搬进新池槽 0..255 */

    int ok = 1;
    for (uint32_t i = 0; i < 256; i++) {
        slot_meta_t *m = &c.meta[i];
        if (!(m->flag == SLOT_IN_RING && m->gen == c.cur.gen && m->psn == 1024 + i)) { ok = 0; break; }
    }
    CHECK(ok, "A16: new pool slots 0..255 == drained psn 1024..1279 (no overwrite)");
    for (uint32_t i = 0; i < 256; i++)
        if (!check_lookup(&c, 1024 + i, 8192)) { ok = 0; break; }
    CHECK(ok, "A16: drained packets content correct");

    ok = 1;
    for (uint32_t i = 256; i < 1024; i++)
        if (!check_lookup(&c, i, 1024)) { ok = 0; break; }
    CHECK(ok, "A16: untouched old-gen slots (256..1023) still readable via old pool");

    conn_destroy(&c);
}

/* ③：len > SC_MAX(8192) 的溢出项搬不回（S 封顶 8192），留在溢出区，计 n_drain_residual */
static void test_drain_residual(void) {
    cfg_t cfg; cfg_default(&cfg);
    cfg.ring_n = 256;
    conn_t c; conn_init(&c, &cfg, 4096);

    uint8_t b[10032];
    uint32_t psn = 0;

    for (uint32_t e = 0; e < 2; e++)        /* 两纪元 10000 包 → 扩，但 S 只能到 8192 → 全残留 */
        for (uint32_t k = 0; k < 256; k++) {
            fill_payload(b, psn, 10000);
            conn_store(&c, psn++, b, 10000);
        }

    CHECK(c.S == 8192, "residual: S capped at SC_MAX=8192 (ceil_class(10000)=8192)");
    CHECK(c.n_drain_residual == 256, "residual: 256 items stayed in overflow (len>S)");
    CHECK(c.ovf_count == 256, "residual: ovf_count == 256 (none moved)");
    CHECK(check_lookup(&c, psn - 1, 10000), "residual: >SC_MAX packet still retrievable from overflow");

    conn_destroy(&c);
}

/* ⑥ 微测量：order_guard=0 vs 1 的 store 延迟（粗糙，正式数据在 exp1a） */
static void test_guard_cost(void) {
    tsc_calibrate();
    double t0 = 1e9, t1 = 1e9;
    for (int r = 0; r < 3; r++) {
        double a = bench_store_ns(10240, 0);
        double b = bench_store_ns(10240, 1);
        if (a < t0) t0 = a;
        if (b < t1) t1 = b;
    }
    printf("A11 guard cost: order_guard=0 %.2f ns/store, =1 %.2f ns/store, delta %.2f ns\n",
           t0, t1, t1 - t0);
}

/* ② CLOCK_MONOTONIC 长窗交叉验证：同批 xorshift 操作分别用 wall 与 rdtsc 计时，
 * 返回 rdtsc 相对 wall 的偏差（>10% 则 rdtsc 不可信）。报告用，不硬断（VM 下可能波动）。 */
static void test_tsc_xval(void) {
    double wall_ns = 0.0, tsc_ns = 0.0;
    uint64_t nops = CFG_RDTSC_XVAL_OPS;
    double dev = tsc_cross_validate(nops, &wall_ns, &tsc_ns);
    printf("TSC cross-validate: %llu ops, wall %.4f ns/op, rdtsc %.4f ns/op, dev %+.2f%%\n",
           (unsigned long long)nops, wall_ns, tsc_ns, dev);
}

/* A12：缩块边界与防振荡护栏（j_quiet 门 / gamma 门 / ovf_max_hist 主导 / hist_decay / 压力） */
static void test_A12_antiosc(void) {
    /* 1) quiet_epochs < j_quiet(4) → 不缩（哪怕 L_ring 很小） */
    {
        cfg_t cfg; cfg_default(&cfg);
        cfg.ring_n = 256;
        conn_t c; conn_init(&c, &cfg, 4096);
        uint8_t b[1024];
        uint32_t psn = 0;
        for (int e = 0; e < 3; e++)
            for (uint32_t k = 0; k < 256; k++) { fill_payload(b, psn, 1024); conn_store(&c, psn++, b, 1024); }
        CHECK(c.S == 4096 && c.quiet_epochs == 3, "A12.1: quiet_epochs<4 → no shrink");
        conn_destroy(&c);
    }
    /* 2) L_ring > 0.75·S → 不缩 */
    {
        cfg_t cfg; cfg_default(&cfg);
        cfg.ring_n = 256;
        conn_t c; conn_init(&c, &cfg, 4096);
        uint8_t b[4096];
        uint32_t psn = 0;
        for (int e = 0; e < 6; e++)              /* 3328 > 0.75*4096 = 3072 */
            for (uint32_t k = 0; k < 256; k++) { fill_payload(b, psn, 3328); conn_store(&c, psn++, b, 3328); }
        CHECK(c.S == 4096, "A12.2: L_ring=3328 > 0.75*S=3072 → no shrink");
        conn_destroy(&c);
    }
    /* 3) + 4) ovf_max_hist 主导缩目标；hist_decay 衰减后才允许缩 */
    {
        cfg_t cfg; cfg_default(&cfg);
        cfg.ring_n = 256;
        conn_t c; conn_init(&c, &cfg, 4096);
        uint8_t b[8192];
        uint32_t psn = 0;
        fill_payload(b, psn, 8192);
        conn_store(&c, psn++, b, 8192);
        CHECK(c.ovf_max_hist == 8192, "A12.3: ovf_max_hist==8192 after one big overflow");
        for (uint32_t k = 0; k < 256; k++) {     /* 填一圈，淘汰大包，ovf_count→0，hist 仍 8192 */
            fill_payload(b, psn, 1024);
            conn_store(&c, psn++, b, 1024);
        }
        CHECK(c.ovf_count == 0, "A12.3: big packet evicted, ovf_count==0");

        int guard = 1;
        for (int e = 0; e < 8 && c.quiet_epochs < 8; e++) {
            for (uint32_t k = 0; k < 256; k++) { fill_payload(b, psn, 1024); conn_store(&c, psn++, b, 1024); }
            if (c.S != 4096) guard = 0;
        }
        CHECK(guard && c.ovf_max_hist == 0 && c.S == 4096,
              "A12.4: no shrink before hist_decay; ovf_max_hist==0 at quiet==8");

        for (int e = 0; e < 2; e++)              /* 再 2 个安静纪元（k_dwell=2）→ 才缩到 1024 */
            for (uint32_t k = 0; k < 256; k++) { fill_payload(b, psn, 1024); conn_store(&c, psn++, b, 1024); }
        CHECK(c.S == 1024 && c.n_resize == 1, "A12.4: shrink to 1024 only after hist decay");
        conn_destroy(&c);
    }
    /* 5) 压力测试：交替 1024/4096 各 4 纪元 × 10 轮 → n_resize 有上界（不横跳） */
    {
        cfg_t cfg; cfg_default(&cfg);
        cfg.ring_n = 256;
        conn_t c; conn_init(&c, &cfg, 4096);
        uint8_t b[4096];
        uint32_t psn = 0;
        for (int round = 0; round < 10; round++) {
            for (uint32_t k = 0; k < 4 * 256; k++) { fill_payload(b, psn, 1024); conn_store(&c, psn++, b, 1024); }
            for (uint32_t k = 0; k < 4 * 256; k++) { fill_payload(b, psn, 4096); conn_store(&c, psn++, b, 4096); }
        }
        printf("A12.5: stress 10 rounds → n_resize=%llu (hysteresis-bounded; << 80 epochs)\n",
               (unsigned long long)c.n_resize);
        CHECK(c.n_resize <= 24, "A12.5: n_resize bounded (anti-oscillation; no per-epoch thrash)");
        conn_destroy(&c);
    }
}

/* A14：五种 MTU 建环 —— S0=ceil_class(MTU)、stride=align16(S0)、N 个 MTU 包全进环、0 溢出 */
static void test_A14_mtu_init(void) {
    static const uint32_t MTUS[5] = {256, 512, 1024, 2048, 4096};
    for (int m = 0; m < 5; m++) {
        uint32_t mtu = MTUS[m];
        cfg_t cfg; cfg_default(&cfg);
        cfg.ring_n = 256;
        conn_t c; conn_init(&c, &cfg, mtu);
        CHECK(c.S == ceil_class(&cfg, mtu), "A14: S0 == ceil_class(MTU)");
        CHECK(c.cur.stride == stride_of(ceil_class(&cfg, mtu)), "A14: stride == align16(S0)");

        uint8_t *b = (uint8_t *)malloc(mtu);
        int ok = 1;
        for (uint32_t k = 0; k < 256; k++) {
            fill_payload(b, k, mtu);
            if (conn_store(&c, k, b, mtu) != R_OK) { ok = 0; break; }
        }
        CHECK(ok, "A14: all MTU packets stored");
        CHECK(c.n_ovf_alloc == 0, "A14: all MTU packets in ring (0 overflow)");
        ok = 1;
        for (uint32_t k = 0; k < 256; k++)
            if (!check_lookup(&c, k, mtu)) { ok = 0; break; }
        CHECK(ok, "A14: all MTU packets retrievable");
        free(b);
        conn_destroy(&c);
    }
}

/* A15：SC_NEW 27 档的代数性质 */
static void test_A15_sc_new_algebra(void) {
    cfg_t cfg; cfg_default(&cfg);

    int ok1 = 1, ok2 = 1, ok3 = 1;
    uint32_t prev = 0;
    for (uint32_t x = 0; x <= 8192; x += 7) {   /* 步长 7 覆盖非档位值（SC_MAX=8192 内） */
        uint32_t c = ceil_class(&cfg, x);
        if (c < x) { ok1 = 0; break; }
        if (ceil_class(&cfg, c) != c) { ok2 = 0; break; }
        if (c < prev) { ok3 = 0; break; }
        prev = c;
    }
    CHECK(ok1, "A15.1: ceil_class(x) >= x for x <= SC_MAX(8192)");
    CHECK(ok2, "A15.2: ceil_class idempotent");
    CHECK(ok3, "A15.3: ceil_class monotonic");

    /* 封顶语义：x > SC_MAX 返回最大档 8192（放不下自然走溢出，对应 ③ residual） */
    int okcap = 1;
    for (uint32_t x = 8193; x <= 9000; x++)
        if (ceil_class(&cfg, x) != 8192) { okcap = 0; break; }
    CHECK(okcap, "A15.cap: ceil_class(x)==8192 for x > SC_MAX");

    int ok4 = 1;
    for (uint32_t x = 0; x <= 2048; x++)
        if (ceil_class(&cfg, x) - x > 128) { ok4 = 0; break; }
    CHECK(ok4, "A15.4: worst waste <= 128 B for x <= 2048");

    uint32_t a = ceil_class(&cfg, 700);          /* 700 → 768（sc_new） */
    cfg_t cfg2 = cfg;
    for (uint32_t i = 0; i < CFG_SC_OLD_TIERED_N; i++) cfg2.sc_old_tiered[i] = 9999u;
    CHECK(a == ceil_class(&cfg2, 700), "A15.5: ceil_class only reads sc_new (tiered_old isolated)");
}

/* A17：n_drop 路径 —— ovf_alloc 与 grow_locked 双失败 ⇒ 丢弃（不破坏正确性）。
 * 双失败构造：S 已封顶 SC_MAX=8192 ⇒ len>8192 时 grow_locked 的 ceil_class(len)==8192==S，
 * 不扩；此时再让 ovf_alloc 失败（a 用注入 ovf_cap=0，b 用真实打满 ovf_cap=8）⇒ 走 drop。
 * 正确性：丢弃返回 R_DROPPED、n_drop 计数、live 不变量成立、既有缓存不损坏、丢弃包查不到。 */
static void test_A17_drop_path(void) {
    /* (a) 注入：ovf_cap=0（ovf_alloc 必失败）+ S 封顶 ⇒ 超上限包丢弃 */
    {
        cfg_t cfg; cfg_default(&cfg);
        cfg.ring_n = 256;
        cfg.ovf_cap = 0;                          /* 注入：溢出区不可用 */
        conn_t c; conn_init(&c, &cfg, 8192);      /* S = ceil_class(8192) = 8192 (SC_MAX) */

        uint8_t b[10032];
        uint32_t psn = 0;
        for (uint32_t k = 0; k < 256; k++) {      /* 填满一圈 1024 包（环路径） */
            fill_payload(b, psn, 1024);
            CHECK(conn_store(&c, psn++, b, 1024) == R_OK, "A17a: ring store ok");
        }
        fill_payload(b, psn, 10000);              /* len > SC_MAX ⇒ 溢出路径 */
        CHECK(conn_store(&c, psn, b, 10000) == R_DROPPED, "A17a: oversized dropped");
        CHECK(c.n_drop == 1, "A17a: n_drop == 1");
        CHECK(c.S == 8192, "A17a: S still capped at 8192 (grow_locked no-op)");
        CHECK(live_invariant_ok(&c), "A17a: live invariant preserved");

        int ok = 1;
        for (uint32_t k = 1; k < 256; k++)        /* 除 psn=0（被位置淘汰）外全在 */
            if (!check_lookup(&c, k, 1024)) { ok = 0; break; }
        CHECK(ok, "A17a: psn 1..255 intact after drop");
        uint16_t tl;
        CHECK(conn_lookup(&c, 0, &tl) == NULL, "A17a: psn 0 evicted (out of window)");
        CHECK(conn_lookup(&c, psn, &tl) == NULL, "A17a: dropped psn not cached");
        conn_destroy(&c);
    }
    /* (b) 真实（无注入）：S 封顶 + 溢出真正打满 ovf_cap=8 ⇒ 第 9 个超上限包丢弃 */
    {
        cfg_t cfg; cfg_default(&cfg);
        cfg.ring_n = 256;
        cfg.ovf_cap = 8;                          /* 小溢出区，快速打满 */
        conn_t c; conn_init(&c, &cfg, 8192);      /* S = 8192 */

        uint8_t b[10032];
        uint32_t psn = 0;
        for (uint32_t k = 0; k < 8; k++) {        /* 8 个 len>SC_MAX 包填满溢出 */
            fill_payload(b, psn, 10000);
            CHECK(conn_store(&c, psn++, b, 10000) == R_OK, "A17b: fill overflow");
        }
        CHECK(c.ovf_count == 8, "A17b: ovf_count == 8 (full)");
        fill_payload(b, psn, 10000);              /* 第 9 个：溢出满 + 无法扩 ⇒ 丢弃 */
        CHECK(conn_store(&c, psn, b, 10000) == R_DROPPED, "A17b: overflow-full dropped");
        CHECK(c.n_drop == 1, "A17b: n_drop == 1");
        CHECK(live_invariant_ok(&c), "A17b: live invariant preserved");

        int ok = 1;
        for (uint32_t k = 0; k < 8; k++)
            if (!check_lookup(&c, k, 10000)) { ok = 0; break; }
        CHECK(ok, "A17b: 8 cached packets still retrievable");
        uint16_t tl;
        CHECK(conn_lookup(&c, psn, &tl) == NULL, "A17b: dropped psn not cached");
        conn_destroy(&c);
    }
}

int main(void) {
    cfg_t cfg;
    cfg_default(&cfg);

    /* Step 0：cfg 三处同源往返（dump→load→memcmp）。字段表改名/新增必须在此回归；
     * 早前 cfg_selftest 定义后从未接线，本轮补上（写 build/ 下，make clean 一并清掉）。 */
    if (cfg_selftest(&cfg, "build/.cfg_roundtrip.json") != 0) {
        fprintf(stderr, "== cfg_selftest FAILED (dump/load/memcmp) ==\n");
        g_fail++;
    } else {
        printf("cfg_roundtrip: dump -> load -> memcmp OK\n");
    }

    /* Step 3：pool 层 */
    test_pool();
    test_ovf(&cfg);
    test_pool_free_robust();

    /* Step 4/5：算法层 */
    test_A1_phi();
    test_A8_wrap();
    test_A2_oid();
    test_A3_malloc_zero();
    test_A4_live();
    test_grow_drain();
    test_A5_old_drains();
    test_A10_shrink();
    test_A9_lookup();
    test_A11_order_guard();
    test_A16_drain_no_overwrite();
    test_drain_residual();
    test_A12_antiosc();
    test_A14_mtu_init();
    test_A15_sc_new_algebra();
    test_A17_drop_path();
    test_guard_cost();
    test_tsc_xval();

    if (g_fail) {
        fprintf(stderr, "== %d CHECK(s) FAILED ==\n", g_fail);
        return 1;
    }
    printf("ALL SELF-TESTS PASS (no 24B header; stride=align16(S); slot_meta=%zu B)\n",
           sizeof(slot_meta_t));
    return 0;
}

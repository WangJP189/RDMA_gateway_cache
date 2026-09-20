/*
 * src/config.c — cfg_t 的唯一实现：默认值 + 字段表驱动 JSON + CLI 覆盖 + 自检
 * --------------------------------------------------------------------------
 * 核心纪律（第 2 轮裁决重申）：
 *   任何被 cfg 读取的字段，必须同时满足
 *     (1) 在 include/config.h 的 cfg_t 中声明；
 *     (2) 在 cfg_default() 中赋值；
 *     (3) 在 cfg_dump_json() 中落盘。
 *   三者由 FIELDS[] 字段表 + cfg_default 同源维护；cfg_selftest 用
 *   dump -> load -> memcmp 断言三者一致（含字段个数 == N_FIELDS）。
 *
 * JSON 往返要求逐字节一致：因此 double 一律 %.17g 落盘（strtod 可精确还原），
 * cfg_t 显式加 _pad0/_pad1/_pad2 且 cfg_default/cfg_load_json 都先 memset 归零，
 * 保证任何隐式 padding 在 dump/load 两端恒为 0。
 */
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ==================== 字段表 ==================== */

typedef enum { T_U32, T_U64, T_DBL, T_STR, T_U32A, T_DBLA, T_PHASE } ftype_t;

typedef struct {
    const char *name;   /* JSON 键名 == 结构体字段名（snake_case） */
    size_t      off;    /* offsetof(cfg_t, field) */
    ftype_t     type;
    uint32_t    count;  /* 数组元素个数（标量/字符串 = 1） */
} field_t;

#define F(f, t, n) { #f, offsetof(cfg_t, f), (t), (n) }

static const field_t FIELDS[] = {
    /* ---- 机制 ---- */
    F(ring_n,                 T_U32,   1),
    F(sc_new,                 T_U32A,  CFG_SC_NEW_N),
    F(n_sc_new,               T_U32,   1),
    F(sc_old_tiered,          T_U32A,  CFG_SC_OLD_TIERED_N),
    F(n_sc_old,               T_U32,   1),
    F(hdr_sz,                 T_U32,   1),
    F(ovf_cap,                T_U32,   1),
    F(ovf_thresh,             T_U32,   1),
    F(fallback_mtu,           T_U32,   1),
    F(k_dwell,                T_U32,   1),
    F(j_quiet,                T_U32,   1),
    F(hist_decay,             T_U32,   1),
    F(shrink_gamma,           T_DBL,   1),
    F(adaptive_enable,        T_U32,   1),
    F(single_thread,          T_U32,   1),
    F(pool_use_mmap,          T_U32,   1),
    F(order_guard,            T_U32,   1),
    F(alloc_mode,             T_U32,   1),
    F(sim_link_gbps,          T_U32,   1),
    F(sim_pend_cap,           T_U32,   1),
    /* ---- 计时/复现 ---- */
    F(seed,                   T_U32,   1),
    F(reps,                   T_U32,   1),
    F(tsc_batch_ops,          T_U32,   1),
    F(warmup_ops,             T_U32,   1),
    F(rdtsc_xval_ops,         T_U32,   1),
    /* ---- 实验一 ---- */
    F(e1a_n,                  T_U32,   1),
    F(e1a_payload_list,       T_U32A,  CFG_E1A_PAYLOAD_N),
    F(e1a_payload_n,          T_U32,   1),
    F(e1a_timed_ops,          T_U32,   1),
    F(e1a_batch_ops,          T_U32,   1),
    F(e1a_batch_xval_ops,     T_U32,   1),
    F(e1a_seq_mode,           T_U32,   1),
    F(e1b_n_list,             T_U32A,  CFG_E1B_N_N),
    F(e1b_n_n,                T_U32,   1),
    F(e1b_gbn_long,           T_U32,   1),
    F(e1b_gbn_short,          T_U32,   1),
    F(e1b_sr_k1,              T_U32,   1),
    F(e1b_sr_k2,              T_U32,   1),
    F(e1b_queries,            T_U32,   1),
    F(e1b_payload_list,       T_U32A,  CFG_E1B_PAYLOAD_N),
    F(e1b_payload_n,          T_U32,   1),
    F(e1b_batch_ops,          T_U32,   1),
    F(e1b_batch_xval_ops,     T_U32,   1),
    F(e1b_sort_impl,          T_U32,   1),
    F(e1b_tree_inorder,       T_U32,   1),
    /* ---- 实验二 ---- */
    F(e2_pktsize_list,        T_U32A,  CFG_E2_PKTSIZE_N),
    F(e2_pktsize_n,           T_U32,   1),
    F(e2_n_list,              T_U32A,  CFG_E2_N_N),
    F(e2_n_n,                 T_U32,   1),
    F(e2_n,                   T_U32,   1),
    F(e2_fixed_l,             T_U32,   1),
    F(e2_tail_frac_n,         T_U32,   1),
    F(e2_tail_frac_list,      T_DBLA,  CFG_E2_TAIL_FRAC_N),
    F(hash_nbuckets,          T_U32,   1),
    F(fixed_block,            T_U32,   1),
    /* ---- 实验三 ---- */
    F(e3_phase_n,             T_U32,   1),
    F(e3_phases,              T_PHASE, 64),
    F(e3_mixed_small,         T_U32,   1),
    F(e3_mixed_big,           T_U32,   1),
    F(e3_mixed_big_p,         T_DBL,   1),
    F(e3_jitter,              T_DBL,   1),
    F(e3_nak_rate,            T_DBL,   1),
    F(e3_nak_mode,            T_U32,   1),
    F(e3_latency_probe,       T_U32,   1),
    /* ---- 模拟器 ---- */
    F(sim_loss_mode,          T_U32,   1),
    F(sim_nak_delay,          T_U32,   1),
    F(sim_seed,               T_U32,   1),
    F(sim_loss_rate,          T_DBL,   1),
    F(sim_burst_good_p,       T_DBL,   1),
    F(sim_burst_bad_p,        T_DBL,   1),
    /* ---- 运行期 ---- */
    F(out_dir,                T_STR,   1),
};

#define N_FIELDS (sizeof(FIELDS) / sizeof(FIELDS[0]))

static const field_t *field_by_name(const char *name) {
    for (size_t k = 0; k < N_FIELDS; k++)
        if (!strcmp(FIELDS[k].name, name)) return &FIELDS[k];
    return NULL;
}

static const field_t *field_by_cli(const char *key) {
    char buf[128];
    size_t j = 0;
    for (size_t i = 0; key[i] && j + 1 < sizeof(buf); i++)
        buf[j++] = (key[i] == '-') ? '_' : key[i];
    buf[j] = 0;
    return field_by_name(buf);
}

/* 数组字段被 CLI 覆盖时，同步更新配对的元素个数字段。 */
static const char *count_field_for(const char *arr_name) {
    if (!strcmp(arr_name, "sc_new"))           return "n_sc_new";
    if (!strcmp(arr_name, "sc_old_tiered"))    return "n_sc_old";
    if (!strcmp(arr_name, "e1a_payload_list")) return "e1a_payload_n";
    if (!strcmp(arr_name, "e1b_n_list"))       return "e1b_n_n";
    if (!strcmp(arr_name, "e1b_payload_list")) return "e1b_payload_n";
    if (!strcmp(arr_name, "e2_pktsize_list"))  return "e2_pktsize_n";
    if (!strcmp(arr_name, "e2_n_list"))        return "e2_n_n";
    if (!strcmp(arr_name, "e2_tail_frac_list")) return "e2_tail_frac_n";
    if (!strcmp(arr_name, "e3_phases"))        return "e3_phase_n";
    return NULL;
}

static int set_u32_by_name(cfg_t *c, const char *name, uint32_t v) {
    const field_t *f = field_by_name(name);
    if (!f || f->type != T_U32) return -1;
    *(uint32_t *)((char *)c + f->off) = v;
    return 0;
}

/* ==================== 默认值 ==================== */

void cfg_default(cfg_t *c) {
    memset(c, 0, sizeof(*c));
    /* 显式清零三个对齐位：memset 已覆盖，此处冗余但自我文档化，确保
     * cfg_selftest 的 memcmp 往返在 padding 上恒等、而非"碰巧相等"。 */
    c->_pad0 = 0; c->_pad1 = 0; c->_pad2 = 0; c->_pad3 = 0;

    /* 机制 */
    c->ring_n = CFG_RING_N;
    { static const uint32_t v[] = CFG_SC_NEW;       memcpy(c->sc_new, v, sizeof(v)); }
    c->n_sc_new = CFG_SC_NEW_N;
    { static const uint32_t v[] = CFG_SC_OLD_TIERED; memcpy(c->sc_old_tiered, v, sizeof(v)); }
    c->n_sc_old = CFG_SC_OLD_TIERED_N;
    c->hdr_sz = CFG_HDR_SZ;
    c->ovf_cap = CFG_OVF_CAP;
    c->ovf_thresh = CFG_OVF_THRESH;
    c->fallback_mtu = CFG_FALLBACK_MTU;
    c->k_dwell = CFG_K_DWELL;
    c->j_quiet = CFG_J_QUIET;
    c->hist_decay = CFG_HIST_DECAY;
    c->shrink_gamma = CFG_SHRINK_GAMMA;
    c->adaptive_enable = CFG_ADAPTIVE_ENABLE;
    c->single_thread = CFG_SINGLE_THREAD;
    c->pool_use_mmap = CFG_POOL_USE_MMAP;
    c->order_guard = CFG_ORDER_GUARD;
    c->alloc_mode = CFG_ALLOC_MODE;
    c->sim_link_gbps = CFG_SIM_LINK_GBPS;
    c->sim_pend_cap = CFG_SIM_PEND_CAP;

    /* 计时/复现 */
    c->seed = CFG_SEED;
    c->reps = CFG_REPS;
    c->tsc_batch_ops = CFG_TSC_BATCH_OPS;
    c->warmup_ops = CFG_WARMUP_OPS;
    c->rdtsc_xval_ops = CFG_RDTSC_XVAL_OPS;

    /* 实验一 */
    c->e1a_n = CFG_E1A_N;
    { static const uint32_t v[] = CFG_E1A_PAYLOAD_LIST; memcpy(c->e1a_payload_list, v, sizeof(v)); }
    c->e1a_payload_n = CFG_E1A_PAYLOAD_N;
    c->e1a_timed_ops = CFG_E1A_TIMED_OPS;
    c->e1a_batch_ops = CFG_E1A_BATCH_OPS;
    c->e1a_batch_xval_ops = CFG_E1A_BATCH_XVAL_OPS;
    c->e1a_seq_mode = CFG_E1A_SEQ_MODE_SEQ;
    { static const uint32_t v[] = CFG_E1B_N_LIST;       memcpy(c->e1b_n_list, v, sizeof(v)); }
    c->e1b_n_n = CFG_E1B_N_N;
    c->e1b_gbn_long = CFG_E1B_GBN_LONG;
    c->e1b_gbn_short = CFG_E1B_GBN_SHORT;
    c->e1b_sr_k1 = CFG_E1B_SR_K1;
    c->e1b_sr_k2 = CFG_E1B_SR_K2;
    c->e1b_queries = CFG_E1B_QUERIES;
    { static const uint32_t v[] = CFG_E1B_PAYLOAD_LIST; memcpy(c->e1b_payload_list, v, sizeof(v)); }
    c->e1b_payload_n = CFG_E1B_PAYLOAD_N;
    c->e1b_batch_ops = CFG_E1B_BATCH_OPS;
    c->e1b_batch_xval_ops = CFG_E1B_BATCH_XVAL_OPS;
    c->e1b_sort_impl = CFG_E1B_SORT_IMPL;
    c->e1b_tree_inorder = CFG_E1B_TREE_INORDER;

    /* 实验二 */
    { static const uint32_t v[] = CFG_E2_PKTSIZE_LIST; memcpy(c->e2_pktsize_list, v, sizeof(v)); }
    c->e2_pktsize_n = CFG_E2_PKTSIZE_N;
    { static const uint32_t v[] = CFG_E2_N_LIST;        memcpy(c->e2_n_list, v, sizeof(v)); }
    c->e2_n_n = CFG_E2_N_N;
    c->e2_n = CFG_E2_N;
    c->e2_fixed_l = CFG_E2_FIXED_L;
    c->e2_tail_frac_n = CFG_E2_TAIL_FRAC_N;
    { static const double v[] = CFG_E2_TAIL_FRAC_LIST; memcpy(c->e2_tail_frac_list, v, sizeof(v)); }
    c->hash_nbuckets = CFG_HASH_NBUCKETS;
    c->fixed_block = CFG_FIXED_BLOCK;

    /* 实验三 */
    { static const cfg_phase_t v[] = CFG_E3_PHASES; memcpy(c->e3_phases, v, sizeof(v)); }
    c->e3_phase_n = CFG_E3_PHASE_N;
    c->e3_mixed_small = CFG_E3_MIXED_SMALL;
    c->e3_mixed_big = CFG_E3_MIXED_BIG;
    c->e3_mixed_big_p = CFG_E3_MIXED_BIG_P;
    c->e3_jitter = CFG_E3_JITTER;
    c->e3_nak_rate = CFG_E3_NAK_RATE;
    c->e3_nak_mode = CFG_E3_NAK_MODE;
    c->e3_latency_probe = CFG_E3_LATENCY_PROBE;

    /* 模拟器 */
    c->sim_loss_mode = CFG_SIM_LOSS_MODE;
    c->sim_loss_rate = CFG_SIM_LOSS_RATE;
    c->sim_burst_good_p = CFG_SIM_BURST_GOOD_P;
    c->sim_burst_bad_p = CFG_SIM_BURST_BAD_P;
    c->sim_nak_delay = CFG_SIM_NAK_DELAY;
    c->sim_seed = CFG_SIM_SEED;

    /* 运行期 */
    snprintf(c->out_dir, sizeof(c->out_dir), "out");
}

/* ==================== JSON 序列化 ==================== */

static void dump_field(FILE *f, const cfg_t *c, const field_t *fd) {
    const void *src = (const char *)c + fd->off;
    switch (fd->type) {
    case T_U32:
        fprintf(f, "%u", *(const uint32_t *)src);
        break;
    case T_U64:
        fprintf(f, "%llu", (unsigned long long)*(const uint64_t *)src);
        break;
    case T_DBL:
        fprintf(f, "%.17g", *(const double *)src);
        break;
    case T_STR: {
        const char *s = (const char *)src;
        fputc('"', f);
        for (; *s; s++) {
            if (*s == '"' || *s == '\\') fputc('\\', f);
            fputc(*s, f);
        }
        fputc('"', f);
        break;
    }
    case T_U32A: {
        const uint32_t *a = (const uint32_t *)src;
        fputc('[', f);
        for (uint32_t i = 0; i < fd->count; i++)
            fprintf(f, "%s%u", i ? "," : "", a[i]);
        fputc(']', f);
        break;
    }
    case T_DBLA: {
        const double *a = (const double *)src;
        fputc('[', f);
        for (uint32_t i = 0; i < fd->count; i++)
            fprintf(f, "%s%.17g", i ? "," : "", a[i]);
        fputc(']', f);
        break;
    }
    case T_PHASE: {
        const cfg_phase_t *a = (const cfg_phase_t *)src;
        fputc('[', f);
        for (uint32_t i = 0; i < fd->count; i++)
            fprintf(f, "%s[%u,%u]", i ? "," : "", a[i].epochs, a[i].pkt_len);
        fputc(']', f);
        break;
    }
    }
}

int cfg_dump_json(const cfg_t *c, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f, "{\n");
    for (size_t k = 0; k < N_FIELDS; k++) {
        fprintf(f, "  \"%s\": ", FIELDS[k].name);
        dump_field(f, c, &FIELDS[k]);
        fprintf(f, "%s\n", (k + 1 < N_FIELDS) ? "," : "");
    }
    fprintf(f, "}\n");
    fclose(f);
    return (int)N_FIELDS;
}

/* ==================== JSON 解析（最小递归下降） ==================== */

typedef struct { const char *s; size_t i, n; int err; } jp_t;

static void jp_skip_ws(jp_t *p) {
    while (p->i < p->n && (p->s[p->i] == ' ' || p->s[p->i] == '\t' ||
                           p->s[p->i] == '\n' || p->s[p->i] == '\r'))
        p->i++;
}

static int jp_expect(jp_t *p, char ch) {
    jp_skip_ws(p);
    if (p->i >= p->n || p->s[p->i] != ch) { p->err = 1; return -1; }
    p->i++;
    return 0;
}

static uint64_t jp_parse_u(jp_t *p) {
    jp_skip_ws(p);
    int neg = 0;
    if (p->i < p->n && p->s[p->i] == '-') { neg = 1; p->i++; }
    uint64_t v = 0;
    while (p->i < p->n && p->s[p->i] >= '0' && p->s[p->i] <= '9') {
        v = v * 10 + (uint64_t)(p->s[p->i] - '0');
        p->i++;
    }
    return neg ? (uint64_t)(-(int64_t)v) : v;
}

static double jp_parse_d(jp_t *p) {
    jp_skip_ws(p);
    char *end = NULL;
    double v = strtod(&p->s[p->i], &end);
    p->i = (size_t)(end - p->s);
    return v;
}

static int jp_parse_str(jp_t *p, char *out, size_t cap) {
    jp_skip_ws(p);
    if (p->i >= p->n || p->s[p->i] != '"') { p->err = 1; return -1; }
    p->i++;
    size_t o = 0;
    while (p->i < p->n && p->s[p->i] != '"') {
        char ch = p->s[p->i++];
        if (ch == '\\' && p->i < p->n) {
            char e = p->s[p->i++];
            ch = (e == 'n') ? '\n' : (e == 't') ? '\t' :
                 (e == '\\') ? '\\' : (e == '"') ? '"' : e;
        }
        if (o + 1 < cap) out[o++] = ch;
    }
    out[o] = 0;
    if (p->i < p->n) p->i++; /* 闭合引号 */
    return 0;
}

static int jp_parse_u32a(jp_t *p, uint32_t *dst, uint32_t cap, uint32_t *outn) {
    jp_skip_ws(p);
    if (p->i >= p->n || p->s[p->i] != '[') { p->err = 1; return -1; }
    p->i++;
    uint32_t n = 0;
    jp_skip_ws(p);
    if (p->i < p->n && p->s[p->i] == ']') { p->i++; *outn = n; return 0; }
    for (;;) {
        uint32_t v = (uint32_t)jp_parse_u(p);
        if (n < cap) dst[n] = v;
        n++;
        jp_skip_ws(p);
        if (p->i < p->n && p->s[p->i] == ',') { p->i++; continue; }
        break;
    }
    if (jp_expect(p, ']')) return -1;
    *outn = n;
    return 0;
}

static int jp_parse_dbla(jp_t *p, double *dst, uint32_t cap, uint32_t *outn) {
    jp_skip_ws(p);
    if (p->i >= p->n || p->s[p->i] != '[') { p->err = 1; return -1; }
    p->i++;
    uint32_t n = 0;
    jp_skip_ws(p);
    if (p->i < p->n && p->s[p->i] == ']') { p->i++; *outn = n; return 0; }
    for (;;) {
        double v = jp_parse_d(p);
        if (n < cap) dst[n] = v;
        n++;
        jp_skip_ws(p);
        if (p->i < p->n && p->s[p->i] == ',') { p->i++; continue; }
        break;
    }
    if (jp_expect(p, ']')) return -1;
    *outn = n;
    return 0;
}

static int jp_parse_phasea(jp_t *p, cfg_phase_t *dst, uint32_t cap, uint32_t *outn) {
    jp_skip_ws(p);
    if (p->i >= p->n || p->s[p->i] != '[') { p->err = 1; return -1; }
    p->i++;
    uint32_t n = 0;
    jp_skip_ws(p);
    if (p->i < p->n && p->s[p->i] == ']') { p->i++; *outn = n; return 0; }
    for (;;) {
        if (jp_expect(p, '[')) return -1;
        uint32_t e = (uint32_t)jp_parse_u(p);
        if (jp_expect(p, ',')) return -1;
        uint32_t l = (uint32_t)jp_parse_u(p);
        if (jp_expect(p, ']')) return -1;
        if (n < cap) { dst[n].epochs = e; dst[n].pkt_len = l; }
        n++;
        jp_skip_ws(p);
        if (p->i < p->n && p->s[p->i] == ',') { p->i++; continue; }
        break;
    }
    if (jp_expect(p, ']')) return -1;
    *outn = n;
    return 0;
}

static int jp_skip_value(jp_t *p) {
    jp_skip_ws(p);
    if (p->i >= p->n) { p->err = 1; return -1; }
    char ch = p->s[p->i];
    if (ch == '"') { char t[2]; return jp_parse_str(p, t, sizeof(t)); }
    if (ch == '[' || ch == '{') {
        char open = ch, close = (ch == '[') ? ']' : '}';
        int depth = 0;
        while (p->i < p->n) {
            char c = p->s[p->i];
            if (c == '"') { char t[2]; if (jp_parse_str(p, t, sizeof(t))) return -1; continue; }
            if (c == open) depth++;
            else if (c == close) { depth--; p->i++; if (depth == 0) return 0; continue; }
            p->i++;
        }
        p->err = 1; return -1;
    }
    while (p->i < p->n && !strchr(",]} \t\n\r", p->s[p->i])) p->i++;
    return 0;
}

static int jp_parse_field(jp_t *p, cfg_t *c, const field_t *f) {
    void *dst = (char *)c + f->off;
    switch (f->type) {
    case T_U32:  *(uint32_t *)dst = (uint32_t)jp_parse_u(p); break;
    case T_U64:  *(uint64_t *)dst = jp_parse_u(p); break;
    case T_DBL:  *(double *)dst = jp_parse_d(p); break;
    case T_STR: { char t[256]; if (jp_parse_str(p, t, sizeof(t))) return -1;
                  snprintf((char *)dst, 256, "%s", t); break; }
    case T_U32A: {
        uint32_t n = 0;
        if (jp_parse_u32a(p, (uint32_t *)dst, f->count, &n)) return -1;
        break;
    }
    case T_DBLA: {
        uint32_t n = 0;
        if (jp_parse_dbla(p, (double *)dst, f->count, &n)) return -1;
        break;
    }
    case T_PHASE: {
        uint32_t n = 0;
        if (jp_parse_phasea(p, (cfg_phase_t *)dst, f->count, &n)) return -1;
        break;
    }
    default: p->err = 1; return -1;
    }
    return 0;
}

static int jp_parse_root(jp_t *p, cfg_t *c) {
    if (jp_expect(p, '{')) return -1;
    int n = 0;
    jp_skip_ws(p);
    if (p->i < p->n && p->s[p->i] == '}') { p->i++; return 0; }
    for (;;) {
        char key[128];
        if (jp_parse_str(p, key, sizeof(key))) return -1;
        if (jp_expect(p, ':')) return -1;
        const field_t *f = field_by_name(key);
        if (!f) { if (jp_skip_value(p)) return -1; }
        else { if (jp_parse_field(p, c, f)) return -1; n++; }
        jp_skip_ws(p);
        if (p->i < p->n && p->s[p->i] == ',') { p->i++; continue; }
        break;
    }
    if (jp_expect(p, '}')) return -1;
    return n;
}

/* ==================== JSON 反序列化 ==================== */

int cfg_load_json(cfg_t *c, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return -1; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return -1; }
    buf[sz] = 0;
    fclose(f);

    cfg_default(c); /* 缺失字段回落到默认值（并保证 padding/pad 归零） */
    jp_t p = { buf, 0, (size_t)sz, 0 };
    int n = jp_parse_root(&p, c);
    free(buf);
    if (p.err) return -1;
    return n;
}

/* ==================== 自检 ==================== */

int cfg_selftest(const cfg_t *c, const char *path) {
    cfg_t a, b;
    memcpy(&a, c, sizeof(a));
    if (a._pad0 || a._pad1 || a._pad2) return -4; /* 对齐位必须已清零 */
    int nd = cfg_dump_json(&a, path);
    if (nd != (int)N_FIELDS) return -1;
    int nl = cfg_load_json(&b, path);
    if (nl != (int)N_FIELDS) return -2;
    if (memcmp(&a, &b, sizeof(a)) != 0) return -3;
    return 0;
}

/* ==================== CLI 覆盖 ==================== */

static int parse_u32_list(const char *s, uint32_t *out, uint32_t cap, uint32_t *n) {
    *n = 0;
    const char *p = s;
    while (*p) {
        while (*p == ',' || *p == ' ') p++;
        if (!*p) break;
        char *end = NULL;
        unsigned long v = strtoul(p, &end, 0);
        if (end == p) return -1;
        if (*n < cap) out[*n] = (uint32_t)v;
        (*n)++;
        p = end;
        if (*p == ',') p++;
    }
    return 0;
}

static int parse_dbl_list(const char *s, double *out, uint32_t cap, uint32_t *n) {
    *n = 0;
    const char *p = s;
    while (*p) {
        while (*p == ',' || *p == ' ') p++;
        if (!*p) break;
        char *end = NULL;
        double v = strtod(p, &end);
        if (end == p) return -1;
        if (*n < cap) out[*n] = v;
        (*n)++;
        p = end;
        if (*p == ',') p++;
    }
    return 0;
}

static int parse_phase_list(const char *s, cfg_phase_t *out, uint32_t cap, uint32_t *n) {
    *n = 0;
    const char *p = s;
    while (*p) {
        while (*p == ',' || *p == ' ' || *p == ';') p++;
        if (!*p) break;
        char *end = NULL;
        unsigned long e = strtoul(p, &end, 0);
        if (end == p) return -1;
        p = end;
        while (*p == ',' || *p == ' ') p++;
        unsigned long l = strtoul(p, &end, 0);
        if (end == p) return -1;
        p = end;
        if (*n < cap) { out[*n].epochs = (uint32_t)e; out[*n].pkt_len = (uint32_t)l; }
        (*n)++;
        if (*p == ';') p++;
    }
    return 0;
}

static uint32_t nak_mode_val(const char *v) {
    if (!strcmp(v, "sr")) return 0;
    if (!strcmp(v, "gbn")) return 1;
    if (!strcmp(v, "mixed")) return 2;
    return (uint32_t)strtoul(v, NULL, 0);
}

static uint32_t loss_mode_val(const char *v) {
    if (!strcmp(v, "uniform")) return 0;
    if (!strcmp(v, "ge") || !strcmp(v, "gilbert")) return 1;
    return (uint32_t)strtoul(v, NULL, 0);
}

static uint32_t alloc_mode_val(const char *v) {
    if (!strcmp(v, "pooled")) return 0;
    if (!strcmp(v, "perstore")) return 1;
    return (uint32_t)strtoul(v, NULL, 0);
}

static void e3_preset(cfg_t *c, const char *v) {
    static const cfg_phase_t phase_a[] = CFG_E3_PHASES;
    static const cfg_phase_t phase_mixed[] = CFG_E3_MIXED_PHASES;
    memset(c->e3_phases, 0, sizeof(c->e3_phases));
    if (!strcmp(v, "mixed")) {
        c->e3_phase_n = CFG_E3_MIXED_PHASE_N;
        memcpy(c->e3_phases, phase_mixed, sizeof(phase_mixed));
    } else {
        c->e3_phase_n = CFG_E3_PHASE_N;
        memcpy(c->e3_phases, phase_a, sizeof(phase_a));
    }
    /* 消融（ablate）：机制滞后旋钮调到最灵敏档（默认相位序列不变）。
     *   慢响应对照（slow）即默认值，无需覆盖。 */
    if (!strcmp(v, "ablate")) {
        c->k_dwell    = CFG_E3_ABLATE_K_DWELL;
        c->ovf_thresh = CFG_E3_ABLATE_OVF_THRESH;
        c->j_quiet    = CFG_E3_ABLATE_J_QUIET;
        c->hist_decay = CFG_E3_ABLATE_HIST_DECAY;
    }
}

static int apply_override(cfg_t *c, const field_t *f, const char *val) {
    void *dst = (char *)c + f->off;
    char *end = NULL;
    switch (f->type) {
    case T_U32: { unsigned long v = strtoul(val, &end, 0); if (*end) return -1;
                  *(uint32_t *)dst = (uint32_t)v; return 0; }
    case T_U64: { unsigned long long v = strtoull(val, &end, 0); if (*end) return -1;
                  *(uint64_t *)dst = (uint64_t)v; return 0; }
    case T_DBL: { double v = strtod(val, &end); if (*end) return -1;
                  *(double *)dst = v; return 0; }
    case T_STR: { snprintf((char *)dst, 256, "%s", val); return 0; }
    case T_U32A: {
        uint32_t arr[64], n = 0;
        if (parse_u32_list(val, arr, 64, &n)) return -1;
        if (n > f->count) return -1;
        uint32_t *d = (uint32_t *)dst;
        for (uint32_t k = 0; k < f->count; k++) d[k] = 0;
        for (uint32_t k = 0; k < n; k++) d[k] = arr[k];
        const char *cf = count_field_for(f->name);
        if (cf) set_u32_by_name(c, cf, n);
        return 0;
    }
    case T_DBLA: {
        double arr[64]; uint32_t n = 0;
        if (parse_dbl_list(val, arr, 64, &n)) return -1;
        if (n > f->count) return -1;
        double *d = (double *)dst;
        for (uint32_t k = 0; k < f->count; k++) d[k] = 0.0;
        for (uint32_t k = 0; k < n; k++) d[k] = arr[k];
        const char *cf = count_field_for(f->name);
        if (cf) set_u32_by_name(c, cf, n);
        return 0;
    }
    case T_PHASE: {
        cfg_phase_t ph[64]; uint32_t n = 0;
        if (parse_phase_list(val, ph, 64, &n)) return -1;
        cfg_phase_t *d = (cfg_phase_t *)dst;
        for (uint32_t k = 0; k < f->count; k++) { d[k].epochs = 0; d[k].pkt_len = 0; }
        for (uint32_t k = 0; k < n && k < f->count; k++) d[k] = ph[k];
        const char *cf = count_field_for(f->name);
        if (cf) set_u32_by_name(c, cf, n);
        return 0;
    }
    default: return -1;
    }
}

static void cfg_print_usage(const char *prog) {
    printf("用法: %s [--key=value ...]\n", prog);
    printf("  通用字段（--snake-case=值，或 --snake-case 值）：覆盖 cfg_t 同名字段\n");
    printf("  数组字段用逗号分隔，如 --sc-new=128,256,1024\n");
    printf("  枚举/预设:\n");
    printf("    --e3-nak-mode=sr|gbn|mixed    --sim-loss-mode=uniform|ge\n");
    printf("    --alloc-mode=pooled|perstore  分配策略（第 5 条正交矩阵）\n");
    printf("    --e3-preset=default|mixed|ablate|slow   --e3-phases=a,b;c,d;...\n");
    printf("    --out=<dir>                   输出目录\n");
}

int cfg_override_cli(cfg_t *c, int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strncmp(a, "--", 2) != 0) continue;
        const char *key = a + 2;
        const char *val = strchr(key, '=');
        char keybuf[128];
        if (val) {
            size_t kl = (size_t)(val - key);
            if (kl >= sizeof(keybuf)) kl = sizeof(keybuf) - 1;
            memcpy(keybuf, key, kl); keybuf[kl] = 0;
            val++;
        } else {
            snprintf(keybuf, sizeof(keybuf), "%s", key);
            val = (i + 1 < argc) ? argv[++i] : "1";
        }
        if (!strcmp(keybuf, "help") || !strcmp(keybuf, "h")) { cfg_print_usage(argv[0]); return 0; }
        if (!strcmp(keybuf, "e3-nak-mode"))  { c->e3_nak_mode = nak_mode_val(val); continue; }
        if (!strcmp(keybuf, "sim-loss-mode")){ c->sim_loss_mode = loss_mode_val(val); continue; }
        if (!strcmp(keybuf, "alloc-mode"))   { c->alloc_mode = alloc_mode_val(val); continue; }
        if (!strcmp(keybuf, "e3-preset"))    { e3_preset(c, val); continue; }
        if (!strcmp(keybuf, "out"))          { snprintf(keybuf, sizeof(keybuf), "out_dir"); }
        const field_t *f = field_by_cli(keybuf);
        if (!f) { fprintf(stderr, "[config] 未知参数 --%s\n", keybuf); return -1; }
        if (apply_override(c, f, val)) {
            fprintf(stderr, "[config] 解析失败 --%s=%s\n", keybuf, val);
            return -1;
        }
    }
    return 0;
}

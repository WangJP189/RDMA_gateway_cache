# R1 —— 基准预热与语义审计（Step 6，只读）

本文件只读 260912 旧项目基准源码，不改动任何文件。每条结论给出【文件 + 行号 + 原代码片段】。

四个审计点：① 预热对称性　② 基线 live-set 语义（D2）　③ 重复键语义（D3）　④ E 可行性（index_only 抽取）。
附：psn_dynblock 四个关键函数完整代码（当前 260916 版本）。

---

## 1. 预热对称性（warm_allocator 调用点）

全项目 `grep -rn "warm_allocator"`：**唯一命中是 `psn_behavior_bench/behavior_bench.c`**。其余三个基准（lookup / space / zeroreorder）都没有 warm_allocator。

### 1.1 behavior_bench.c（store 延迟基准，6 方法）—— 有预热，且方法间对称

定义：
```c
// psn_behavior_bench/behavior_bench.c:117-128
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
```

调用点（`main` 内、跑所有方法**之前**一次性预热）：
```c
// psn_behavior_bench/behavior_bench.c:978-980
    warm_allocator(MEM_BLOCK_SIZE, 2 * N);
    warm_allocator(1056, 2 * N);
    warm_allocator(512, 2 * N);
```

`MEM_BLOCK_SIZE` 定义：
```c
// psn_behavior_bench/behavior_bench.c:46
#define MEM_BLOCK_SIZE 5120 /* psn_fixed fixed block (status quo) */
```

方法集合：
```c
// psn_behavior_bench/behavior_bench.c:984-985
    printf("methods: fifo / chained_hash / balanced_tree / psn_fixed / "
           "psn_dynamic / psn_tiered\n");
```

**判定**：预热 {5120, 1056, 512} 三个 size class，各 `2*N` 次，是**一次全局预热 → 跑全部 6 方法**，所以这 6 个方法之间是**对称**的。5120 对应 psn_fixed 的固定块；1056/512 对应小 payload 的节点块。这是 store 延迟对比时应当照抄的口径。

### 1.2 psn_bench.c（lookup 延迟基准，4 方法）—— 无预热，但 store 不在计时区

4 方法 = fifo / chained_hash / balanced_tree / psn_mapping（`psn_bench.c:103-104`）。无 warm_allocator。

关键：这个 benchmark **只对 lookup 计时，insert 是 setup**：
```c
// psn_lookup_benchmark/psn_bench.c:734-741
                struct cache c;
                cache_init(&c, m, N);
                for (uint32_t i = 0; i < N; i++)
                    cache_insert(&c, pkts[i].psn, &pkts[i]);   /* setup，不计时 */

                for (uint32_t r = 0; r < reps; r++)
                    bench_lookup(&c, psns + (size_t)r * total, R, B,
                                 cycles + (size_t)r * R);      /* 只计时这个 */
```

**判定**：hash / tree 的 `malloc(sizeof(node))` 都发生在 setup 的 `cache_insert` 里，不进计时区；所以「无预热」对 lookup 数字本身无害。**但**一旦我们把 psn_bench 的 4 方法复用到 exp1 的 **store 延迟**对比，就必须补 warm_allocator（照 1.1 的口径），否则 hash/tree 的每 store malloc 会命中冷页、被系统性高估。

### 1.3 space_bench.c / zeroreorder_bench.c —— 无预热，但指标与 malloc 冷页无关

- `space_bench.c`：测稳态**内存占用**（byte 数），不测延迟，无需预热。
- `zeroreorder_bench.c`：测 retrieve 的**平均比较次数**（`avg_cmp`，是一个确定性计数，不是墙钟），与 malloc 冷页无关。其 store 路径确有每项 malloc，但不进入被计时/被计数的路径：
```c
// psn_zeroreorder/zeroreorder_bench.c:143  (fifo_store)
    struct fifo_node *n = malloc(sizeof(*n) + (size_t)len);
// psn_zeroreorder/zeroreorder_bench.c:212  (hash_store)
    struct hash_node *n = malloc(sizeof(*n) + (size_t)len);
// psn_zeroreorder/zeroreorder_bench.c:331  (tree_store)
        struct avl_node *n = malloc(sizeof(*n) + (size_t)len);
// psn_zeroreorder/zeroreorder_bench.c:441  (tiered_store)
        blk = malloc(tier_block_size(tier));
```

### 1.4 逐方法预热 size class 汇总

| 基准 | 方法 | 预热的 size class | 计时/计数对象 | 对称性 |
|---|---|---|---|---|
| behavior_bench | fifo/chained_hash/balanced_tree/psn_fixed/psn_dynamic/psn_tiered | {5120, 1056, 512}×2N | **store 延迟**（ns） | ✅ 对称（一次全局预热） |
| psn_bench | fifo/chained_hash/balanced_tree/psn_mapping | 无 | **lookup 延迟**（ns），insert=setup | ⚠️ 自身无影响；复用到 store 对比须补预热 |
| space_bench | （多流空间） | 无 | 稳态内存 byte | ✅ 无关 |
| zeroreorder_bench | fifo/chained_hash/balanced_tree/psn_tiered | 无 | retrieve 平均比较次数 | ✅ 无关（计数非墙钟） |

**对 psn_dynblock 的启示**：环路径稳态 `malloc==0`（不变量 I3），天然免疫；溢出路径按项 malloc。exp1a（store 延迟对比）必须对**所有方法统一预热**，口径照 `behavior_bench.c:978-980`，否则 hash/tree 吃亏。

---

## 2. 基线 live-set 语义（D2）

每种方法的 insert 路径（`psn_bench.c:276-303` 的 `cache_insert`）：

```c
// psn_lookup_benchmark/psn_bench.c:276-303
static void cache_insert(struct cache *c, uint32_t psn, struct packet *p) {
    switch (c->m) {
    case M_FIFO: {
        if (c->u.fifo.count < c->u.fifo.cap)
            c->u.fifo.arr[c->u.fifo.count++] = p;
        break;
    }
    case M_HASH: {
        uint32_t h = psn % c->u.hash.nbuckets;
        struct hash_node *n = malloc(sizeof(*n));
        n->psn = psn;
        n->pkt = p;
        n->next = c->u.hash.buckets[h];
        c->u.hash.buckets[h] = n; /* 头插 */
        break;
    }
    case M_TREE:
        c->u.tree = avl_insert(c->u.tree, psn, p);
        break;
    case M_PSNMAP: {
        uint32_t idx = psn % c->u.map.size;
        c->u.map.arr[idx] = p;
        break;
    }
    ...
```

容量常量：
```c
// psn_bench.c:46
#define RING_SIZE 10240
// psn_bench.c:48
#define HASH_NBUCKETS 4096
```

| 方法 | 插入行为 | 有界/无界 | 淘汰？ | 200k store 后的 live set 估算（N=10240） |
|---|---|---|---|---|
| fifo | `count < cap` 才追加，满则**静默丢弃** | 有界 cap=N | ❌ 无淘汰（满了就丢新的） | **10240**（冻结；第 10240 个之后的 store 全部丢弃，缓存内容不再变化） |
| chained_hash | 每 store **malloc 头插**，无去重无淘汰 | **无界** | ❌ 无 | **200000 节点**（每 store 一个新节点，负载因子 200000/4096 ≈ 48.8，桶链极长 → lookup 退化 O(负载)） |
| balanced_tree | AVL 插入，重复 psn 覆盖 | 取决于 psn 分布 | ❌ 无淘汰（只覆盖同 psn） | psn 单调 0..199999 → **200000 节点**；psn 循环于小窗口 → **~唯一 psn 数** |
| psn_mapping | `arr[psn % size] = p` 位置覆盖 | 有界 RING_SIZE=10240 | ✅ 位置淘汰（覆盖） | **10240**（始终只保留最近映射到的槽） |

**结论（D2）**：只有 psn_mapping 是真正的「有界滑动窗口 + 淘汰」语义；fifo 是「满即冻结」；chained_hash 与 balanced_tree 都是**无界增长**——它们不是同一类缓存，lookup 成本随 live set 增长（hash 链长 / 树深），而 psn_mapping 与 psn_dynblock 是 O(1)。exp2（空间/缓存命中对比）必须显式说明这一语义差，不能把「无界 hash/tree」与「有界环」放在同一张图里假装同语义。

---

## 3. 重复键语义（D3）—— 同 psn 重复写

| 方法 | 重复写行为 | 源码依据 |
|---|---|---|
| fifo | 无去重，重复 append 成多条（受 cap 限） | `psn_bench.c:278-281` |
| chained_hash | **头插保留所有重复节点**（同 psn 可多条）；lookup 返回**头插后最新**的那个（首个匹配） | `psn_bench.c:283-290` + lookup `319-324` |
| balanced_tree | **覆盖**：同 psn 命中 `else` 分支替换 `pkt`，不新增节点 | `psn_bench.c:209-211` |
| psn_mapping | **位置覆盖**：同 idx 直接覆盖；不同 psn 但 `psn%RING_SIZE` 相同也算碰撞覆盖（lookup 用 `p->psn==psn` 兜底校验） | `psn_bench.c:295-299` + lookup `328-334` |

AVL 覆盖分支原文：
```c
// psn_lookup_benchmark/psn_bench.c:209-211
    else {
        n->pkt = p;
        return n;
    }
```

**结论（D3）**：chained_hash 是唯一「重复写会导致内存无界膨胀」的方法（无去重），其余三者重复写不增内存（fifo 受 cap、tree 覆盖、map 位置覆盖）。psn_dynblock 的语义 = psn_mapping（位置覆盖，O(1)），属「覆盖」一族，与 chained_hash 有本质差异。

---

## 4. E 可行性 —— psn_mapping 的 index 能否抽成 index_only 微基准

psn_mapping 的映射/索引代码只有两处、各 1 行：

```c
// 插入索引：psn_lookup_benchmark/psn_bench.c:296-297
        uint32_t idx = psn % c->u.map.size;
        c->u.map.arr[idx] = p;

// 查找索引：psn_lookup_benchmark/psn_bench.c:329-332
        uint32_t idx = psn % c->u.map.size;
        struct packet *p = c->u.map.arr[idx];
        if (p && p->psn == psn)
            return p;
```

**判定：可抽，且应抽。** index_only 微基准 = 只跑 `idx = psn % N` + 一次槽指针追逐（不含 payload memcpy），给出 store 延迟的**算法下界**（映射 + 槽命中的不可约成本）。

**索引成本论证（不依赖具体 N）**：对同一个 N，psn_dynblock 的 `phi(psn) = (psn & 0xFFFFFFu) % N` 与基线的 `psn % N` 是**同一种运算**——一次整数取模；唯一差别是我们多做一次 24-bit 掩码。那次掩码是 **24-bit 回绕安全防御**（RDMA 的 PSN 本身就是 24-bit 字段）；若上层已保证 `psn < 2^24`，它是冗余的，可改成 debug-only 的 assert，那时**索引成本与基线完全一致**。

**诚实边界**：若某个配置的 N 恰为 2 的幂（基线的 `% N` 退化为位与），要**如实说明「该配置下基线的取模比我们少一步」**，不回避。exp1a 同时报「N 为 2 的幂」与「非 2 的幂」两种配置（或 exp1b 扫 N 时自然覆盖），证明索引成本差 ≤1 cycle、可忽略。

抽取工作量：把 `cache_insert`/`cache_lookup` 的 `M_PSNMAP` 分支单独拎成一个「只索引、不拷贝」的循环即可，10 行内完成。

---

## 5. 四个关键函数完整代码（当前 260916 版本，行号以本次为准）

### 5.1 conn_store（`src/dynblock.c:79-129`）

```c
int conn_store(conn_t *c, uint32_t psn, const uint8_t *payload, uint16_t len) {
    spin_lock(&c->lk);
    uint32_t i = phi(c, psn);
    slot_meta_t *m = &c->meta[i];
    int ret = R_OK;

    /* 1A order_guard：旧包晚到 → 丢弃 incoming，不回收槽内 occupant */
    if (c->cfg->order_guard && m->flag != SLOT_FREE && psn_newer(m->psn, psn)) {
        c->n_ooo_drop++;
        ret = R_DROPPED;
        goto done;
    }

    evict_occupant(c, i);

    if (len <= c->S) {                          /* 正常路径：进环 */
        uint8_t *dst = payload_of(&c->cur, i);
        memcpy(dst, payload, len);
        hdr_set(slot(&c->cur, i), len, psn, (uint64_t)psn);   /* stamp = psn */
        *m = (slot_meta_t){ .psn = psn, .len = len, .flag = SLOT_IN_RING,
                            .gen = c->cur.gen, .ovf_idx = 0 };
        c->cur_live++;
        if (c->cfg->adaptive_enable && len > c->epoch_ring_max) c->epoch_ring_max = len;
    } else {                                     /* 溢出路径：每项 malloc 变长块 */
        int oi = ovf_alloc(c, psn, payload, len);
        if (oi < 0 && c->cfg->adaptive_enable) {
            grow_locked(c, len);                 /* 调用方已持锁，不 relock */
            oi = ovf_alloc(c, psn, payload, len);
        }
        if (oi < 0) {                            /* 溢出满且无法扩 ⇒ 丢弃（不破坏正确性） */
            c->n_drop++;
            ret = R_DROPPED;
            goto done;
        }
        *m = (slot_meta_t){ .psn = psn, .len = len, .flag = SLOT_IN_OVF,
                            .gen = c->cur.gen, .ovf_idx = (uint32_t)oi };
        c->n_ovf_ins++;
        if (c->cfg->adaptive_enable && len > c->ovf_max_hist) c->ovf_max_hist = len;
    }

done:
    c->n_store++;
    if (c->cfg->adaptive_enable) {
        c->epoch_stores++;                        /* 丢弃路径也推进（B-4：纪元只认此计数） */
        release_old_if_drained(c);
        if (c->epoch_stores >= c->N)
            check_and_maybe_resize(c);            /* 唯一触发（无时间兜底） */
    }
    spin_unlock(&c->lk);
    return ret;
}
```

### 5.2 check_and_maybe_resize（`src/dynblock.c:173-213`）

```c
static void check_and_maybe_resize(conn_t *c) {
    const cfg_t *cf = c->cfg;
    uint32_t L_ring  = c->epoch_ring_max;
    uint32_t ovf_cnt = c->ovf_count;
    uint32_t L_ovf   = ovf_scan_max(c);          /* O(OVF_CAP)，~1 μs */
    uint32_t S_old   = c->S;

    c->epoch_stores = 0;
    c->epoch_ring_max = 0;
    if (ovf_cnt == 0) c->quiet_epochs++; else c->quiet_epochs = 0;
    if (c->quiet_epochs >= cf->hist_decay) c->ovf_max_hist = 0;

    /* ===== 规则 A：溢出过多 ⇒ 块太小 ⇒ 扩 ===== */
    if (ovf_cnt >= cf->ovf_thresh) {
        c->grow_streak++; c->shrink_streak = 0;
        if (c->grow_streak >= cf->k_dwell) {
            uint32_t S_new = ceil_class(cf, L_ovf ? L_ovf : c->S + 1);
            if (S_new > c->S && gen_switch(c, S_new)) {
                drain_overflow(c);
                c->n_resize++;                   /* B-6：真正切换才计数 */
            }
            c->grow_streak = 0;
        }
        return;
    }
    c->grow_streak = 0;

    /* ===== 规则 B：环内最大包远小于 S ⇒ 块太大 ⇒ 缩 ===== */
    if (ovf_cnt == 0 && c->quiet_epochs >= cf->j_quiet && L_ring > 0) {
        uint32_t target = ceil_class(cf, L_ring);
        if (target < ceil_class(cf, c->ovf_max_hist))
            target = ceil_class(cf, c->ovf_max_hist);   /* 绝不缩到最近溢出包以下 */
        if (target < S_old && L_ring <= (uint32_t)((double)S_old * cf->shrink_gamma)) {
            c->shrink_streak++;
            if (c->shrink_streak >= cf->k_dwell) {
                if (gen_switch(c, target)) c->n_resize++;
                c->shrink_streak = 0;
            }
        } else c->shrink_streak = 0;
    } else c->shrink_streak = 0;
}
```

### 5.3 gen_switch（`src/dynblock.c:218-230`）

```c
/* 双世代切换（零迁移）。返回 1=真的切换了；0=延后（旧池未排空）或分配失败。 */
static int gen_switch(conn_t *c, uint32_t S_new) {
    if (c->has_old && c->old_live > 0) return 0; /* §2.11-3：旧池未排空 ⇒ 延后，下纪元重试 */
    if (c->has_old) { pool_free(&c->old); c->has_old = 0; }

    pool_t np;
    if (pool_alloc(&np, c->N, S_new, c->cfg->pool_use_mmap) < 0) return 0;
    np.gen = (uint8_t)(c->cur.gen + 1);

    c->old = c->cur; c->has_old = 1; c->old_live = c->cur_live;   /* B-8：O(1) 计数，不扫数组 */
    c->cur = np;      c->cur_live = 0;
    c->S = S_new;
    return 1;
}
```

### 5.4 drain_overflow（`src/dynblock.c:241-268`，本轮已加 ② 断言兜底 + ③ 残留语义）

```c
static void drain_overflow(conn_t *c) {
    uint32_t moved = 0, residual = 0, skipped = 0;
    for (uint32_t oi = 0; oi < c->cfg->ovf_cap; oi++) {
        ovf_entry_t *e = &c->ovf[oi];
        if (!e->used) continue;
        uint32_t i = phi(c, e->psn);
        slot_meta_t *m = &c->meta[i];
        /* 目标槽校验（正确性级，严格于 C-10）：只可能是本项自己的 IN_OVF 槽。
         * 断言 + 安全退化双保险：-DNDEBUG 下 assert 被删，仍不覆盖原槽、不计为搬回。 */
        if (!(m->flag == SLOT_IN_OVF && m->psn == e->psn)) {
            assert(0 && "drain: target slot not owned by this overflow entry");
            skipped++;                 /* 不覆盖，保留原槽内容 */
            continue;
        }
        if (e->len > c->S) { residual++; continue; }   /* ③ 超上限项，留在溢出区 */
        uint8_t *dst = payload_of(&c->cur, i);
        memcpy(dst, ovf_payload(e), e->len);
        hdr_set(slot(&c->cur, i), e->len, e->psn, (uint64_t)e->psn);
        m->flag = SLOT_IN_RING; m->gen = c->cur.gen; m->len = e->len;
        c->cur_live++;
        ovf_release(c, oi);
        moved++;
    }
    c->ovf_count = residual + skipped;   /* 只剩超上限项 + 跳过项（通常为 0） */
    c->n_drain += moved;
    c->n_drain_residual = residual;      /* 语义：最近一次 drain 的残留数（每次重置） */
    c->n_drain_skip += skipped;          /* 累计 */
}
```

---

## 附：审计要点速览（供 Step 9/10 设计直接引用）

1. **预热**：store 延迟对比统一照 `behavior_bench.c:978-980`（{5120,1056,512}×2N）；psn_dynblock 环路径零 malloc，溢出路径按项 malloc，对比时必须给 hash/tree 同等的热 bin。
2. **live-set**：hash/tree 无界，不是滑动窗口缓存；只有 psn_mapping / psn_dynblock 是有界淘汰。exp2 的空间对比要显式标注语义差。
3. **重复键**：chained_hash 是唯一「重复写内存无界膨胀」的方法。
4. **index_only**：可抽；同一 N 下 `(psn&0xFFFFFF)%N` 与 `psn%N` 是同一次取模，24-bit 掩码是防御（详见 §4 改写）。

---

## 6. 预热覆盖验证（R1 §1 的细化：从「结构性对称」到「覆盖对称」）

从源码算出每个方法 store 路径的实际 malloc 尺寸，映射到 glibc size class，检查 {5120,1056,512} 是否覆盖。

### 6.1 各方法 store 的 malloc 尺寸（behavior_bench.c）

| 方法 | 节点结构 | sizeof(节点) | store 的 malloc 尺寸 | @SL=64 | @L=1024 |
|---|---|---|---|---|---|
| fifo | `{next*, hdr(24), data[]}` | 32 | `32+len` | **96** | 1056 |
| chained_hash | 同 fifo | 32 | `32+len` | **96** | 1056 |
| balanced_tree | `{l*,r*,h(4),hdr(24),data[]}` | 48 | `48+len` | **112** | 1072 |
| psn_fixed | — | — | `MEM_BLOCK_SIZE=5120` | 5120 | 5120 |
| psn_dynamic | — | — | `align16(24+len)` | **96** | 1056 |
| psn_tiered | — | — | `align16(24+tier_boundary[t])` | **288** | 1056 |

（`mem_block_header`=24B、8 对齐，与 260916 一致；fifo/hash 节点 = 8+24 = 32；tree = 8+8+4 → pad 至 24 → +24 = 48。）

### 6.2 覆盖检查：预热集 {5120, 1056, 512}

- **5120** → 只暖到 psn_fixed。✅
- **1056** → 暖到 fifo/hash/psn_dynamic @L=1024 与 psn_tiered @1024。✅（但不暖 @64）
- **512** → 与任何方法的真实尺寸都无关（最小节点 96）。❌ 无效预热。

**未被覆盖**：96（fifo/hash/psn_dynamic@64）、112（tree@64）、288（tiered@64）、1072（tree@1024）、4128~4144（所有方法@4096）。

### 6.3 结论

store 采样器（`run_store_samples`，SL=64）里，**只有 psn_fixed 的 5120 malloc 被预热**；fifo/hash/tree/psn_dynamic/psn_tiered 的 96/112/288 全是冷 bin。

叠加「自我预热」差异：
- **fifo/hash/tree（无界）**：每 store malloc 且**从不 free**（结构只增不减）→ 完全无自我预热；
- **psn_fixed/dynamic**：每 store `malloc` 后 `free(旧块)`（behavior_bench.c:516-520）→ 计时内首轮覆盖后 tcache 被 self-prime；
- **psn_tiered**：走 free-list 复用（behavior_bench.c:493-507）→ 同样自我预热。

⇒ 旧 `store_samples.csv` **系统性高估** fifo/hash/tree 的 store 延迟（相对 psn_fixed）。**exp1a 必须统一预热**：照 `behavior_bench.c:978-980` 口径 + **补齐每个方法的实际 malloc 尺寸类**（96/112/288/1056/1072/4128/5120）。

---

## 7. evict_occupant 确认（函数审查第①条）

```c
// src/dynblock.c:65-75（当前 260916 版本）
static void evict_occupant(conn_t *c, uint32_t i) {
    slot_meta_t *m = &c->meta[i];
    if (m->flag == SLOT_FREE) return;
    if (m->flag == SLOT_IN_RING) {
        if (m->gen == c->cur.gen) c->cur_live--; else c->old_live--;
    } else {                                   /* 溢出项需显式回收（下标不被索引覆盖） */
        ovf_release(c, m->ovf_idx);
    }
    m->flag = SLOT_FREE;
    c->n_evict++;
}
```

**确认 (a)**：`m->flag = SLOT_FREE`（末行）。⇒ 溢出路径「ovf_alloc 与 grow_locked 双双失败 ⇒ goto done」后，槽 meta 是 SLOT_FREE（evict_occupant 在溢出路径之前已把 occupant 淘汰并置 FREE），**无 IN_OVF 陈旧 meta、无野 ovf_idx**。状态一致。

**确认 (b)**：按 `m->gen` 区分 `cur_live--` / `old_live--`（第 5 行）。⇒ 覆盖旧代占用槽时正确减 old_live，旧池能排空，gen_switch 不会永久延后；A4（cur_live+old_live==IN_RING 槽数）跨代成立。

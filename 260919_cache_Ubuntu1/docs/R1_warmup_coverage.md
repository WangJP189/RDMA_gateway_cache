# R1 预热覆盖验证（exp1a 统一预热尺寸表）

**目标**：exp1a 的 store 延迟基准里，每个方法的 store 路径都有不同的 glibc malloc
尺寸类。若预热集覆盖不全，未暖到的尺寸类首次 malloc 会付「冷分配器」成本，系统性
高估该方法的 store 延迟（旧 `store_samples.csv` 就因此高估 fifo/hash/tree，见
README_NOTES 第 17 条）。本文从源码算出每个方法 store 路径的真实 malloc 尺寸，
检查旧预热集 `{5120, 1056, 512}` 的覆盖，产出 exp1a 的统一预热表。

## 1. 方法集（exp1a）

fifo / chained_hash / balanced_tree / psn_dynblock + 对照 D（`*_pool` 预分配池）
+ 对照 E（index_only）。payload ∈ {64, 1024, 4096} B。

## 2. 节点结构尺寸（源码逐字段）

来源：旧实现 `260912/psn_behavior_bench/behavior_bench.c`（新 baselines 沿用同一
24B 头，与 `include/dynblock.h` 的 `mem_block_header` 一致）。

```
struct mem_block_header { int data_len(4); uint64_t recv_stamp(8); uint32_t psn(4); }
    → 4 + pad4 + 8 + 4 + pad4 = 24 B

struct fifo_node { next(8) + hdr(24) }        → 32 B
struct hash_node { next(8) + hdr(24) }        → 32 B
struct avl_node  { l(8) + r(8) + h(4) + pad4 + hdr(24) }  → 48 B
```

store 路径的 malloc 请求尺寸 = `sizeof(node) + payload_len`。

## 3. malloc 请求尺寸表（字节）

| 方法 | @64B | @1024B | @4096B | store 路径是否 malloc |
|---|---|---|---|---|
| fifo | **96** | **1056** | **4128** | 是（每 store 一次，不 free） |
| chained_hash | **96** | **1056** | **4128** | 是（每 store 一次，不 free） |
| balanced_tree | **112** | **1072** | **4144** | 是（每 store 一次，不 free） |
| psn_dynblock | 0 | 0 | 0 | **否**：环池 init 时 mmap(MAP_NORESERVE)，store=memcpy 进池 |
| 对照 D（*_pool） | 0 | 0 | 0 | **否**：池 init 时预分配，store=memcpy |
| 对照 E（index_only） | 0 | 0 | 0 | **否**：只写 meta 槽 |

> psn_dynblock 的溢出路径会 `malloc(HDR_SZ + len)`（24+len），但 exp1a 里
> payload ≤ S=4096 恒走环路径，溢出不触发，故 store 路径 malloc-free。

## 4. 旧预热集覆盖检查

旧集 `{5120, 1056, 512}`（`warm_allocator(·, 2N)`）：

| 尺寸 | 命中谁 | 结论 |
|---|---|---|
| 5120 | psn_fixed 固定块 | **无关**（新方法集无 psn_fixed） |
| 1056 | fifo/hash @1024 | 覆盖 |
| 512 | （旧 tiered 相关） | **无关** |

**遗漏**（exp1a 需要但旧集未暖）：
- `96`  = fifo/hash @64
- `112` = tree @64
- `1072` = tree @1024（≠ 1056！tree 节点比 hash/fifo 大 16B）
- `4128` = fifo/hash @4096
- `4144` = tree @4096

## 5. exp1a 统一预热表（产出）

```c
/* 统一预热：每个方法 store 路径的真实 malloc 尺寸 × 预热次数（见 §6 次数口径） */
warm_allocator(96,   WARM_N);   /* fifo/hash @64 */
warm_allocator(112,  WARM_N);   /* tree @64 */
warm_allocator(1056, WARM_N);   /* fifo/hash @1024 */
warm_allocator(1072, WARM_N);   /* tree @1024 */
warm_allocator(4128, WARM_N);   /* fifo/hash @4096 */
warm_allocator(4144, WARM_N);   /* tree @4096 */
```

即补缺集合 = `{96, 112, 1056, 1072, 4128, 4144}`。psn_dynblock / 对照 D / 对照 E
无需 malloc 预热（store 路径 malloc-free）；但对照 D 的池 init 分配应与 psn_dynblock
同用 mmap（`pool_use_mmap=1`），使「池 vs 每次 malloc」成为 D 唯一变量。

## 6. 预热次数 / 公平性 / 池 touch（三条硬约束）

1. **预热次数 ≥ 计时区间的 store 次数**：旧口径 `2N=20480` 只覆盖 exp1a 计时区间
   `timed_ops=200000` 的 10%，计时后半段分配器又会冷下去。取
   `WARM_N = max(2*N, timed_ops)`（exp1a 即 200000）。
2. **无界基线的预热只能「部分公平」**：fifo/hash/tree 每 store malloc 且不 free，
   预热后仍会向 brk 扩展、持续缺页 ⇒ 单靠预热抹不平。**真正的公平必须靠 `*_bounded`
   变体（预分配池 = 零 malloc）**——因此 `*_bounded` 是**必做**、不是可选；README 必须
   写明「无界基线经预热后仍只部分公平，对比结论以 `*_bounded` 为准」。
3. **池要实际 touch（对照 D / psn_dynblock）**：环池 mmap + MAP_NORESERVE ⇒ 首次写
   才缺页、会污染前几个计时样本。计时前对池「每页写一个字节」预热（或
   `madvise(addr, len, MADV_WILLNEED)`）。**禁止 memset 整池**——那会破坏 NORESERVE
   的惰性提交语义（反而把页全提交了）。

## 7. 交叉核对

- `96 = 32 + 64` ✓、`112 = 48 + 64` ✓（与 README_NOTES 第 16 条「fifo/hash@64B=96、
  tree@64B=112」一致）。
- 尺寸表随 payload 集变化：exp1a 用 `{64,1024,4096}`；若改 payload 集，须按
  `sizeof(node)+len` 重算并重跑本验证。

## 8. 待办挂起（Step 11 落地时复核）

本表基于旧实现的节点结构（24B 头 + 指针布局）。Step 11 写新 baselines 时，**节点结构
必须逐字段与旧实现一致**（尤其 mem_block_header=24B），否则本表失效；写完后再跑一次
本验证确认 `sizeof` 未漂移。

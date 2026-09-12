# 实验三：存 / 取行为（跨 6 种缓存方法对比）

独立复刻 6 种缓存方法，对比**存 / 取处理延迟**与**行为确定性**，证明本文
「PSN 分档动态块」方法的优势。这是论文「图 3」的数据来源。

```bash
make
./behavior_bench -o ./out             # 默认 N=10240, L=1024, B=128, R=100
python3 ../paper_figures/plot_figures.py   # 生成论文组合图（图 1/2/3）
```

## 对比的 6 种方法

| 方法 | 结构 | 查找复杂度 | 按序取回 |
|------|------|-----------|---------|
| `fifo` | 链表顺序 append | O(n) 扫描 | 需整理（O(n)） |
| `chained_hash` | 16384 桶链式哈希 | O(1) | **无序** |
| `balanced_tree` | AVL 平衡树 | O(log n) | 有序但需比较 |
| `psn_fixed` | PSN 单环 + 固定 5KB 块 | O(1) | 有序，零整理 |
| `psn_dynamic` | PSN 单环 + 逐包 malloc | O(1) | 有序，零整理 |
| `psn_tiered` | **PSN 分档动态块（本文）** | O(1) | 有序，零整理 |

统一计数器：`n_cmp`（PSN 比较次数，零整理证据）、`n_malloc`（malloc 次数）。

## 指标

| 指标 | 说明 | 论文用途 |
|------|------|---------|
| store 延迟（顺序/乱序，L=1024） | 存一个包的总延迟（含 payload 拷贝） | 图 1(c)：固定 5KB 慢 5× |
| store 控制面开销（64B） | 存一个包的数据结构开销（分配/索引/指针，隔离大 memcpy） | 图 3(b)：tiered 最低 |
| retrieve 延迟（按序取回/命中/丢包判定） | 取一个包的延迟 | 图 1(d)：FIFO O(n) 重排 8.8µs |
| PSN 比较次数 / 取回 | 取回一次平均比较几次 PSN | 图 3(a)：本文 = 0（零整理） |
| malloc / 覆盖写 store | 稳态覆盖写时每 store 的 malloc 次数 | 图 3(c)：分档池 = 0 |
| 丢包判定延迟 vs 丢包率 | 未命中包的判定延迟随丢包率变化 | 图 3(d)：O(1) 与丢包率无关 |
| 判定准确率 | 命中/丢包判定正确率 | 恒 100% |

## 任务

| 任务 | 内容 | 证明 |
|------|------|------|
| A | 顺序存储（psn 0..N-1 递增 store） | store 正确，payload+PSN 全量校验 |
| B | 乱序存储（随机置换）→ 顺序取回 | 乱序 store 后按序取回，**排序操作数 = 0**（零整理） |
| C | 随机丢包取回（丢包率 1%） | 丢包判定 O(1) 且准确 |
| D | 突发丢包取回（连续 512 包） | 突发丢包同样 100% 判定 |
| E | 丢包率扫描（0%~10%） | store/retrieve 延迟与丢包率无关、判定恒 100% |
| 附 | 覆盖写稳态 malloc 计数 | 分档池复用，0 次 malloc |

## 输出

- `out/behavior_summary.csv` — 6 方法延迟 + 比较次数 + 正确率总表
- `out/behavior_sweep.csv` — 丢包率 × 方法 的 miss 延迟与准确率
- `out/behavior_alloc.csv` — PSN 三变体的 malloc/覆盖写 store

延迟用 `rdtsc` 批量（B=128 包）计时并标定 TSC→ns，避免 VM 的 `rdtscp/lfence`
VM-exit 噪声；随机序列用 xorshift32 固定种子（0x9E3779B9）可复现。
分配器在计时前 `warm_allocator` 预热（模拟网关长驻进程稳态），store 源数据
用单块热 `scratch`（64B 头模式按 PSN 生成），避免冷 10MB 数组把 memcpy 拉慢、
污染数据。控制面开销单独用 64B 负载隔离出「分配器 + 索引 + 指针」的真实开销。

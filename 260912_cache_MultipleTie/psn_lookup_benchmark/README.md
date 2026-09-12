# psn_lookup_benchmark — RDMA 网关「PSN 查找延迟」微基准（时间开销）

单机微基准：对比四种「按 PSN 缓存/查找报文」的数据结构的查找延迟，是论文的
**时间开销**证据。

## 对比方法

| key | 方法 | 复杂度 |
|-----|------|--------|
| `fifo` | FIFO 队列（线性扫描） | O(n) |
| `chained_hash` | 链式哈希 | O(1) 均摊（无序） |
| `balanced_tree` | AVL 平衡树 | O(log n) |
| `psn_mapping` | PSN 映射（环形数组直接下标，本文） | O(1) 且按序 |

## 编译与运行

```bash
make                              # gcc -O2 -Wall -std=gnu11 psn_bench.c -o psn_bench
./psn_bench -o ./out              # 查找延迟 CDF + summary.csv
./psn_bench -o ./out --sweep      # 中位延迟 vs 缓存占用 N → scaling.csv
./psn_bench -o ./out --mode range # 按序提取连续 PSN 区间 → range_summary.csv
python3 plot_cdf.py --dir ./out --out ./out --all   # 画 cdf_lookup + scaling
```

## 测量方法（重要）

用**批量计时 + 普通 rdtsc**：把 B 次查找打包成一批，用一对 `rdtsc` 计时再除以 B。
这样可把 `rdtsc` 自身固定开销（VMware 里 lfence/rdtscp 触发 VM exit 可达微秒级）
摊薄到每次查找，避免其淹没 O(1) 与 O(n) 间的纳秒级差异。

## 输出

- `summary.csv`：各方法 × 访问模式的 min/p50/p90/p99/p99.9/max/mean 延迟
- `cdf_<方法>_<模式>.csv`：已排序的每包延迟样本
- `scaling.csv`：方法 × 缓存占用 N 的中位延迟（复杂度曲线）
- `range_summary.csv`：按序提取连续 PSN 区间的每包延迟
- `cdf_lookup.png/pdf`、`scaling.png/pdf`

## 结论

PSN Mapping 查找 P50 ≈ 9ns（哈希 18ns / 树 33ns / FIFO 2.1µs），
且延迟不随缓存占用 N 增长。详见根目录 [ANALYSIS.md](../ANALYSIS.md)。

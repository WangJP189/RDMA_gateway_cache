# psn_space_bench — RDMA 网关「缓存空间开销」微基准

与 `psn_lookup_benchmark`（时间开销）互补，本目录量化不同缓存数据结构的
**空间开销 / 空间利用率**，构成论文「时间 + 空间」两组证据。

## 1. 对比的方法

| key | 方法 | 内存分配方式 |
|-----|------|-------------|
| `fifo` | FIFO 队列 | 指针数组 `N×8B` + 每包一个内联报文结构 |
| `chained_hash` | 链式哈希 | 桶数组 `4096×8B` + 每包一个节点(含 next 指针) + payload 块 |
| `balanced_tree` | AVL 平衡树 | 每包一个节点(含 left/right 指针) + payload 块 |
| `psn_map_fixed` | PSN 映射（现状） | 每包 `malloc(5120)` + 环形数组 `10240×8B` |
| `psn_map_dynamic` | PSN 映射（新机制） | 每包 `malloc(header+payload)` + 环形数组 |
| `contiguous` | 直接叠加包（理想下界） | N 个 `[header+payload]` 紧密拼接，无对齐/无每包 malloc |

## 2. 指标

- `allocated_bytes`：总分配字节（逐项统计 malloc / 数组 / 节点 / 对齐）
- `payload_bytes = N × L`
- `utilization = payload_bytes / allocated_bytes`（0~1，越大越好）
- `avg_bytes_per_pkt = allocated_bytes / N`

## 3. 分配模型的关键假设

1. 每次 `malloc(n)` 实际占用 `ALIGN16(n)` 字节（与 glibc 的 16 字节对齐一致）；
   glibc 内部 chunk 头（约 8~16B/块）在六种方法里近似相同，相对比较时被抵消，
   故**不计入**。
2. PSN 映射的环形数组固定为 `RING_SIZE(10240) × 8B = 80KB / 连接`，
   这正是「以空间换时间」付出的空间代价。
3. 结构体大小用真实 `sizeof()` 计算：`mem_block_header=24B`、
   `fifo_packet=16B`、`hash_node=24B`(→32B)、`avl_node=40B`(→48B)。

> 诚实性说明：绝对字节值受结构体定义与对齐规则影响；论文结论基于
> **相对差异与曲线形态**，而非绝对数字。

## 4. 编译与运行

```bash
make                          # gcc -O2 -Wall -std=gnu11 space_bench.c -o space_bench
./space_bench -o ./out        # 生成 space_summary.csv
./space_bench -o ./out --multiflow   # 额外生成 space_multiflow.csv
python3 plot_space.py --dir ./out --out ./out              # 画利用率 + 绝对空间
python3 plot_space.py --dir ./out --out ./out --multiflow  # 额外画多流摊销
```

## 5. 输出文件

- `space_summary.csv`：`method,packet_size,N,payload_bytes,allocated_bytes,utilization,avg_bytes_per_pkt`
- `space_multiflow.csv`（`--multiflow`）：`scenario,K,npp,total_packets,packet_size,ring_bytes,block_bytes,allocated_bytes,utilization`
- `space_utilization.png/pdf`：横轴 = 包大小 L（log），纵轴 = 空间利用率 %，6 条线。
  预期：`contiguous` 一直 ≈100%；`psn_map_dynamic` 从小包的较低利用率爬升到
  逼近 `contiguous`；`psn_map_fixed` 一直很低；`fifo/hash/tree` 中等且缓升。
- `space_absolute.png/pdf`：横轴 = L，纵轴 = 每包平均分配字节（log），展示绝对空间消耗。
- `space_multiflow.png/pdf`：环形数组 80KB 固定开销随占用深度被摊薄。

## 6. 论文解读要点（供作者）

- **固定 5KB 块的问题**：小包（64B）时 `psn_map_fixed` 利用率仅 ~1%，浪费 99%，
  是动态块机制要解决的痛点。
- **动态块机制**：`malloc(header + payload)` 把内部碎片压到 16B 对齐粒度，
  利用率随包大小单调爬升，最终逼近 `contiguous` 下界。
- **PSN 映射的空间代价 = 环形数组 80KB/连接**：在多流满载时每包摊销约 8B，
  相对块大小可忽略；这也是「以空间换时间」的量化依据。

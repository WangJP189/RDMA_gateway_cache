# exp1a 分配策略敏感性 — 脚本提取（pooled vs perstore）

> 唯一变量 = 分配策略（pooled 复用 / perstore 逐 store malloc+free）；索引结构 / 淘汰 / 计时口径完全相同（alloc_equiv_test 验证字节一致）。

> 主指标 p50，B=2048；pooled n_malloc=0，perstore n_malloc=n_free=实际 store 次数。


## p50 (ns) — pooled vs perstore（4 结构 × 5 payload）


### FIFO Queue

| payload | pooled p50 | perstore p50 | Δ (ns) | Δ% |
|---|---|---|---|---|
| 256 B | 9.465 | 11.468 | +2.003 | +21.2% |
| 512 B | 12.479 | 15.472 | +2.993 | +24.0% |
| 1024 B | 23.191 | 27.565 | +4.374 | +18.9% |
| 2048 B | 43.744 | 46.827 | +3.083 | +7.0% |
| 4096 B | 89.068 | 90.567 | +1.499 | +1.7% |
| **均值** | | | | **+14.5%** |

### Chained Hash

| payload | pooled p50 | perstore p50 | Δ (ns) | Δ% |
|---|---|---|---|---|
| 256 B | 8.807 | 12.360 | +3.553 | +40.3% |
| 512 B | 11.967 | 16.038 | +4.071 | +34.0% |
| 1024 B | 23.198 | 28.726 | +5.528 | +23.8% |
| 2048 B | 42.379 | 48.514 | +6.135 | +14.5% |
| 4096 B | 89.436 | 95.655 | +6.219 | +7.0% |
| **均值** | | | | **+23.9%** |

### Balanced Tree

| payload | pooled p50 | perstore p50 | Δ (ns) | Δ% |
|---|---|---|---|---|
| 256 B | 45.853 | 52.301 | +6.448 | +14.1% |
| 512 B | 49.763 | 55.618 | +5.855 | +11.8% |
| 1024 B | 53.512 | 66.270 | +12.758 | +23.8% |
| 2048 B | 68.043 | 82.587 | +14.544 | +21.4% |
| 4096 B | 112.217 | 128.436 | +16.219 | +14.5% |
| **均值** | | | | **+17.1%** |

### PSN Mapping

| payload | pooled p50 | perstore p50 | Δ (ns) | Δ% |
|---|---|---|---|---|
| 256 B | 8.050 | 13.022 | +4.972 | +61.8% |
| 512 B | 11.609 | 17.625 | +6.016 | +51.8% |
| 1024 B | 22.851 | 27.579 | +4.728 | +20.7% |
| 2048 B | 41.860 | 48.897 | +7.037 | +16.8% |
| 4096 B | 88.625 | 92.980 | +4.355 | +4.9% |
| **均值** | | | | **+31.2%** |

## 裸 malloc/free 参考（alloc_probe，B=2048，p50 ns/op）

| n_bytes | 语义 | p50 (ns) |
|---|---|---|
| 32 | 32 B（fifo/hash 节点） | 4.917 |
| 56 | 56 B（avl 节点） | 5.014 |
| 64 | 64 B（align16(56)） | 4.887 |
| 4128 | 4128 B（32+4096，perstore 最大 malloc） | 11.303 |

## 关键结论（脚本计算，禁手抄）

- **FIFO Queue**：perstore 相对 pooled 平均 **+14.5%**（5 档 payload）——逐 store malloc/free 的固定开销，随 payload 增大被 memcpy 摊薄。
- **Chained Hash**：perstore 相对 pooled 平均 **+23.9%**（5 档 payload）——逐 store malloc/free 的固定开销，随 payload 增大被 memcpy 摊薄。
- **Balanced Tree**：perstore 相对 pooled 平均 **+17.1%**（5 档 payload）——逐 store malloc/free 的固定开销，随 payload 增大被 memcpy 摊薄。
- **PSN Mapping**：perstore 相对 pooled 平均 **+31.2%**（5 档 payload）——逐 store malloc/free 的固定开销，随 payload 增大被 memcpy 摊薄。

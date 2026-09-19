# exp1a 分配策略敏感性 — 脚本提取（pooled vs perstore）

> 唯一变量 = 分配策略（pooled 复用 / perstore 逐 store malloc+free）；索引结构 / 淘汰 / 计时口径完全相同（alloc_equiv_test 验证字节一致）。

> 主指标 p50，B=2048；pooled n_malloc=0，perstore n_malloc=n_free=实际 store 次数。


## p50 (ns) — pooled vs perstore（4 结构 × 5 payload）


### FIFO Queue

| payload | pooled p50 | perstore p50 | Δ (ns) | Δ% |
|---|---|---|---|---|
| 256 B | 9.439 | 11.468 | +2.029 | +21.5% |
| 512 B | 12.191 | 15.472 | +3.281 | +26.9% |
| 1024 B | 23.205 | 27.565 | +4.360 | +18.8% |
| 2048 B | 42.903 | 46.827 | +3.924 | +9.1% |
| 4096 B | 85.933 | 90.567 | +4.634 | +5.4% |
| **均值** | | | | **+16.3%** |

### Chained Hash

| payload | pooled p50 | perstore p50 | Δ (ns) | Δ% |
|---|---|---|---|---|
| 256 B | 7.366 | 12.360 | +4.994 | +67.8% |
| 512 B | 11.910 | 16.038 | +4.128 | +34.7% |
| 1024 B | 22.837 | 28.726 | +5.889 | +25.8% |
| 2048 B | 42.314 | 48.514 | +6.200 | +14.7% |
| 4096 B | 87.515 | 95.655 | +8.140 | +9.3% |
| **均值** | | | | **+30.4%** |

### Balanced Tree

| payload | pooled p50 | perstore p50 | Δ (ns) | Δ% |
|---|---|---|---|---|
| 256 B | 46.208 | 52.301 | +6.093 | +13.2% |
| 512 B | 49.438 | 55.618 | +6.180 | +12.5% |
| 1024 B | 53.607 | 66.270 | +12.663 | +23.6% |
| 2048 B | 68.592 | 82.587 | +13.995 | +20.4% |
| 4096 B | 111.313 | 128.436 | +17.123 | +15.4% |
| **均值** | | | | **+17.0%** |

### PSN Mapping

| payload | pooled p50 | perstore p50 | Δ (ns) | Δ% |
|---|---|---|---|---|
| 256 B | 8.025 | 13.022 | +4.997 | +62.3% |
| 512 B | 11.572 | 17.625 | +6.053 | +52.3% |
| 1024 B | 22.787 | 27.579 | +4.792 | +21.0% |
| 2048 B | 42.109 | 48.897 | +6.788 | +16.1% |
| 4096 B | 88.480 | 92.980 | +4.500 | +5.1% |
| **均值** | | | | **+31.4%** |

## 裸 malloc/free 参考（alloc_probe，B=2048，p50 ns/op）

| n_bytes | 语义 | p50 (ns) |
|---|---|---|
| 32 | 32 B（fifo/hash 节点） | 4.917 |
| 56 | 56 B（avl 节点） | 5.014 |
| 64 | 64 B（align16(56)） | 4.887 |
| 4128 | 4128 B（32+4096，perstore 最大 malloc） | 11.303 |

## 关键结论（脚本计算，禁手抄）

- **FIFO Queue**：perstore 相对 pooled 平均 **+16.3%**（5 档 payload）——逐 store malloc/free 的固定开销，随 payload 增大被 memcpy 摊薄。
- **Chained Hash**：perstore 相对 pooled 平均 **+30.4%**（5 档 payload）——逐 store malloc/free 的固定开销，随 payload 增大被 memcpy 摊薄。
- **Balanced Tree**：perstore 相对 pooled 平均 **+17.0%**（5 档 payload）——逐 store malloc/free 的固定开销，随 payload 增大被 memcpy 摊薄。
- **PSN Mapping**：perstore 相对 pooled 平均 **+31.4%**（5 档 payload）——逐 store malloc/free 的固定开销，随 payload 增大被 memcpy 摊薄。

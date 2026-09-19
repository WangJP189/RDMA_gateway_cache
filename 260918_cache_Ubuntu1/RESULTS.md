# RESULTS — 实验结果（数字由脚本从各 CSV 提取）

> 生成：各实验 `plot_expX_*.py` 写 `*_numbers.md`，再由 `paper_figures/assemble_results.py` 拼成本文；表内数字禁止手抄。
> 环境：实机（见 docs/ENV_CHECK.md）。

---

## exp1a — store time cost（存时间开销）

**图：** `paper_figures/fig_exp1a_store.pdf`（同目录同名 `.png` @300dpi）。

横轴 packet size（256/512/1024/2048/4096 B，RDMA 5 档 MTU）、纵轴 store time cost（ns），双对数；N=4096，主指标 p50、次指标 p90。图内 4 方法；index_only 与 PSN(fixed S=4096) 只入下表。结论：PSN Mapping 存时间与 FIFO/Chained Hash 同阶（O(1)）：256B 略慢于 Chained Hash（8.0 vs 7.4 ns）但快于 FIFO（9.4 ns），512–2048B 反超成为最快，4096B 与 FIFO/Hash 基本持平（±3%），全程远优于 Balanced Tree（O(log N)）——验证「PSN 映射与线性结构同阶、远优于树」（自适应 S 收敛到各档，见「收敛后 S」表）。

## B=2048 主矩阵 · p50 (ns) — 主指标

| method | 256 B | 512 B | 1024 B | 2048 B | 4096 B |
|---|---|---|---|---|---|
| FIFO Queue | 9.439 | 12.191 | 23.205 | 42.903 | 85.933 |
| Chained Hash | 7.366 | 11.910 | 22.837 | 42.314 | 87.515 |
| Balanced Tree | 46.208 | 49.438 | 53.607 | 68.592 | 111.313 |
| PSN Mapping (adaptive S) | 8.025 | 11.572 | 22.787 | 42.109 | 88.480 |
| PSN Mapping (fixed S=4096) | 10.956 | 16.179 | 27.550 | 48.713 | 88.032 |
| index-only ($\Phi$) | 1.096 | 1.096 | 1.096 | 1.096 | 1.096 |

## B=2048 主矩阵 · p90 (ns) — 次指标

| method | 256 B | 512 B | 1024 B | 2048 B | 4096 B |
|---|---|---|---|---|---|
| FIFO Queue | 9.725 | 12.607 | 23.676 | 43.525 | 97.359 |
| Chained Hash | 7.548 | 12.124 | 23.514 | 43.043 | 100.397 |
| Balanced Tree | 46.778 | 49.991 | 68.162 | 69.399 | 129.242 |
| PSN Mapping (adaptive S) | 8.222 | 11.735 | 23.045 | 42.687 | 97.277 |
| PSN Mapping (fixed S=4096) | 11.289 | 16.773 | 32.664 | 52.501 | 96.400 |
| index-only ($\Phi$) | 1.098 | 1.098 | 1.098 | 1.098 | 1.098 |

## B=1024 交叉验证 · p50 (ns) — 排序/差距一致性

| method | 256 B | 512 B | 1024 B | 2048 B | 4096 B |
|---|---|---|---|---|---|
| FIFO Queue | 6.492 | 11.110 | 22.099 | 41.970 | 86.061 |
| Chained Hash | 7.358 | 11.968 | 22.798 | 42.411 | 87.667 |
| Balanced Tree | 46.251 | 49.338 | 53.458 | 68.779 | 110.707 |
| PSN Mapping (adaptive S) | 7.825 | 11.410 | 22.881 | 42.217 | 88.662 |
| PSN Mapping (fixed S=4096) | 10.935 | 16.214 | 27.729 | 49.184 | 88.729 |
| index-only ($\Phi$) | 1.100 | 1.100 | 1.100 | 1.100 | 1.100 |

## 收敛后 S（dynblock）

- PSN Mapping (adaptive S): 256 B→S=256, 512 B→S=512, 1024 B→S=1024, 2048 B→S=2048, 4096 B→S=4096
- PSN Mapping (fixed S=4096): 256 B→S=4096, 512 B→S=4096, 1024 B→S=4096, 2048 B→S=4096, 4096 B→S=4096

---

## exp1a-alloc — 分配策略敏感性（第 5 条正交矩阵：pooled vs perstore）

**图：** `paper_figures/fig_exp1a_alloc_sensitivity.pdf`（同目录同名 `.png` @300dpi）。

横轴 packet size、纵轴 store time cost（ns），双对数；4 结构各两条线（pooled 实线 / perstore 虚线）。唯一变量 = 分配策略：pooled 一次性预分配+复用（n_malloc=0）、perstore 逐 store malloc/free （n_malloc=n_free=实际 store 次数）；索引结构 / 淘汰 / 计时口径相同（alloc_equiv_test 验证字节一致）。结论：perstore 相对 pooled 的固定开销 ≈ 裸 malloc/free 成本，随 payload 增大被 memcpy 摊薄（见下表 Δ/Δ%）。

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

---

## exp1b — lookup time cost（取时间开销）

**图：** `paper_figures/fig_exp1b_lookup_gbn64.pdf`（同目录同名 `.png` @300dpi）。

横轴 Cache depth N（128/256/512/1024/2048/4096/5120）、纵轴 lookup time cost（ns），双对数；payload=1024、B=512、reps=5，主指标 p50。两条 PSN 变体：S0=payload（槽大小固定=包长）与 adaptive S（弹性槽大小）。第 4B 统一交付契约：retrieve_set 按 PSN 升序交付（fifo/hash qsort、tree 中序、dynblock 扫槽；SR 图另见 `fig_exp1b_lookup_sr64.pdf`）。结论：① GBN（retrieve_range 天然升序）：FIFO O(N)（n_cmp≈N/2）、Tree O(log N)、Hash O(1)，而 PSN Mapping 两变体 n_cmp=0、与 index-only Φ 同阶（O(1) 平坦），持平/反超 Hash、远优于 Tree/FIFO；② SR（retrieve_set 升序交付）：fifo/hash 显式 qsort O(k log k)、tree 中序 O(N)、dynblock 扫槽 O(N)——小 N 下 tree/dynblock 零排序交付占优，大 N 下 hash 的 qsort 占优（见 numbers.md 表）。

> payload=1024，B=512，reps=5；主指标 p50_ns（实机尾部干净，p50 稳健）。

> 第 4B：retrieve_set 统一按 PSN 升序交付（fifo/hash qsort、tree 中序、dynblock 扫槽）。

> 读钟地板：B=512 → 0.196 ns/op；B=32 → 0.283 ns/op（floor ∝ 1/B 闭环）。


## p50 (ns) — gbn_long64（retrieve_range，天然升序）

| method | 128 | 256 | 512 | 1024 | 2048 | 4096 | 5120 |
|---|---|---|---|---|---|---|---|
| FIFO Queue | 4462.5 | 8397.4 | 16508.7 | 35186.3 | 69513.6 | 139020.0 | 172974.6 |
| Chained Hash | 1016.3 | 1008.6 | 1010.1 | 1013.0 | 1065.7 | 1187.6 | 1216.5 |
| Balanced Tree | 1070.5 | 1102.0 | 1142.3 | 1188.5 | 1300.2 | 1521.8 | 1577.6 |
| PSN Mapping (S0=payload) | 992.6 | 992.3 | 995.7 | 996.4 | 1026.7 | 1104.0 | 1132.3 |
| PSN Mapping (adaptive S) | 994.9 | 991.8 | 994.9 | 996.9 | 1024.8 | 1101.7 | 1130.5 |
| index-only ($\Phi$) | 181.1 | 181.2 | 181.8 | 181.2 | 181.3 | 181.1 | 181.3 |

## p50 (ns) — sr_64（retrieve_set，第 4B 升序交付）

| method | 128 | 256 | 512 | 1024 | 2048 | 4096 | 5120 |
|---|---|---|---|---|---|---|---|
| FIFO Queue | 6296.1 | 10038.6 | 18059.6 | 34489.6 | 67262.7 | 140474.0 | 163426.6 |
| Chained Hash | 2574.4 | 2617.9 | 2651.4 | 2677.3 | 2824.8 | 3046.6 | 3099.3 |
| Balanced Tree | 1745.8 | 3223.5 | 5388.5 | 7660.0 | 10078.7 | 14498.4 | 16370.1 |
| PSN Mapping (S0=payload) | 1391.5 | 2433.8 | 4109.1 | 6622.9 | 8666.8 | 12819.5 | 14846.1 |
| PSN Mapping (adaptive S) | 1388.7 | 2433.3 | 4100.6 | 6629.5 | 8656.6 | 12875.3 | 14885.5 |
| index-only ($\Phi$) | 1737.8 | 1736.7 | 1740.7 | 1740.9 | 1738.1 | 1737.4 | 1737.9 |

## mean (ns) — gbn_long64（上界参考）

| method | 128 | 256 | 512 | 1024 | 2048 | 4096 | 5120 |
|---|---|---|---|---|---|---|---|
| FIFO Queue | 4462.2 | 8420.5 | 16524.8 | 35169.8 | 69665.1 | 138939.1 | 172965.9 |
| Chained Hash | 1017.8 | 1009.8 | 1012.4 | 1015.0 | 1073.9 | 1203.4 | 1247.2 |
| Balanced Tree | 1072.2 | 1102.9 | 1143.1 | 1190.8 | 1308.0 | 1527.3 | 1589.9 |
| PSN Mapping (S0=payload) | 993.3 | 995.6 | 1001.3 | 1009.0 | 1035.0 | 1124.2 | 1166.8 |
| PSN Mapping (adaptive S) | 996.5 | 994.5 | 1001.0 | 1010.0 | 1038.6 | 1119.5 | 1169.8 |
| index-only ($\Phi$) | 181.5 | 181.6 | 182.3 | 181.6 | 181.9 | 181.7 | 181.8 |

## n_cmp（mean_cmp_per_pkt，纯取包比较）— gbn_long64 代表

| method | 128 | 256 | 512 | 1024 | 2048 | 4096 | 5120 | 复杂度 |
|---|---|---|---|---|---|---|---|---|
| FIFO Queue | 64.7 | 128.2 | 255.5 | 510.8 | 1014.4 | 2043.5 | 2556.5 | O(N)≈N/2 |
| Chained Hash | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 | O(1) |
| Balanced Tree | 6.0 | 7.0 | 8.0 | 9.0 | 10.0 | 11.0 | 11.4 | O(log N) |
| PSN Mapping (S0=payload) | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | O(1) Φ |
| PSN Mapping (adaptive S) | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | O(1) Φ |
| index-only ($\Phi$) | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | O(1) Φ |

---

## exp2 — space utilization（空间利用率）

**图：** `paper_figures/fig_exp2_space.pdf`（同目录同名 `.png` @300dpi）。

横轴 packet size（5 档 MTU）、纵轴 space utilization（%，线性 0-100）；utilization = payload_bytes / allocated_bytes（满窗 N=4096，cache_footprint_bytes sizeof 实测）。结论：PSN Mapping（弹性）利用率**反超**原最优基线 FIFO（256B +5.51pp、4096B +0.41pp，5 档全部反超），且优于 Chained Hash / Balanced Tree；而 PSN(fixed S=4096) 在 256B 档崩到 6.2%——证明「弹性内存槽大小机制」解决了固定内存块的空间利用率塌陷。

> N=4096 满窗；utilization = payload_bytes / allocated_bytes（cache_footprint_bytes，sizeof 实测）。


## 空间利用率 (%) — 5 方法 × 5 档 MTU

| method | 256 B | 512 B | 1024 B | 2048 B | 4096 B |
|---|---|---|---|---|---|
| FIFO Queue | 88.89 | 94.12 | 96.97 | 98.46 | 99.22 |
| Chained Hash | 78.05 | 87.67 | 93.43 | 96.60 | 98.27 |
| Balanced Tree | 80.00 | 88.89 | 94.12 | 96.97 | 98.46 |
| PSN Mapping (adaptive S) | 94.40 | 97.12 | 98.54 | 99.26 | 99.63 |
| PSN Mapping (fixed S=4096) | 6.23 | 12.45 | 24.91 | 49.82 | 99.63 |

## 分配总量 allocated_bytes (B) — 满窗 N=4096

| method | 256 B | 512 B | 1024 B | 2048 B | 4096 B |
|---|---|---|---|---|---|
| FIFO Queue | 1179648 | 2228224 | 4325376 | 8519680 | 16908288 |
| Chained Hash | 1343488 | 2392064 | 4489216 | 8683520 | 17072128 |
| Balanced Tree | 1310720 | 2359296 | 4456448 | 8650752 | 17039360 |
| PSN Mapping (adaptive S) | 1110832 | 2159408 | 4256560 | 8450864 | 16839472 |
| PSN Mapping (fixed S=4096) | 16839472 | 16839472 | 16839472 | 16839472 | 16839472 |

## block_S（dynblock 槽大小；非 dynblock=0）

- PSN Mapping (adaptive S): 256 B→S=256, 512 B→S=512, 1024 B→S=1024, 2048 B→S=2048, 4096 B→S=4096
- PSN Mapping (fixed S=4096): 256 B→S=4096, 512 B→S=4096, 1024 B→S=4096, 2048 B→S=4096, 4096 B→S=4096

## 关键结论（脚本计算，供正文）

- FIFO（原最优基线）利用率区间：88.89%–99.22%
- PSN(弹性) 利用率区间：94.40%–99.63%（**反超 FIFO**）
- 256 B：PSN(弹性)−FIFO=+5.51 pp，FIFO−PSN(fixed)=82.66 pp
- 512 B：PSN(弹性)−FIFO=+3.00 pp，FIFO−PSN(fixed)=81.66 pp
- 1024 B：PSN(弹性)−FIFO=+1.57 pp，FIFO−PSN(fixed)=72.06 pp
- 2048 B：PSN(弹性)−FIFO=+0.80 pp，FIFO−PSN(fixed)=48.65 pp
- 4096 B：PSN(弹性)−FIFO=+0.41 pp，FIFO−PSN(fixed)=-0.41 pp
- 弹性反超最小点：4096 B，PSN 仍高 +0.41 pp（5 档全部反超 FIFO）
- fixed 最大塌陷点：256 B，利用率 6.23%（vs FIFO 88.89%）——弹性槽大小机制的价值所在

---

## exp3 — elastic ablation（弹性消融 / 稳定性）

**图：** `paper_figures/fig_exp3_adapt.pdf`（同目录同名 `.png` @300dpi）。

横轴 epoch、纵轴 block size S（B，对数 base2）；两条件同一相位序列（第 3 条重设计：11 相位 = 1 预热 + 5 升档 + 5 降档，每档稳定 16 纪元再切换），仅弹性旋钮不同（ablation 去滞后 vs slow 默认滞后对照）。另报三指标：① 利用率时间序列（fig_exp3_util）、② 逐相位利用率统计（排除切换后首 6 纪元）、③ 收敛时间（每次切换后 S 首次命中目标 MTU 的纪元数）。结论：每档稳定 16 纪元下，去滞后消融与默认滞后**都零丢包零 miss**（hit_rate=1.0000，n_drop=0）；区别在收敛速度（降档：ablation 0.0 纪元 vs slow 1.4 纪元）与 resize 临界区成本（resize_ns mean：ablation 2188 vs slow 3715 ns）——默认滞后的 gen_switch 延后到 old_live 归零、旧池同步 munmap，故临界区峰值延迟更高（见 numbers.md 收敛时间/逐相位利用率表）。

> 两条件同一相位序列（第 3 条：11 相位 = 1 预热 + 5 升档 + 5 降档，每档稳定 16 纪元），仅弹性旋钮不同。


## 弹性旋钮

| 旋钮 | ablation | slow 对照 |
|---|---|---|
| k_dwell（同向纪元数） | 1 | 2 |
| ovf_thresh（阈值扩条数） | 1 | 256 |
| j_quiet（缩前安静纪元数） | 1 | 4 |
| hist_decay（溢出史清空纪元数） | 2 | 8 |

## 收敛时间（每次切换后 S 首次命中目标 MTU 的纪元数；1 纪元 = 1342.18 µs）

| 跳变 | 方向 | ablation（纪元） | slow 对照（纪元） |
|---|---|---|---|
| 4096 B → 256 B | 降档 | 0 | 3 |
| 256 B → 512 B | 升档 | 0 | 0 |
| 512 B → 1024 B | 升档 | 0 | 0 |
| 1024 B → 2048 B | 升档 | 0 | 0 |
| 2048 B → 4096 B | 升档 | 0 | 0 |
| 4096 B → 4096 B | 持平 | 0 | 0 |
| 4096 B → 2048 B | 降档 | 0 | 1 |
| 2048 B → 1024 B | 降档 | 0 | 1 |
| 1024 B → 512 B | 降档 | 0 | 1 |
| 512 B → 256 B | 降档 | 0 | 1 |
| 升档均值（4 次） | | 0.0 | 0.0 |
| 降档均值（5 次） | | 0.0 | 1.4 |
| 持平（1 次，4096 平台） | | 0.0 | 0.0 |

## 相位利用率统计（util_adaptive = payload/allocated；排除切换后首 6 纪元）

| 相位 | MTU | 方向 | ablation mean/min/max/振幅 | slow mean/min/max/振幅 |
|---|---|---|---|---|
| 0 | 4096 B | 预热 | — | — |
| 1 | 256 B | 降档 | 0.955 / 0.955 / 0.955 / 0.000 | 0.955 / 0.955 / 0.955 / 0.000 |
| 2 | 512 B | 升档 | 0.977 / 0.977 / 0.977 / 0.000 | 0.977 / 0.977 / 0.977 / 0.000 |
| 3 | 1024 B | 升档 | 0.988 / 0.988 / 0.988 / 0.000 | 0.988 / 0.988 / 0.988 / 0.000 |
| 4 | 2048 B | 升档 | 0.994 / 0.994 / 0.994 / 0.000 | 0.994 / 0.994 / 0.994 / 0.000 |
| 5 | 4096 B | 升档 | 0.997 / 0.997 / 0.997 / 0.000 | 0.997 / 0.997 / 0.997 / 0.000 |
| 6 | 4096 B | 持平 | 0.997 / 0.997 / 0.997 / 0.000 | 0.997 / 0.997 / 0.997 / 0.000 |
| 7 | 2048 B | 降档 | 0.994 / 0.994 / 0.994 / 0.000 | 0.994 / 0.994 / 0.994 / 0.000 |
| 8 | 1024 B | 降档 | 0.988 / 0.988 / 0.988 / 0.000 | 0.988 / 0.988 / 0.988 / 0.000 |
| 9 | 512 B | 降档 | 0.977 / 0.977 / 0.977 / 0.000 | 0.977 / 0.977 / 0.977 / 0.000 |
| 10 | 256 B | 降档 | 0.955 / 0.955 / 0.955 / 0.000 | 0.955 / 0.955 / 0.955 / 0.000 |

## resize 计数 / 成本

| 指标 | ablation | slow 对照 |
|---|---|---|
| n_resize（总） | 9 | 9 |
| grow（overflow_grow/grow） | 4 | 4 |
| shrink | 5 | 5 |
| resize_ns（单次切换） | mean 2188 / max 3504 | mean 3715 / max 8554 |
| drain_ns（单次排空） | mean 156049 / max 746476 | mean 163773 / max 810453 |

> resize_ns 语义：gen_switch 临界区耗时（pool_alloc/pool_free 的 mmap/munmap + 指针切换）。
> 慢对照的均值被降档收缩的同步 munmap 拉高：其 gen_switch 被延后到 old_live 恰好归零的当次 store，旧池的 munmap 落在计时区内；
> ablation 因 j_quiet=1/k_dwell=1 提前一个纪元排空，旧池经 release_old_if_drained 在普通 store 里释放（不计时），故 resize_ns 只剩 mmap。两者最终都 munmap 同一池，resize_ns 衡量的只是「临界区内的峰值延迟」，不是累计 mmap/munmap 总量。


## 稳定性（丢包 / 命中）

| 指标 | ablation | slow 对照 |
|---|---|---|
| n_drop（溢出满+延后切换丢弃） | 0 | 0 |
| n_ovf_ins（溢出插入数） | 3280 | 3280 |
| hit / miss（NAK 重传） | 13325 / 0 | 13325 / 0 |
| hit_rate | 1.0000 | 1.0000 |

---

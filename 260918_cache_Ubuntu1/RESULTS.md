# RESULTS — 实验结果（数字由脚本从各 CSV 提取）

> 生成：各实验 `plot_expX_*.py` 写 `*_numbers.md`，再由 `paper_figures/assemble_results.py` 拼成本文；表内数字禁止手抄。
> 环境：实机 Ubuntu 22.04.5 / HWE 6.8.0-138 / i7-14700K（见 docs/ENV_CHECK.md）。

---

## exp1a — store time cost（存时间开销）

**图：** `paper_figures/fig_exp1a_store.pdf`（同目录同名 `.png` @300dpi）。

横轴 packet size（256/512/1024/2048/4096 B，RDMA 5 档 MTU）、纵轴 store time cost（ns），双对数；N=4096，主指标 p50、次指标 p90。图内 4 方法；index_only 与 PSN(fixed S=4096) 只入下表。结论：PSN Mapping 在小包（256B）存时间略慢于 FIFO/Chained Hash，在大包（4096B）反超成为最快——验证「PSN 映射小包开销高、大包持平/更快」的假设（自适应 S 收敛到各档，见「收敛后 S」表）。

## B=2048 主矩阵 · p50 (ns) — 主指标

| method | 256 B | 512 B | 1024 B | 2048 B | 4096 B |
|---|---|---|---|---|---|
| FIFO Queue | 8.358 | 12.082 | 23.890 | 43.964 | 91.950 |
| Chained Hash | 8.302 | 11.923 | 23.868 | 44.087 | 92.406 |
| Balanced Tree | 44.762 | 48.165 | 52.734 | 69.177 | 116.451 |
| PSN Mapping (adaptive S) | 9.917 | 13.047 | 23.860 | 43.674 | 89.140 |
| PSN Mapping (fixed S=4096) | 11.009 | 18.710 | 29.745 | 51.076 | 89.200 |
| index-only ($\Phi$) | 1.096 | 1.099 | 1.099 | 1.099 | 1.099 |

## B=2048 主矩阵 · p90 (ns) — 次指标

| method | 256 B | 512 B | 1024 B | 2048 B | 4096 B |
|---|---|---|---|---|---|
| FIFO Queue | 8.427 | 12.213 | 24.855 | 46.418 | 109.081 |
| Chained Hash | 8.467 | 12.336 | 24.602 | 46.487 | 101.683 |
| Balanced Tree | 45.421 | 49.051 | 54.058 | 72.005 | 132.020 |
| PSN Mapping (adaptive S) | 10.057 | 13.665 | 24.376 | 45.362 | 97.479 |
| PSN Mapping (fixed S=4096) | 11.107 | 18.920 | 34.094 | 55.450 | 98.261 |
| index-only ($\Phi$) | 1.098 | 1.103 | 1.103 | 1.104 | 1.103 |

## B=1024 交叉验证 · p50 (ns) — 排序/差距一致性

| method | 256 B | 512 B | 1024 B | 2048 B | 4096 B |
|---|---|---|---|---|---|
| FIFO Queue | 5.998 | 11.818 | 23.178 | 43.925 | 90.295 |
| Chained Hash | 8.260 | 11.936 | 24.207 | 44.074 | 92.217 |
| Balanced Tree | 45.019 | 48.024 | 53.090 | 69.561 | 116.811 |
| PSN Mapping (adaptive S) | 9.670 | 12.979 | 24.082 | 44.399 | 91.961 |
| PSN Mapping (fixed S=4096) | 11.055 | 18.260 | 30.104 | 51.392 | 91.647 |
| index-only ($\Phi$) | 1.100 | 1.100 | 1.100 | 1.100 | 1.100 |

## 收敛后 S（dynblock）

- PSN Mapping (adaptive S): 256 B→S=256, 512 B→S=512, 1024 B→S=1024, 2048 B→S=2048, 4096 B→S=4096
- PSN Mapping (fixed S=4096): 256 B→S=4096, 512 B→S=4096, 1024 B→S=4096, 2048 B→S=4096, 4096 B→S=4096

---

## exp1b — lookup time cost（取时间开销）

**图：** `paper_figures/fig_exp1b_lookup_gbn64.pdf`（同目录同名 `.png` @300dpi）。

横轴 Cache depth N（128/256/512/1024/2048/4096/5120）、纵轴 lookup time cost（ns），双对数；payload=1024、B=512、reps=5，主指标 p50。两条 PSN 变体：S0=payload（槽大小固定=包长）与 adaptive S（弹性槽大小）。结论：① FIFO 呈 O(N)（n_cmp≈N/2）、Tree O(log N)、Hash O(1)，而 PSN Mapping 两变体 n_cmp=0、与 index-only Φ 同阶（O(1) 平坦），持平/反超 Hash、远优于 Tree/FIFO；② reorder 增量对所有方法对称（见「reorder 增量」表，非 PSN 特有劣势）。SR 图另见 `fig_exp1b_lookup_sr64.pdf`（reorder=0 实线 / reorder=1 虚线）。

> payload=1024，B=512，reps=5；主指标 p50_ns（实机尾部干净，p50 稳健）。

> 读钟地板：B=512 → 0.196 ns/op；B=32 → 0.283 ns/op（floor ∝ 1/B 闭环）。


## p50 (ns) — gbn_long64，reorder=0（GBN 天然有序）

| method | 128 | 256 | 512 | 1024 | 2048 | 4096 | 5120 |
|---|---|---|---|---|---|---|---|
| FIFO Queue | 4459.3 | 8243.9 | 16111.8 | 32688.6 | 64949.3 | 130083.6 | 174082.3 |
| Chained Hash | 995.2 | 1013.0 | 1015.9 | 1017.1 | 1083.2 | 1214.1 | 1244.2 |
| Balanced Tree | 1061.1 | 1090.0 | 1142.5 | 1205.0 | 1336.7 | 1576.1 | 1644.1 |
| PSN Mapping (S0=payload) | 971.6 | 972.2 | 972.2 | 974.5 | 1010.3 | 1131.6 | 1148.2 |
| PSN Mapping (adaptive S) | 973.8 | 973.1 | 971.6 | 972.8 | 1008.6 | 1126.8 | 1150.4 |
| index-only ($\Phi$) | 192.6 | 192.6 | 192.6 | 192.6 | 192.6 | 192.6 | 192.6 |

## p50 (ns) — sr_64，reorder=0（仅取包）

| method | 128 | 256 | 512 | 1024 | 2048 | 4096 | 5120 |
|---|---|---|---|---|---|---|---|
| FIFO Queue | 4621.0 | 8616.1 | 16812.7 | 35024.1 | 71067.4 | 139543.7 | 175430.5 |
| Chained Hash | 1013.4 | 1024.8 | 1047.9 | 1065.3 | 1213.0 | 1495.8 | 1575.4 |
| Balanced Tree | 1249.6 | 1450.5 | 1683.4 | 1951.8 | 2381.0 | 3013.9 | 3331.7 |
| PSN Mapping (S0=payload) | 1009.4 | 1026.0 | 1058.9 | 1081.3 | 1187.9 | 1450.2 | 1652.8 |
| PSN Mapping (adaptive S) | 1007.6 | 1026.8 | 1059.3 | 1080.5 | 1180.4 | 1450.1 | 1683.7 |
| index-only ($\Phi$) | 193.2 | 191.8 | 191.8 | 191.8 | 191.8 | 191.8 | 191.9 |

## p50 (ns) — sr_64，reorder=1（取包前升序整理）

| method | 128 | 256 | 512 | 1024 | 2048 | 4096 | 5120 |
|---|---|---|---|---|---|---|---|
| FIFO Queue | 5329.3 | 9336.0 | 17482.8 | 35726.2 | 71858.7 | 140568.5 | 176352.6 |
| Chained Hash | 1699.6 | 1716.1 | 1742.9 | 1765.7 | 1910.8 | 2208.5 | 2365.3 |
| Balanced Tree | 1940.9 | 2120.0 | 2359.5 | 2592.5 | 2949.9 | 3515.0 | 3746.9 |
| PSN Mapping (S0=payload) | 1686.8 | 1714.1 | 1752.3 | 1781.1 | 1883.9 | 2157.8 | 2347.1 |
| PSN Mapping (adaptive S) | 1687.7 | 1715.8 | 1752.2 | 1781.9 | 1889.8 | 2152.8 | 2375.1 |
| index-only ($\Phi$) | 903.1 | 908.8 | 911.5 | 906.7 | 907.6 | 909.4 | 911.5 |

## mean (ns) — gbn_long64，reorder=0（上界参考）

| method | 128 | 256 | 512 | 1024 | 2048 | 4096 | 5120 |
|---|---|---|---|---|---|---|---|
| FIFO Queue | 4459.9 | 8282.1 | 16171.1 | 32689.4 | 65131.7 | 129991.4 | 174092.0 |
| Chained Hash | 998.3 | 1015.2 | 1018.9 | 1022.2 | 1091.0 | 1248.0 | 1274.0 |
| Balanced Tree | 1067.2 | 1096.2 | 1147.2 | 1211.7 | 1345.8 | 1594.8 | 1659.0 |
| PSN Mapping (S0=payload) | 975.2 | 976.9 | 977.6 | 988.4 | 1021.2 | 1163.8 | 1165.0 |
| PSN Mapping (adaptive S) | 979.8 | 975.9 | 978.6 | 980.4 | 1014.1 | 1155.3 | 1185.1 |
| index-only ($\Phi$) | 193.2 | 193.1 | 193.3 | 193.0 | 193.9 | 193.2 | 193.4 |

## reorder 增量（reorder=1 − reorder=0，p50 ns）

### sr_16 @ N=5120

| method | reorder=0 p50 | reorder=1 p50 | Δ (ns) |
|---|---|---|---|
| FIFO Queue | 43930.9 | 43980.1 | +49.2 |
| Chained Hash | 349.2 | 449.1 | +99.9 |
| Balanced Tree | 830.0 | 928.1 | +98.1 |
| PSN Mapping (S0=payload) | 323.4 | 428.9 | +105.5 |
| PSN Mapping (adaptive S) | 321.9 | 424.9 | +103.0 |
| index-only ($\Phi$) | 48.1 | 157.8 | +109.7 |

### sr_64 @ N=5120

| method | reorder=0 p50 | reorder=1 p50 | Δ (ns) |
|---|---|---|---|
| FIFO Queue | 175430.5 | 176352.6 | +922.1 |
| Chained Hash | 1575.4 | 2365.3 | +789.9 |
| Balanced Tree | 3331.7 | 3746.9 | +415.2 |
| PSN Mapping (S0=payload) | 1652.8 | 2347.1 | +694.3 |
| PSN Mapping (adaptive S) | 1683.7 | 2375.1 | +691.3 |
| index-only ($\Phi$) | 191.9 | 911.5 | +719.6 |

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

横轴 packet size（5 档 MTU）、纵轴 space utilization（%，线性 0-100）；utilization = payload_bytes / allocated_bytes（满窗 N=4096，cache_footprint_bytes sizeof 实测）。结论：PSN Mapping（弹性）利用率非最高，但与最优基线 FIFO 的差距 <=5%（断言成立，见「关键结论」表），且优于 Chained Hash / Balanced Tree；而 PSN(fixed S=4096) 在 256B 档崩到个位数——证明「弹性内存槽大小机制」解决了固定内存块的空间利用率塌陷。

> N=4096 满窗；utilization = payload_bytes / allocated_bytes（cache_footprint_bytes，sizeof 实测）。


## 空间利用率 (%) — 5 方法 × 5 档 MTU

| method | 256 B | 512 B | 1024 B | 2048 B | 4096 B |
|---|---|---|---|---|---|
| FIFO Queue | 88.89 | 94.12 | 96.97 | 98.46 | 99.22 |
| Chained Hash | 78.05 | 87.67 | 93.43 | 96.60 | 98.27 |
| Balanced Tree | 80.00 | 88.89 | 94.12 | 96.97 | 98.46 |
| PSN Mapping (adaptive S) | 84.43 | 91.56 | 95.59 | 97.75 | 98.86 |
| PSN Mapping (fixed S=4096) | 6.18 | 12.36 | 24.72 | 49.43 | 98.86 |

## 分配总量 allocated_bytes (B) — 满窗 N=4096

| method | 256 B | 512 B | 1024 B | 2048 B | 4096 B |
|---|---|---|---|---|---|
| FIFO Queue | 1179648 | 2228224 | 4325376 | 8519680 | 16908288 |
| Chained Hash | 1343488 | 2392064 | 4489216 | 8683520 | 17072128 |
| Balanced Tree | 1310720 | 2359296 | 4456448 | 8650752 | 17039360 |
| PSN Mapping (adaptive S) | 1241904 | 2290480 | 4387632 | 8581936 | 16970544 |
| PSN Mapping (fixed S=4096) | 16970544 | 16970544 | 16970544 | 16970544 | 16970544 |

## block_S（dynblock 槽大小；非 dynblock=0）

- PSN Mapping (adaptive S): 256 B→S=256, 512 B→S=512, 1024 B→S=1024, 2048 B→S=2048, 4096 B→S=4096
- PSN Mapping (fixed S=4096): 256 B→S=4096, 512 B→S=4096, 1024 B→S=4096, 2048 B→S=4096, 4096 B→S=4096

## 关键结论（脚本计算，供正文）

- FIFO（最优基线）利用率区间：88.89%–99.22%
- 256 B：FIFO−PSN(弹性)=4.46 pp，FIFO−PSN(fixed)=82.71 pp
- 512 B：FIFO−PSN(弹性)=2.56 pp，FIFO−PSN(fixed)=81.76 pp
- 1024 B：FIFO−PSN(弹性)=1.38 pp，FIFO−PSN(fixed)=72.25 pp
- 2048 B：FIFO−PSN(弹性)=0.71 pp，FIFO−PSN(fixed)=49.03 pp
- 4096 B：FIFO−PSN(弹性)=0.36 pp，FIFO−PSN(fixed)=0.36 pp
- 弹性最大劣势点：256 B，差 4.46 pp（<=5% 断言成立）
- fixed 最大塌陷点：256 B，利用率 6.18%（vs FIFO 88.89%）——弹性槽大小机制的价值所在

---

## exp3 — elastic ablation（弹性消融 / 稳定性）

**图：** `paper_figures/fig_exp3_adapt.pdf`（同目录同名 `.png` @300dpi）。

横轴 epoch、纵轴 block size S（B，对数 base2）；ablation（去滞后，实线○）vs slow 对照（默认滞后，虚线□），背景底纹标 256B/4096B 相位（5 档 MTU 跳变 + 256↔4096 ×16 振荡，两条件同一相位序列，仅弹性旋钮不同）。结论：去滞后消融（k_dwell=1/ovf_thresh=1/j_quiet=1/hist_decay=2）对 256↔4096 快速振荡全跟踪、零丢包零 miss（代价是 resize 次数更多）；默认滞后缩得太晚 → gen_switch 延后 → 溢出打满 → 每次振荡扩丢包（见「稳定性（丢包/命中）」与「振荡段跟踪」表）。resize_ns 语义见 numbers.md 内注。

> 两条件同一相位序列（5 档 MTU 跳变 + 256↔4096 ×16 振荡），仅弹性旋钮不同。


## 弹性旋钮

| 旋钮 | ablation | slow 对照 |
|---|---|---|
| k_dwell（同向纪元数） | 1 | 2 |
| ovf_thresh（阈值扩条数） | 1 | 256 |
| j_quiet（缩前安静纪元数） | 1 | 4 |
| hist_decay（溢出史清空纪元数） | 2 | 8 |

## 五档跳变响应延迟（MTU 跳变后 S 首次命中目标的纪元数）

| 跳变 | ablation | slow 对照 |
|---|---|---|
| 4096 B → 256 B | 0 | 2 |
| 256 B → 512 B | 0 | 0 |
| 512 B → 1024 B | 0 | 0 |
| 1024 B → 2048 B | 0 | 0 |
| 2048 B → 4096 B | 0 | 0 |

## 振荡段（256↔4096 ×16，32 相位）跟踪

| 方向 | 相位数 | ablation 收敛 | slow 收敛 |
|---|---|---|---|
| grow（256→4096） | 16 | 16/16（延迟 0 纪元） | 16/16（延迟 0 纪元） |
| shrink（4096→256） | 16 | 16/16（延迟 0 纪元） | 6/16（未收敛 10） |
| 相位末跟踪率（S==MTU） | 32 | 100.0% | 68.8% |

## resize 计数 / 成本

| 指标 | ablation | slow 对照 |
|---|---|---|
| n_resize（总） | 37 | 17 |
| grow（overflow_grow/grow） | 20 | 10 |
| shrink | 17 | 7 |
| resize_ns（单次切换） | mean 2241 / max 18161 | mean 139906 / max 434235 |
| drain_ns（单次排空） | mean 363212 / max 888176 | mean 365633 / max 854752 |

> resize_ns 语义：gen_switch 临界区耗时（pool_alloc/pool_free 的 mmap/munmap + 指针切换）。
> 慢对照的均值被 6 次振荡扩的同步 munmap 拉高：其 gen_switch 被延后到 old_live 恰好归零的当次 store，旧 16.9 MB 池（4096 个已提交页）的 munmap 落在计时区内（≈0.4 ms）；
> ablation 因 j_quiet=1/k_dwell=1 提前一个纪元排空，旧池经 release_old_if_drained 在普通 store 里释放（不计时），故 resize_ns 只剩 mmap（≈1 µs）。两者最终都 munmap 同一池，resize_ns 衡量的只是「临界区内的峰值延迟」，不是累计 mmap/munmap 总量。


## 稳定性（丢包 / 命中）

| 指标 | ablation | slow 对照 |
|---|---|---|
| n_drop（溢出满+延后切换丢弃） | 0 | 19656 |
| n_ovf_ins（溢出插入数） | 16400 | 8200 |
| hit / miss（NAK 重传） | 8759 / 0 | 8366 / 393 |
| hit_rate | 1.0000 | 0.9551 |

---

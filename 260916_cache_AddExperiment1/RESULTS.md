# RESULTS — 实验结果（数字由脚本从各 CSV 提取）

> 生成：各实验 `plot_expX_*.py` 写 `*_numbers.md`，再由 `paper_figures/assemble_results.py` 拼成本文；表内数字禁止手抄。

---

## exp1a — store time cost（存时间开销）

**图：** `paper_figures/fig_exp1a_store.pdf`（同目录同名 `.png` @300dpi）。

横轴 packet size（256/512/1024/2048/4096 B，RDMA 5 档 MTU）、纵轴 store time cost（ns），双对数。结论：PSN Mapping 在小包（256B）存时间略慢于 FIFO/Chained Hash，在大包（4096B）反超成为最快——验证「PSN 映射小包开销高、大包持平/更快」的假设（自适应 S 收敛到各档，见「收敛后 S」）。图内仅 4 方法；index_only 与 PSN(fixed S=4096) 只入下表（正文用数字给自适应收益）。

## B=8192 主矩阵 · p50 (ns) — 主指标

| method | 256 B | 512 B | 1024 B | 2048 B | 4096 B |
|---|---|---|---|---|---|
| FIFO Queue | 10.450 | 16.457 | 39.299 | 132.754 | 464.994 |
| Chained Hash | 11.952 | 18.447 | 45.855 | 138.175 | 473.735 |
| Balanced Tree | 90.403 | 92.637 | 102.453 | 227.504 | 554.909 |
| PSN Mapping (adaptive S) | 14.223 | 20.950 | 33.671 | 122.926 | 435.926 |
| PSN Mapping (fixed S=4096) | 19.765 | 32.865 | 90.269 | 247.758 | 432.752 |
| index-only ($\Phi$) | 1.917 | 1.941 | 2.259 | 2.283 | 2.051 |

## B=8192 主矩阵 · p90 (ns) — 次指标

| method | 256 B | 512 B | 1024 B | 2048 B | 4096 B |
|---|---|---|---|---|---|
| FIFO Queue | 15.932 | 22.488 | 59.638 | 175.935 | 523.448 |
| Chained Hash | 17.104 | 23.465 | 72.469 | 180.538 | 524.937 |
| Balanced Tree | 104.431 | 105.542 | 126.296 | 288.680 | 601.826 |
| PSN Mapping (adaptive S) | 19.949 | 27.445 | 41.264 | 162.274 | 484.601 |
| PSN Mapping (fixed S=4096) | 27.164 | 41.936 | 131.301 | 281.465 | 475.896 |
| index-only ($\Phi$) | 2.198 | 2.173 | 2.442 | 2.454 | 2.307 |

## B=1024 交叉验证 · p50 (ns) — 排序/差距一致性

| method | 256 B | 512 B | 1024 B | 2048 B | 4096 B |
|---|---|---|---|---|---|
| FIFO Queue | 12.697 | 19.827 | 33.597 | 124.233 | 469.389 |
| Chained Hash | 14.943 | 22.268 | 36.821 | 150.603 | 492.536 |
| Balanced Tree | 88.389 | 93.077 | 102.746 | 233.229 | 580.730 |
| PSN Mapping (adaptive S) | 16.994 | 25.003 | 37.895 | 136.246 | 441.652 |
| PSN Mapping (fixed S=4096) | 23.831 | 31.351 | 83.310 | 250.224 | 447.707 |
| index-only ($\Phi$) | 6.056 | 5.762 | 5.762 | 5.372 | 6.153 |

## 收敛后 S（dynblock）

- PSN Mapping (adaptive S): 256 B→S=256, 512 B→S=512, 1024 B→S=1024, 2048 B→S=2048, 4096 B→S=4096
- PSN Mapping (fixed S=4096): 256 B→S=4096, 512 B→S=4096, 1024 B→S=4096, 2048 B→S=4096, 4096 B→S=4096

---

## exp1b — lookup time cost（取时间开销）

**图：** `paper_figures/fig_exp1b_lookup_gbn64.pdf`（同目录同名 `.png` @300dpi）。

横轴 Cache depth N、纵轴 lookup time cost（ns），双对数；主指标 p50（mean 受 VM 停顿尾污染，入表作上界）。回答两个疑问：① reorder 成本对所有方法对称（见「reorder 增量」表：sr_64 排序 ~1050ns、sr_16 ~150ns，与缓存结构无关，非 PSN 落后根因）；② PSN 落后 hash 的根因是块大小 S 与 payload 不匹配（旧 S=4096 存 1024B 包），修正为 S=payload（S=1024，stride 与 hash 节点相同）后 PSN 反超 hash（gbn_long64/short8/sr_16 领先，仅 sr_64 差 ~7%）。SR 图另见 `fig_exp1b_lookup_sr64.pdf`（reorder=0 实线 / reorder=1 虚线）。

> payload=1024，B=512，reps=5；主指标 p50_ns（mean 受 VM ~1% 停顿污染，入表作上界参考）。

> 读钟地板：B=512 → 7.228 ns/op；B=32 → 115.636 ns/op（floor ∝ 1/B 闭环）。


## p50 (ns) — gbn_long64，reorder=0（GBN 天然有序）

| method | 512 | 1024 | 2048 | 4096 | 8192 | 10240 |
|---|---|---|---|---|---|---|
| FIFO Queue | 23523.9 | 50899.5 | 95488.1 | 195959.6 | 454111.2 | 561597.7 |
| Chained Hash | 1508.0 | 1551.9 | 1642.6 | 1933.2 | 2204.5 | 2305.5 |
| Balanced Tree | 1722.7 | 1854.7 | 2146.1 | 2466.5 | 3093.7 | 3249.6 |
| PSN Mapping (S0=payload) | 1473.4 | 1508.0 | 1627.9 | 1814.5 | 2152.4 | 2279.6 |
| PSN Mapping (adaptive S) | 1505.4 | 1535.5 | 1566.0 | 1770.3 | 1936.9 | 2062.1 |
| index-only ($\Phi$) | 251.8 | 225.0 | 238.5 | 261.7 | 270.3 | 276.2 |

## p50 (ns) — sr_64，reorder=0（仅取包）

| method | 512 | 1024 | 2048 | 4096 | 8192 | 10240 |
|---|---|---|---|---|---|---|
| FIFO Queue | 24106.2 | 51625.7 | 100401.4 | 209905.7 | 421536.6 | 638947.1 |
| Chained Hash | 1578.9 | 1704.9 | 1910.8 | 2406.3 | 2760.1 | 3116.0 |
| Balanced Tree | 2675.3 | 3181.6 | 3886.4 | 4969.9 | 6952.2 | 8229.8 |
| PSN Mapping (S0=payload) | 1602.5 | 1737.3 | 1886.2 | 2324.3 | 2768.5 | 3827.0 |
| PSN Mapping (adaptive S) | 1907.1 | 1648.0 | 1962.9 | 2377.8 | 2803.6 | 3326.4 |
| index-only ($\Phi$) | 265.5 | 285.0 | 248.1 | 290.3 | 290.5 | 251.6 |

## p50 (ns) — sr_64，reorder=1（取包前升序整理）

| method | 512 | 1024 | 2048 | 4096 | 8192 | 10240 |
|---|---|---|---|---|---|---|
| FIFO Queue | 25776.3 | 52666.3 | 102524.5 | 209698.7 | 417457.6 | 635433.2 |
| Chained Hash | 2713.4 | 2790.7 | 3382.8 | 3499.0 | 3912.7 | 4264.3 |
| Balanced Tree | 3648.3 | 4169.6 | 4772.2 | 5706.1 | 7453.6 | 8547.4 |
| PSN Mapping (S0=payload) | 2644.0 | 2824.5 | 3022.6 | 3442.8 | 4000.6 | 4932.6 |
| PSN Mapping (adaptive S) | 2814.2 | 2789.2 | 3238.5 | 3565.6 | 3938.1 | 4401.3 |
| index-only ($\Phi$) | 1301.5 | 1222.8 | 1381.2 | 1432.4 | 1348.2 | 1283.3 |

## mean (ns) — gbn_long64，reorder=0（上界参考，受 VM 停顿尾污染）

| method | 512 | 1024 | 2048 | 4096 | 8192 | 10240 |
|---|---|---|---|---|---|---|
| FIFO Queue | 23900.3 | 51199.3 | 95921.5 | 206885.1 | 455359.1 | 566491.9 |
| Chained Hash | 1564.4 | 1610.0 | 1729.3 | 1942.7 | 2267.8 | 2359.3 |
| Balanced Tree | 1785.4 | 1911.2 | 2171.5 | 2503.3 | 3223.6 | 3355.1 |
| PSN Mapping (S0=payload) | 1496.7 | 1537.6 | 1656.7 | 1935.9 | 2929.7 | 3246.4 |
| PSN Mapping (adaptive S) | 1564.7 | 2163.3 | 1585.3 | 1838.9 | 1971.7 | 2181.6 |
| index-only ($\Phi$) | 293.2 | 383.0 | 259.3 | 287.0 | 288.6 | 276.1 |

## reorder 增量（reorder=1 − reorder=0，p50 ns）

### sr_16 @ N=10240

| method | reorder=0 p50 | reorder=1 p50 | Δ (ns) |
|---|---|---|---|
| FIFO Queue | 141811.5 | 140172.3 | -1639.2 |
| Chained Hash | 775.3 | 881.2 | +105.9 |
| Balanced Tree | 1805.5 | 1936.0 | +130.5 |
| PSN Mapping (S0=payload) | 766.5 | 922.8 | +156.3 |
| PSN Mapping (adaptive S) | 662.6 | 838.0 | +175.4 |
| index-only ($\Phi$) | 67.4 | 214.7 | +147.3 |

### sr_64 @ N=10240

| method | reorder=0 p50 | reorder=1 p50 | Δ (ns) |
|---|---|---|---|
| FIFO Queue | 638947.1 | 635433.2 | -3513.9 |
| Chained Hash | 3116.0 | 4264.3 | +1148.4 |
| Balanced Tree | 8229.8 | 8547.4 | +317.6 |
| PSN Mapping (S0=payload) | 3827.0 | 4932.6 | +1105.6 |
| PSN Mapping (adaptive S) | 3326.4 | 4401.3 | +1074.9 |
| index-only ($\Phi$) | 251.6 | 1283.3 | +1031.8 |

## n_cmp（mean_cmp_per_pkt，纯取包比较）— gbn_long64 代表

| method | 512 | 1024 | 2048 | 4096 | 8192 | 10240 | 复杂度 |
|---|---|---|---|---|---|---|---|
| FIFO Queue | 255.5 | 510.8 | 1014.4 | 2043.5 | 4089.0 | 5123.4 | O(N)≈N/2 |
| Chained Hash | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 | O(1) |
| Balanced Tree | 8.0 | 9.0 | 10.0 | 11.0 | 12.0 | 12.4 | O(log N) |
| PSN Mapping (S0=payload) | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | O(1) Φ |
| PSN Mapping (adaptive S) | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | O(1) Φ |
| index-only ($\Phi$) | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | O(1) Φ |

---

## exp2 — space utilization（空间利用率）

**图：** `paper_figures/fig_exp2_space.pdf`（同目录同名 `.png` @300dpi）。

横轴 packet size（5 档 MTU）、纵轴 space utilization（%，线性 0-100）；utilization = payload_bytes / allocated_bytes（满窗 N=10240，cache_footprint_bytes sizeof 实测）。结论：PSN Mapping（弹性）利用率非最高但与最优基线 FIFO 相差 <=5%（256B 档差 4.46pp，见下「关键结论」），且优于 Chained Hash / Balanced Tree；而 PSN(fixed S=4096) 在 256B 档崩到 ~6%——证明「弹性内存槽大小机制」解决了固定内存块空间利用率低下的问题。

> N=10240 满窗；utilization = payload_bytes / allocated_bytes（cache_footprint_bytes，sizeof 实测）。


## 空间利用率 (%) — 5 方法 × 5 档 MTU

| method | 256 B | 512 B | 1024 B | 2048 B | 4096 B |
|---|---|---|---|---|---|
| FIFO Queue | 88.89 | 94.12 | 96.97 | 98.46 | 99.22 |
| Chained Hash | 82.90 | 90.65 | 95.10 | 97.49 | 98.73 |
| Balanced Tree | 80.00 | 88.89 | 94.12 | 96.97 | 98.46 |
| PSN Mapping (adaptive S) | 84.43 | 91.56 | 95.59 | 97.75 | 98.86 |
| PSN Mapping (fixed S=4096) | 6.18 | 12.36 | 24.72 | 49.43 | 98.86 |

## 分配总量 allocated_bytes (B) — 满窗 N=10240

| method | 256 B | 512 B | 1024 B | 2048 B | 4096 B |
|---|---|---|---|---|---|
| FIFO Queue | 2949120 | 5570560 | 10813440 | 21299200 | 42270720 |
| Chained Hash | 3162112 | 5783552 | 11026432 | 21512192 | 42483712 |
| Balanced Tree | 3276800 | 5898240 | 11141120 | 21626880 | 42598400 |
| PSN Mapping (adaptive S) | 3104768 | 5726208 | 10969088 | 21454848 | 42426368 |
| PSN Mapping (fixed S=4096) | 42426368 | 42426368 | 42426368 | 42426368 | 42426368 |

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

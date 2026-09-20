# RESULTS — 实验结果（数字由脚本从各 CSV 提取）

> 生成：各实验 `plot_expX_*.py` 写 `*_numbers.md`，再由 `paper_figures/assemble_results.py` 拼成本文；表内数字禁止手抄。
> 环境：实机（见 docs/ENV_CHECK.md）。

---

## exp1a — store time cost（存时间开销）

**图：** `paper_figures/fig_exp1a_store.pdf`（同目录同名 `.png` @300dpi）。

横轴 packet size（256/512/1024/2048/4096 B，RDMA 5 档 MTU）、纵轴 store time cost（ns），双对数；N=4096，主指标 p50、次指标 p90。左图 4 方法（FIFO Queue / Chained Hash / Balanced Tree / PSN Mapping）；右图 PSN Mapping（adaptive S）vs PSN fixed block（S=4096），逐档标注 fixed 相对 elastic 的 p50 百分比差。结论：PSN Mapping 存时间与 FIFO/Chained Hash 同阶（O(1)，见 p50 主表），全程远优于 Balanced Tree（O(log N)）——验证「PSN 映射与线性结构同阶、远优于树」；右图证明弹性槽大小使 PSN 在小包档快于固定块（百分比差见右图表），至 4096 B 两者 S 相同、差收敛到 0。

## B=2048 主矩阵 · p50 (ns) — 主指标

| method | 256 B | 512 B | 1024 B | 2048 B | 4096 B |
|---|---|---|---|---|---|
| FIFO Queue | 9.465 | 12.479 | 23.191 | 43.744 | 89.068 |
| Chained Hash | 8.807 | 11.967 | 23.198 | 42.379 | 89.436 |
| Balanced Tree | 45.853 | 49.763 | 53.512 | 68.043 | 112.217 |
| PSN Mapping (adaptive S) | 8.050 | 11.609 | 22.851 | 41.860 | 88.625 |
| PSN fixed block (S=4096) | 10.505 | 16.444 | 27.489 | 48.924 | 88.413 |
| index-only ($\Phi$) | 1.099 | 1.099 | 1.099 | 1.099 | 1.099 |

## B=2048 主矩阵 · p90 (ns) — 次指标

| method | 256 B | 512 B | 1024 B | 2048 B | 4096 B |
|---|---|---|---|---|---|
| FIFO Queue | 9.590 | 12.651 | 23.629 | 44.432 | 100.559 |
| Chained Hash | 9.002 | 12.244 | 23.379 | 43.248 | 102.580 |
| Balanced Tree | 46.300 | 50.222 | 54.152 | 68.983 | 125.311 |
| PSN Mapping (adaptive S) | 8.219 | 11.813 | 23.224 | 42.570 | 95.855 |
| PSN fixed block (S=4096) | 10.860 | 16.903 | 32.617 | 52.577 | 98.180 |
| index-only ($\Phi$) | 1.103 | 1.104 | 1.103 | 1.103 | 1.104 |

## B=1024 交叉验证 · p50 (ns) — 排序/差距一致性

| method | 256 B | 512 B | 1024 B | 2048 B | 4096 B |
|---|---|---|---|---|---|
| FIFO Queue | 7.172 | 12.592 | 22.323 | 42.221 | 87.786 |
| Chained Hash | 9.177 | 12.042 | 23.263 | 43.653 | 89.139 |
| Balanced Tree | 45.934 | 49.599 | 53.359 | 67.580 | 113.143 |
| PSN Mapping (adaptive S) | 7.604 | 11.361 | 22.875 | 41.780 | 88.804 |
| PSN fixed block (S=4096) | 11.865 | 17.421 | 27.777 | 49.063 | 88.629 |
| index-only ($\Phi$) | 1.160 | 1.160 | 1.160 | 1.160 | 1.161 |

## 右图：PSN 弹性 vs ring_fixed（B=2048 p50，fixed 相对 elastic 的百分比差）

| payload | 256 B | 512 B | 1024 B | 2048 B | 4096 B |
|---|---|---|---|---|---|
| elastic p50 (ns) | 8.050 | 11.609 | 22.851 | 41.860 | 88.625 |
| fixed p50 (ns) | 10.505 | 16.444 | 27.489 | 48.924 | 88.413 |
| fixed vs elastic (%) | +30.5 | +41.6 | +20.3 | +16.9 | -0.2 |

## 收敛后 S（dynblock）

- PSN Mapping (adaptive S): 256 B→S=256, 512 B→S=512, 1024 B→S=1024, 2048 B→S=2048, 4096 B→S=4096
- PSN fixed block (S=4096): 256 B→S=4096, 512 B→S=4096, 1024 B→S=4096, 2048 B→S=4096, 4096 B→S=4096

---

## exp1a-alloc — 分配策略敏感性（第 5 条正交矩阵：pooled vs perstore）

**图：** `paper_figures/fig_exp1a_alloc_sensitivity.pdf`（同目录同名 `.png` @300dpi）。

横轴 packet size、纵轴 store time cost（ns），双对数；4 结构各两条线（pooled 实线 / perstore 虚线）。唯一变量 = 分配策略：pooled 一次性预分配+复用（n_malloc=0）、perstore 逐 store malloc/free （n_malloc=n_free=实际 store 次数）；索引结构 / 淘汰 / 计时口径相同（alloc_equiv_test 验证字节一致）。结论：perstore 相对 pooled 的固定开销 ≈ 裸 malloc/free 成本，随 payload 增大被 memcpy 摊薄（见下表 Δ/Δ% 与关键结论）。

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

---

## exp1b — lookup time cost（取时间开销）

**图：** `paper_figures/fig_exp1b_lookup_gbn64.pdf / fig_exp1b_lookup_sr64.pdf`（同目录同名 `.png` @300dpi）。

横轴 Cache depth N（128/256/512/1024/2048/4096/5120）、纵轴 lookup time cost（ns），双对数；payload=1024、B=512、reps=5，主指标 p50。三条 PSN 变体（S0=payload / adaptive S / ring_fixed 固定 S=4096）。第 4B 统一交付契约：retrieve_set 按 PSN 升序交付（fifo/hash 显式 sort_u32_asc、tree 排序+查找、dynblock 扫槽）。结论见下方关键结论（脚本计算）：① GBN 下 FIFO O(N)、PSN/index_only n_cmp=0 与 Φ 同阶、持平/反超 Hash；② SR 下 PSN 位图快路径零排序交付、优于 fifo/hash 的 qsort 与 tree 的中序；③ SR-64 伸缩性门未过——诊断为 64KB memcpy 缓存局部性（index_only 扁平佐证）而非算法退化，位图快路径已把旧版（位图快路径前）增长压降一个数量级以上，保留现状并如实报告（见关键结论第 5 条）。

> payload=1024，B=512，reps=5；主指标 p50_ns（实机尾部干净，p50 稳健）。

> 第 4B：retrieve_set 统一按 PSN 升序交付（fifo/hash sort_u32_asc、tree 排序+查找、dynblock 扫槽）。

> 排序原语 sort_impl：0=qsort(主) / 1=插入排序(xval)；表内取主(sort_impl=0)。

> 读钟地板：B=512 → 0.293 ns/op；B=32 → 0.375 ns/op（floor ∝ 1/B 闭环）。


## p50 (ns) — gbn_long64（retrieve_range，天然升序；sort_impl=0）

| method | 128 | 256 | 512 | 1024 | 2048 | 4096 | 5120 |
|---|---|---|---|---|---|---|---|
| FIFO Queue | 4451.9 | 8228.7 | 16124.7 | 32828.7 | 64971.9 | 130042.1 | 162798.6 |
| Chained Hash | 1003.4 | 1015.8 | 1019.5 | 1031.5 | 1107.5 | 1333.0 | 1368.5 |
| Balanced Tree | 1067.8 | 1087.6 | 1133.6 | 1179.8 | 1303.3 | 1559.8 | 1625.1 |
| PSN Mapping (S0=payload) | 986.5 | 990.4 | 993.0 | 993.7 | 1045.7 | 1224.4 | 1262.8 |
| PSN Mapping (adaptive S) | 986.6 | 990.4 | 993.3 | 995.3 | 1035.1 | 1243.2 | 1291.7 |
| PSN fixed block (S=4096) | 1042.1 | 1040.2 | 1135.5 | 1485.9 | 1791.6 | 1931.3 | 1988.8 |
| index-only ($\Phi$) | 181.5 | 181.5 | 181.2 | 181.3 | 181.5 | 181.3 | 181.5 |

## p50 (ns) — sr_64（retrieve_set，第 4B 升序交付；sort_impl=0）

| method | 128 | 256 | 512 | 1024 | 2048 | 4096 | 5120 |
|---|---|---|---|---|---|---|---|
| FIFO Queue | 6197.3 | 10192.5 | 18365.7 | 37085.6 | 72342.2 | 141122.6 | 175888.9 |
| Chained Hash | 2575.3 | 2587.9 | 2614.8 | 2666.4 | 2804.1 | 3149.8 | 3083.6 |
| Balanced Tree | 2833.8 | 2943.4 | 3220.0 | 3419.4 | 3752.6 | 4321.9 | 4626.6 |
| PSN Mapping (S0=payload) | 864.5 | 959.7 | 1033.9 | 1080.9 | 1220.5 | 1558.8 | 1512.1 |
| PSN Mapping (adaptive S) | 864.2 | 960.7 | 1034.7 | 1082.3 | 1215.1 | 1579.6 | 1500.6 |
| PSN fixed block (S=4096) | 852.2 | 991.6 | 1127.9 | 1363.5 | 1812.2 | 1992.4 | 1870.7 |
| index-only ($\Phi$) | 1754.9 | 1749.9 | 1751.0 | 1751.7 | 1753.1 | 1758.2 | 1778.5 |

## mean (ns) — gbn_long64（上界参考；sort_impl=0）

| method | 128 | 256 | 512 | 1024 | 2048 | 4096 | 5120 |
|---|---|---|---|---|---|---|---|
| FIFO Queue | 4446.7 | 8252.8 | 16173.8 | 33444.7 | 65099.9 | 133270.8 | 162924.0 |
| Chained Hash | 1004.5 | 1016.9 | 1020.1 | 1032.1 | 1110.3 | 1350.1 | 1325.1 |
| Balanced Tree | 1068.2 | 1087.8 | 1134.1 | 1180.5 | 1305.3 | 1571.9 | 1620.8 |
| PSN Mapping (S0=payload) | 987.7 | 992.4 | 996.0 | 998.9 | 1047.5 | 1227.2 | 1230.9 |
| PSN Mapping (adaptive S) | 987.0 | 993.2 | 995.3 | 1005.2 | 1034.7 | 1229.7 | 1270.6 |
| PSN fixed block (S=4096) | 1042.3 | 1041.2 | 1123.9 | 1408.7 | 1657.8 | 1954.5 | 1889.1 |
| index-only ($\Phi$) | 182.5 | 181.8 | 181.6 | 181.6 | 182.2 | 181.8 | 183.0 |

## n_cmp（mean_cmp_per_pkt，纯取包比较）— gbn_long64 代表（sort_impl=0）

| method | 128 | 256 | 512 | 1024 | 2048 | 4096 | 5120 | 复杂度 |
|---|---|---|---|---|---|---|---|---|
| FIFO Queue | 64.7 | 128.2 | 255.5 | 510.8 | 1014.4 | 2043.5 | 2556.5 | O(N)≈N/2 |
| Chained Hash | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 | O(1) |
| Balanced Tree | 6.0 | 7.0 | 8.0 | 9.0 | 10.0 | 11.0 | 11.4 | O(log N) |
| PSN Mapping (S0=payload) | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | O(1) Φ |
| PSN Mapping (adaptive S) | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | O(1) Φ |
| PSN fixed block (S=4096) | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | O(1) Φ |
| index-only ($\Phi$) | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | O(1) Φ |
## 关键结论（脚本计算，禁手抄）
1. 伸缩性（gbn_long64）：FIFO p50 从 N=128 的 4452 ns 涨到 N=5120 的 162799 ns（×37），n_cmp≈N/2（N=5120 时 2556.5）；Hash n_cmp=1.0 恒定、PSN/index_only n_cmp=0（Φ 纯算术）——O(N) vs O(1) 直接证据。
2. PSN(adaptive S) vs Chained Hash @ N=4096（p50 ns；PSN 快为正 %）：
   GBN-long64：PSN 1243.2 vs hash 1333.0（PSN +6.7%）
   GBN-short8：PSN 107.1 vs hash 124.9（PSN +14.2%）
   SR-16：PSN 352.7 vs hash 567.7（PSN +37.9%）
   SR-64：PSN 1579.6 vs hash 3149.8（PSN +49.9%）
3. SR 升序交付（第 4B，sr_64 @ N=5120，retrieve_set p50 ns；fifo/hash/tree 需排序检索、dynblock 零排序）：
   FIFO Queue = 175888.9
   Chained Hash = 3083.6
   Balanced Tree = 4626.6
   PSN Mapping (S0=payload) = 1512.1
   PSN Mapping (adaptive S) = 1500.6
   PSN fixed block (S=4096) = 1870.7
   index-only ($\Phi$) = 1778.5
4. S0=payload 与 adaptive 收敛两条路结果一致（最终 S=1024）：gbn_long64 @ N=4096 二者 p50 1224.4 / 1243.2 ns。
5. SR-64 伸缩性门（门槛 +<20%）：PSN(adaptive) sr_64 p50 N=128→5120 = 864→1501 ns（+74%）。
   对照：index_only（Φ 纯算术，无 memcpy）1755→1778（+1%，扁平）；gbn_long64（连续 64KB memcpy）987→1292（+31%）。
   诊断：增长来自 64KB memcpy 的缓存局部性（源环 128KB→5MB 跨 L2→L3），SR-64 再叠加离散访问惩罚；
   位图扫描 O(span/64) 可忽略（index_only 扁平佐证），非算法退化。旧版（位图快路径前）sr_64 增长 +972%（1389→14886），
   位图快路径把增长压降 13.2×、sr_64@5120 降至 1501 ns（旧版 14886）。此门未过，保留现状并如实报告。
6. 排序原语一致性：sr_64 @ N=5120 PSN(adaptive) 主(qsort) 1500.6 vs xval(插入排序) 1497.6 ns（差 2.99 ns，等价）。

---

## exp2 — space utilization（空间利用率）

**图：** `paper_figures/fig_exp2_space.pdf`（同目录同名 `.png` @300dpi）。

横轴 packet size（5 档 MTU）；左轴 space utilization（%，线性）、右轴 overhead per pkt（B，对数）。utilization = payload_bytes / allocated_bytes（满窗 N=4096，cache_footprint_bytes sizeof 实测）。5 方法：三个基线 + PSN Mapping（adaptive S）+ PSN fixed block（S=4096）。结论见下方关键结论（脚本计算）：PSN（弹性）利用率反超原最优基线 FIFO（5 档全部反超）且贴近理想上界；PSN fixed block（S=4096）在小包档利用率塌陷、overhead 冲高——证明弹性槽大小机制解决了固定块空间利用率塌陷。

> N=4096 满窗；utilization = payload_bytes / allocated_bytes；overhead_per_pkt = (allocated−payload)/N；lower_bound_util = pl/(align16(pl)+12)。


## 空间利用率 (%) — 5 方法 × 5 档 MTU

| method | 256 B | 512 B | 1024 B | 2048 B | 4096 B |
|---|---|---|---|---|---|
| FIFO Queue | 88.89 | 94.12 | 96.97 | 98.46 | 99.22 |
| Chained Hash | 78.05 | 87.67 | 93.43 | 96.60 | 98.27 |
| Balanced Tree | 80.00 | 88.89 | 94.12 | 96.97 | 98.46 |
| PSN Mapping (adaptive S) | 94.40 | 97.12 | 98.54 | 99.26 | 99.63 |
| PSN fixed block (S=4096) | 6.23 | 12.45 | 24.91 | 49.82 | 99.63 |

## 每包空间开销 overhead_per_pkt (B) — 5 方法 × 5 档 MTU

| method | 256 B | 512 B | 1024 B | 2048 B | 4096 B |
|---|---|---|---|---|---|
| FIFO Queue | 32.0 | 32.0 | 32.0 | 32.0 | 32.0 |
| Chained Hash | 72.0 | 72.0 | 72.0 | 72.0 | 72.0 |
| Balanced Tree | 64.0 | 64.0 | 64.0 | 64.0 | 64.0 |
| PSN Mapping (adaptive S) | 15.2 | 15.2 | 15.2 | 15.2 | 15.2 |
| PSN fixed block (S=4096) | 3855.2 | 3599.2 | 3087.2 | 2063.2 | 15.2 |

## 理想上界 lower_bound_util (%)（仅对齐填充 + 12B meta，方法无关）

| lower_bound_util | 95.52 | 97.71 | 98.84 | 99.42 | 99.71 |

## 分配总量 allocated_bytes (B) — 满窗 N=4096

| method | 256 B | 512 B | 1024 B | 2048 B | 4096 B |
|---|---|---|---|---|---|
| FIFO Queue | 1179648 | 2228224 | 4325376 | 8519680 | 16908288 |
| Chained Hash | 1343488 | 2392064 | 4489216 | 8683520 | 17072128 |
| Balanced Tree | 1310720 | 2359296 | 4456448 | 8650752 | 17039360 |
| PSN Mapping (adaptive S) | 1110832 | 2159408 | 4256560 | 8450864 | 16839472 |
| PSN fixed block (S=4096) | 16839472 | 16839472 | 16839472 | 16839472 | 16839472 |

## block_S（dynblock 槽大小；非 dynblock=0）

- PSN Mapping (adaptive S): 256 B→S=256, 512 B→S=512, 1024 B→S=1024, 2048 B→S=2048, 4096 B→S=4096
- PSN fixed block (S=4096): 256 B→S=4096, 512 B→S=4096, 1024 B→S=4096, 2048 B→S=4096, 4096 B→S=4096

## 关键结论（脚本计算，供正文）

- FIFO（原最优基线）利用率区间：88.89%–99.22%
- PSN(弹性) 利用率区间：94.40%–99.63%（**反超 FIFO**）
- 256 B：PSN(弹性)−FIFO=+5.51 pp，FIFO−PSN(fixed)=82.66 pp
- 512 B：PSN(弹性)−FIFO=+3.00 pp，FIFO−PSN(fixed)=81.66 pp
- 1024 B：PSN(弹性)−FIFO=+1.57 pp，FIFO−PSN(fixed)=72.06 pp
- 2048 B：PSN(弹性)−FIFO=+0.80 pp，FIFO−PSN(fixed)=48.65 pp
- 4096 B：PSN(弹性)−FIFO=+0.41 pp，FIFO−PSN(fixed)=-0.41 pp
- 弹性反超最小点：4096 B，PSN 仍高 +0.41 pp（5 档全部反超 FIFO）
- fixed 最大塌陷点：256 B，利用率 6.23%（vs FIFO 88.89%），overhead 3855.2 B/包 —— 弹性槽大小机制的价值所在
- 弹性距理想上界最近：4096 B，PSN 99.63% vs lower_bound 99.71%（差 0.08 pp；仅剩溢出数组等固定开销）

---

## exp2(b) — 尾包占比敏感性（tail sensitivity）

**图：** `paper_figures/fig_exp2_tail.pdf`（同目录同名 `.png` @300dpi）。

横轴尾包占比 f（包尺寸 (1-f)@MTU + f@U[1,MTU]；MTU=4096、N=4096 满窗）、纵轴 space utilization（%，线性）。结论见下方关键结论（脚本计算）：弹性 S 被满 MTU 包钉在 4096、7 档 f 全程零扩缩/零溢出/零丢包，utilization 随 f 单调优雅下降——单全局 S 在尺寸尾下稳定、无结构抖动。

> 包尺寸 (1-f)@MTU + f@U[1,MTU]；MTU=4096、N=4096 满窗；utilization = payload_bytes / allocated_bytes（sizeof 实测）。


| f | final_S | utilization (%) | overhead_per_pkt (B) | n_ovf_ins | n_drop | n_resize |
|---|---|---|---|---|---|---|
| 0.00 | 4096 | 99.63 | 15.2 | 0 | 0 | 0 |
| 0.01 | 4096 | 99.19 | 33.4 | 0 | 0 | 0 |
| 0.03 | 4096 | 98.00 | 82.3 | 0 | 0 | 0 |
| 0.06 | 4096 | 96.59 | 140.4 | 0 | 0 | 0 |
| 0.10 | 4096 | 94.86 | 211.5 | 0 | 0 | 0 |
| 0.20 | 4096 | 89.81 | 419.0 | 0 | 0 | 0 |
| 0.30 | 4096 | 84.17 | 650.6 | 0 | 0 | 0 |

## 关键结论（脚本计算，禁手抄）

- 弹性 S 被 (1-f) 的满 MTU 包钉在 **4096 B**（7 档 f 恒不变），规则 B 不缩（L_ring=4096 恒 ≥ S）。
- 7 档 f 全部 **n_ovf_ins = n_drop = n_resize = 0**：尾包 len≤S 走环内正常路径，单全局 S 在尺寸尾下稳定、优雅降级，无扩缩/溢出/丢包抖动。
- utilization 随 f **单调下降**（脚本判定：是）：99.63%（f=0）→ 84.17%（f=0.30）。
- overhead_per_pkt 随 f 上升：15.2 B（f=0）→ 650.6 B（f=0.30），代价只是利用率线性下降，无结构抖动。

---

## 综合权衡（Pareto）— store vs SR-64 lookup

**图：** `paper_figures/fig_pareto.pdf`（同目录同名 `.png` @300dpi）。

散点（双对数）：x = store p50 @ 1024 B pooled、y = SR-64 retrieve_set p50 @ N=4096；左下角 = 更好。5 点：三个基线 + PSN Mapping + index-only（Φ 纯算术对照）。结论见下方主导关系（脚本计算）：PSN Mapping 在两个维度同时严格优于三个基线（FIFO/Chained Hash/Balanced Tree），是唯一真实方法的 Pareto 最优点；另一个前沿点是 index-only（Φ 地板，无 payload 拷贝，但 SR 交付走排序路径故 lookup 略高于 PSN 位图快路径）。

> x=store p50 @ payload=1024 pooled B=2048（exp1a）；y=SR-64 retrieve_set p50 @ N=4096 sort_impl=0（exp1b）。


| method | store p50 (ns) | SR-64 lookup p50 (ns) |
|---|---|---|
| FIFO Queue | 23.191 | 141122.6 |
| Chained Hash | 23.198 | 3149.8 |
| Balanced Tree | 53.512 | 4321.9 |
| PSN Mapping | 22.851 | 1579.6 |
| index-only ($\Phi$) | 1.099 | 1758.2 |

## 主导关系（脚本计算）
- PSN Mapping 同时优于 FIFO Queue：store 22.851 < 23.191 ns 且 lookup 1579.6 < 141122.6 ns
- PSN Mapping 同时优于 Chained Hash：store 22.851 < 23.198 ns 且 lookup 1579.6 < 3149.8 ns
- PSN Mapping 同时优于 Balanced Tree：store 22.851 < 53.512 ns 且 lookup 1579.6 < 4321.9 ns
- index-only（Φ 地板）：store 1.099 ns（无 payload 拷贝的不可约成本），但 SR 交付仍走排序路径，lookup 1758.2 ns 略高于 PSN 位图快路径 1579.6 ns

---

## exp3 — elastic ablation（弹性消融 / 稳定性）

**图：** `paper_figures/fig_exp3_adapt.pdf`（同目录同名 `.png` @300dpi）。

横轴 epoch、纵轴 block size S（B，对数）；两条件同一相位序列（11 相位 = 1 预热 + 5 升档 + 5 降档，每档稳定 16 纪元再切换），仅弹性旋钮不同（ablation 去滞后 vs slow 默认滞后）。另报收敛时间、逐相位利用率、resize 计数/成本、稳定性四表。结论见下方各表（脚本计算）：每档稳定 16 纪元下两条件都零丢包零 miss（见稳定性表）；区别在收敛速度与 resize 临界区峰值延迟——默认滞后的 gen_switch 延后到 old_live 归零、旧池同步 munmap，故临界区峰值更高（见 resize 计数/成本表）。

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

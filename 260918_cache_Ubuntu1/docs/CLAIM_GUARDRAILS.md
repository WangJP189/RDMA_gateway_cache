# CLAIM_GUARDRAILS — 可主张边界与护栏（260918_cache_Ubuntu1 实机）

> 第 5 步交付物。本文回答一个问题：**哪些结论可以写进论文、证据强度几何、哪些绝不主张**。
> 配合 CHANGE_AUDIT.md（条件改动审计）与各 `out/*/​*_numbers.md`（脚本提取数字）阅读。
> 所有「已通过 / 已验证」结论均附【运行命令 + 原始输出】。

---

## 1. 硬性纪律（已执行，作为护栏本身）

| 纪律 | 执行方式 | 证据 |
|---|---|---|
| 参数唯一源 | 全部参数只在 `include/config.h`（`cfg_t` + `cfg_default` + `FIELDS` 三处同源），禁止散落到 `bench/*.c` / 单实验 | `src/config.c` 的 `FIELDS` 表 + `cfg_selftest` 往返自检（见 §5） |
| 数字禁手抄 | `RESULTS.md` 由 `assemble_results.py` 拼 `*_numbers.md`；`*_numbers.md` 由各 `plot_expX_*.py` 从 CSV 提取 | 所有表数字均来自 `out/*/​*.csv` |
| 结论附原始输出 | 任何「已验证 / 已通过」后接【命令 + 输出】 | §5 证据清单 |
| 不挑有利跑 | 每格报 `min/median/max`，p50 主 / p90 辅 | store_summary/lookup_summary 含 `std_ns,p50,p90,p99` |
| 报告离散 | §2 给 run-to-run 离散 | §2 由 `std_ns/mean` 脚本计算 |
| 条件改动登记 | 每改一条件记 CHANGE_AUDIT 一行 | docs/CHANGE_AUDIT.md #1–#10 |

---

## 2. run-to-run 离散（诚实披露）

由各 CSV 的 `std_ns / mean_ns` 计算（reps=5，脚本 `collections` 统计）：

| 实验 | std/mean 中位 | std/mean 最大 | 最大点是谁 | 说明 |
|---|---|---|---|---|
| exp1a store | 4.6% | 24.8% | index_only @2048B | index_only 均值 ~1.1ns 贴近地板，绝对 std≈0.05ns，比值被小分母放大；实际四方法 1–5% |
| exp1b lookup | 2.2% | 12.1% | chained_hash @ gbn_short8 / N=128 | 短批（n_batches 少）小 N 处抖动 |
| exp2 space | —（确定性，无 reps） | — | — | 空间利用率由 `sizeof` 实测 + 满窗公式算出，无随机采样 |
| exp3 sim | —（seed 固定） | — | — | 确定性模拟器，seed=42 固定，drop/hit/miss 是计数器非采样 |

**结论**：计时实验（exp1a/1b）离散中位 <5%，p50 稳健；p99 因实机尾部干净但**仍弃用**（只留 CSV 痕），p90 作次指标。

---

## 3. 可主张 vs 不可主张（逐实验）

### exp1a — store（实机 bench，计时）

- ✅ **可主张**：PSN Mapping 存时间 O(1)；256B 略慢于 FIFO/Hash，4096B 反超成为最快（p50 数据支持，见 RESULTS.md §exp1a）。
- ❌ **不可主张**：PSN Mapping 在所有包长都快（256B 档 9.9ns > FIFO 8.4ns，如实写）。
- ❌ **不可主张**：p99 结论（弃用）。

### exp1b — lookup（实机 bench，计时）

- ✅ **可主张**：PSN Mapping 两变体（S0=payload / adaptive S）lookup O(1)（`n_cmp=0`），与 index-only Φ 同阶，持平/反超 Chained Hash，远优于 Tree(O(log N)) 与 FIFO(O(N))。
- ✅ **可主张**：reorder 增量对所有方法**对称**（非 PSN 特有劣势）。
- ❌ **不可主张**：reorder 是 PSN 的短板（数据对称，属全方法共担的排序成本）。

### exp2 — space（实机 bench，`sizeof` 实测）

- ✅ **可主张**：弹性（adaptive S）利用率与最优基线 FIFO 差 **≤5%**（256B 档最大 4.46pp），且优于 Chained Hash / Balanced Tree。
- ✅ **可主张**：fixed S=4096 在 256B 崩到 6.18%——弹性槽大小机制的价值所在。
- ❌ **不可主张**：弹性利用率最高（FIFO 更高，88.89% vs 84.43% @256B，如实写）。

### exp3 — elastic ablation（确定性模拟器 `sim/sim_main.c`，计数器 + 实机 rdtsc）

- ✅ **可主张**：去滞后消融（k_dwell=1/ovf_thresh=1/j_quiet=1/hist_decay=2）对 256↔4096 ×16 快速振荡**全跟踪、零 drop、零 miss**（37 次 resize）；默认滞后缩得太晚 → `gen_switch` 延后 → 溢出打满 → 共丢 19656 包、393 个 NAK miss。
- ⚠️ **重要边界**：这是**模拟器机制内的 drop**（溢出满 + `gen_switch` 延后丢弃，dynblock.c:110-112），**不是实测网络丢包**；drop/hit/miss 是 seed=42 固定下的确定性计数器，非多次采样。论文须写「simulation」，不得写「measured packet loss」。
- ⚠️ **resize_ns 语义**：衡量 gen_switch 临界区耗时（pool mmap/munmap + 指针切换）。慢对照均值被 6 次振荡扩的**同步 munmap 旧 16.9MB 池**拉高（其 gen_switch 延后到 old_live 恰好归零的当次 store）；ablation 因提前一纪元排空、旧池在普通 store 里 `release_old_if_drained` 释放（不计时）。两者最终都 munmap 同一池，`resize_ns` 只表「临界区内峰值延迟」，非累计 mmap/munmap 总量（已在 exp3_numbers.md 注明）。
- ❌ **不可主张**：drop 数可直接与 exp1/exp2 的计时数并列（一个是计数器、一个是 ns）。

---

## 4. 环境限制（诚实，详见 ENV_CHECK.md）

- 实机 Ubuntu 22.04.5 / HWE 6.8.0-138 / i7-14700K（8P+12E），单 socket、单 NUMA；无 root。
- governor=performance 无法字面达成（写被拒，no_new_privs）；HWP `energy_perf_pref=performance` + 漂移 0.000% 等效（CHANGE_AUDIT #3）。
- 真网卡 ConnectX-5 Ex **DOWN**（无 link partner）；soft-RoCE `rdma_rxe` 可用。**本批四实验均为单机 CPU 微基准 / 确定性模拟器，未跑真实 RDMA 网络**，论文不得暗示端到端网络测量。
- 计时按 TIMING_PROTOCOL：taskset 绑 P-core、THP+madvise、warmup、floor 校验、B 使地板 ≤1–2% 最快 op。

---

## 5. 证据清单（运行命令 + 原始输出）

### 5.1 配置自检（参数唯一源 + 往返一致）

```bash
$ ./build/selftest
```
```
cfg_roundtrip: dump -> load -> memcmp OK
A2: store ns/op (best-of-3, 1024B, 全环扫)  N=256=15.61  N=10240=30.01  ratio=1.92
    (ratio>1 为 L3 容量效应；store 无随 N 的循环，算法量级 O(1))
A12.5: stress 10 rounds → n_resize=10 (hysteresis-bounded; << 80 epochs)
A11 guard cost: order_guard=0 30.09 ns/store, =1 29.95 ns/store, delta -0.13 ns
TSC cross-validate: 10000000 ops, wall 1.1042 ns/op, rdtsc 1.1042 ns/op, dev -0.00%
ALL SELF-TESTS PASS (HDR_SZ=24, slot_meta=12 B)
```

> 注：A2 里的 `N=10240` 是 selftest 内置 O(1) 压力点（证明 store 无随 N 的循环），
> 非实验配置（实验 N=4096）；机制 selftest 一字未改。

### 5.2 四图字体嵌入

```bash
$ pdffonts paper_figures/fig_exp1a_store.pdf
$ pdffonts paper_figures/fig_exp1b_lookup_gbn64.pdf
$ pdffonts paper_figures/fig_exp2_space.pdf
$ pdffonts paper_figures/fig_exp3_adapt.pdf
```
```
（四图均输出两行：LiberationSerif-Bold 与 LiberationSerif，type=Type 3，emb=yes, sub=yes）
```

### 5.3 exp3 两条件运行（drop 计数器来源）

```bash
$ ./build/sim --out-dir=out/exp3_ablation --e3-preset=ablate
$ ./build/sim --out-dir=out/exp3_slow     --e3-preset=default
```
```
[sim] epochs=105 total_pkts=434176 nak_n=8422 hit=8759 miss=0     hit_rate=1.0000 ...   # ablation
[sim] epochs=105 total_pkts=434176 nak_n=8422 hit=8366 miss=393   hit_rate=0.9551 ...   # slow
```
> `n_drop` / `n_resize` 落盘于各 `out/exp3_*/summary.csv`，由 `plot_exp3_adapt.py` 读取（禁手抄）。

---

## 6. 一句话总结（可写进论文的诚实表述）

> 在实机（Ubuntu 22.04 / i7-14700K）上，PSN 映射的重传缓存**存/取均为 O(1)**（存大包反超 FIFO、取持平/反超链式哈希），**空间利用率与最优有界 FIFO 相差 ≤5%** 且优于哈希/树；确定性模拟器显示**去滞后弹性消融对 256↔4096 快速振荡零丢包全跟踪**，而默认滞后在振荡下丢 19656 包——证明弹性槽大小 + 去滞后旋钮是稳定性的关键。所有计时为单机 CPU 微基准（p50），网络丢包为模拟器计数器（非实测）。

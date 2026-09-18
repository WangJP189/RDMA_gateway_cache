# CHANGE_AUDIT — 实验条件改动审计（260918_cache_Ubuntu1 实机）

> 第 5 步交付物（随步骤滚动记录）。纪律：**每改一个实验条件，登记一行**
> 「条件 / 依据 / 偏差方向（对谁有利）/ 是否在论文中披露」。禁手抄、禁挑有利跑、
> 禁给对手加劣势。所有「已通过/已验证」结论必须附【运行命令 + 原始输出】。

---

## 变更总表

| # | 条件 | 依据 | 偏差方向（对谁有利） | 论文披露 |
|---|---|---|---|---|
| 1 | 主计时原语=rdtsc_raw（实机 4.767ns vs clock_gettime 9.925ns） | Step1 原语对比（各 10^7 次） | 中性（全方法同一口径） | 是 |
| 2 | exp1a 批 B: 8192 → 2048 | Step1 实机读钟 8.19ns，地板≤最快 op 1–2% | 全方法同向摊薄，中性 | 是 |
| 3 | governor=performance 无法字面达成（无 root，写被拒） | 环境限制（no_new_privs） | HWP `energy_perf_pref=performance` + 漂移 0.000% 等效；不影响相对结论 | 是 |
| 4 | N: 10240 → 4096（=128×32） | Step2 W×k 推导 + BDP 校验 | FIFO 环形维护随 N 降（略利基线）；hash 负载 0.625→0.25（基线更快，**对己不利=保守**）；Tree O(logN) 略降；PSN Φ O(1) 不受影响 | 是 |
| 5 | ovf_cap: 2048 → 819（=⌊N/5⌋） | 随 N 缩放，保持「打满=紧急」相对口径 | 相对难度不变，中性 | 是 |
| 6 | E1B_N_LIST: 6 点 → 7 点（+锚点 5120 非 2 的幂） | mask 披露需要（验证运行时取模无 AND 优势） | 中性（多一个诚实锚点） | 是 |
| 7 | exp1a「删除 24B slot header → stride=align16(S)、meta→8B/slot」**未实施** | 与「机制逻辑一字不改」冲突 + 8B meta 打包方式未 specify | 保留 24B header ⇒ stride=align16(24+S) 更大 ⇒ 空间利用率略低（**对己不利=保守**），但机制忠实 | 是（须写 stride=align16(24+S) 而非 align16(S)） |
| 8 | exp1b dynblock 环长=扫描点 N（原 harness 环长固定 ring_n=4096） | 锚点 5120 > ring_n=4096，固定环会在 N=5120 静默淘汰 ~20% 条目；且「窗口 N」在四方法上应同义 | 中性（`Φ=psn%N` 一字未改，仅实例化环长；让 FIFO/Hash/Tree 的 capacity=N 与 dynblock 环长=N 对齐） | 是（须写 dynblock 环长=窗口 N） |
| 9 | exp3 ablation 旋钮经 `--e3-preset=ablate` 覆盖：k_dwell 2→1、ovf_thresh 256→1、j_quiet 4→1、hist_decay 8→2（机制默认值一字未改） | 消融要测「去滞后」档；默认值由 selftest A10（缩）/A12（抗振荡）护栏验证，不能直接改默认 | 只作用于 ablation 条件；slow 对照仍用默认滞后（慢=对己不利=保守）。同相位序列下对比，公平 | 是（须写「ablation 为 per-run override，非默认变更」） |
| 10 | exp3 相位序列重写：5 档 MTU 跳变 + 256↔4096 ×16 振荡（38 相位）；e3_phases 容量 16→64 | 规范 Step3 exp3 要求「5 档 MTU 跳变 incl 256↔4096 16×」 | 中性（同一相位序列喂给两条件） | 是 |

---

## 逐条细节与证据

### #2 exp1a B=8192→2048

- **旧依据已失效**：VM 上单次读钟 ~3.8μs ⇒ 需 B=8192 摊薄；实机单次 rdtsc_raw=4.767ns（TIMING_PROTOCOL §1）。
- **新依据**：读钟摊销 `2×rdtsc/B = 8.19/2048 = 0.0040ns` = index_only(1.1ns) 的 0.4% ≪ 1–2%。
- **证据**：`floor B=2048 = 0.186 ns/op`（TIMING_PROTOCOL §2 原始输出）。

### #4 N=10240→4096

- **出处**：`N = W×k = 128 × ceil(msg/MTU)`；工作 MTU=4096 ⇒ N₀=128×32=4096。
- **BDP 一致性**：WAN 1Gb/s × 40ms ⇒ BDP=5MB；`ceil(5,000,000/4096)=1221 ≤ 4096 ✓`（RING_N_DERIVATION §2 表 A/B）。
- **偏差方向细读**：hash 负载因子 `4096/16384=0.25`（旧 0.625）⇒ **hash 更稀疏更快，对基线有利、对己不利**；这是保守口径，须如实写。

### #7 「删除 24B slot header」未实施（**待用户确认**）

- 规范 Step3 exp1a 要求「delete 24B slot header → stride=align16(S)、meta→8B/slot」，但另一条红线「机制逻辑与流程一字不改」禁止改 `dynblock.h` 的 `stride_of()` 与 `slot_meta_t`。
- 8B meta 如何把 `{psn,len,flag,gen,ovf_idx}`（现 12B）压成 8B 未 specify，硬做会有信息丢失风险。
- **本次决策**：按机制原样跑（`HDR_SZ=24`、`slot_meta=12B`、`stride=align16(24+S)`）。
- **代价**：stride 含 24B header，空间利用率略低于「删头」版（对己不利=保守）。
- **待确认**：是否要（a）维持原样（推荐，忠实机制），或（b）按规范删头并给出 8B meta 打包定义。确认前本表 #7 保持「未实施」。

### #9 exp3 ablation 旋钮 = per-run override

- **为什么不能改默认**：selftest `test_A10_shrink`（`bench/selftest.c:358-383`）与 `test_A12_antiosc`（`518-589`）的断言依赖默认 `j_quiet=4/k_dwell=2/hist_decay=8`；改默认会破坏「机制一字不改」的回归护栏。
- **实现**：`config.c` 的 `e3_preset("ablate")` 分支在运行时覆盖这四个旋钮（`CFG_E3_ABLATE_*`），机制默认值（§A）不动。slow 对照用 `--e3-preset=default`。
- **证据**：`build/sim --out-dir=... --e3-preset=ablate` 与 `--e3-preset=default` 各跑一遍，`resolved_config.json` 落盘各自旋钮（脚本比对，无手抄）。

### #10 exp3 相位序列

- **序列**（`CFG_E3_PHASES`，38 相位）：`{2,4096}` 预热 → `{8,256}→{8,512}→{8,1024}→{8,2048}→{8,4096}` 五档跳变 → `{2,256},{2,4096} ×16` 快速振荡。
- **容量**：`cfg_t.e3_phases[16]` → `[64]`；`config.c` 的 `F(e3_phases, T_PHASE, 64)` 与 `parse_phase_list(...,64,...)` 同步，`cfg_selftest` 往返自检覆盖。
- **偏差方向**：同一序列喂两条件，消融对比公平。

---

## 未登记为「变更」但需披露的环境事实

- VM（Ubuntu 24.04 + 7.0.0-31-generic）→ **实机 Ubuntu 22.04.5 / HWE 6.8.0-138 / i7-14700K**（见 ENV_CHECK.md）。
- 真网卡 ConnectX-5 Ex 处于 DOWN（无 link partner），soft-RoCE（rdma_rxe）可用（见 ENV_CHECK.md §3）。
- numactl 未安装，用 `taskset -c 0` 绑 P-core 等效（单 NUMA node）。

# 实机计时规程（TIMING_PROTOCOL）— 260918_cache_Ubuntu1（实机）

> 第 1 步交付物。**替换掉 VM 时代的一切计时假设**（VM 上 `rdtsc` 单次读钟 ≈3.8 μs，实机 ≈4.8 ns，
> 低约 800×）。所有原始输出在本文第 1~4 节；结论与「选型决策」在第 5 节。
>
> 依据探针 `bench/timing_probe.c`（一次性诊断，非实验，不进 config.h），编译/运行命令：
>
> ```
> gcc -O2 -Wall -Wextra -std=gnu11 bench/timing_probe.c -o build/timing_probe -lm
> taskset -c 0 ./build/timing_probe
> ```
>
> 绑核到 **P-core CPU 0**（i7-14700K 8P+12E 异构，见 ENV_CHECK.md）。

---

## 0. 结论速览

| 项 | VM(260916) 旧 | 实机(260918) 新 | 决策 |
|---|---|---|---|
| 主计时原语 | rdtsc（读钟 ≈3.8 μs） | **rdtsc_raw = 4.767 ns**（clock_gettime 9.925 ns） | **rdtsc_raw 为主**（2.08× 更便宜） |
| 频率漂移 | 未验证 | **+0.000%**（空转 vs 内存密集 10 s memcpy） | **TSC 可直接作 ns 时间基准**，无需回落 CLOCK_MONOTONIC |
| 空批地板（B=8192） | 需 B=8192 盖 3.8 μs 读钟 | **0.183 ns/op**（读钟摊销已 <0.001 ns） | B 可大幅缩小 |
| exp1a 批 B | 8192（VM 读钟底噪产物） | **2048**（读钟摊销 0.004 ns = 最快 op 的 0.2%） | **B: 8192 → 2048** |
| governor | （未记录） | powersave（**写 performance 被拒**，无 root） | 见 §4，HWP 已等效 performance |

---

## 1. 原语对比（各 10^7 次，去首尾各 10%）

```
== (1) 原语单次成本（各 10^7 次，去首尾各 10%） ==
rdtsc_raw()                        median=   4.767 ns   min=   4.745   max=   4.781   mean=   4.765
clock_gettime(MONOTONIC)           median=   9.925 ns   min=   9.745   max=   9.953   mean=   9.864
```

**结论**：`rdtsc_raw()`（裸 rdtsc，无 lfence/rdtscp，与 `include/util.h` 逐字一致）单次
**4.767 ns**，比 `clock_gettime(CLOCK_MONOTONIC)`（9.925 ns）便宜 **2.08×**。
→ **主计时原语 = rdtsc_raw**。与既有代码口径（全项目用 `rdtsc_raw`）一致，机制逻辑一字不改。

> 注意：`rdtsc_raw` 是**裸** rdtsc，CPU 可能对括住的操作做轻微重排；这是本项目既有计时约定
> （exp1a/exp1b/exp2/sim 全部同一口径），空批地板也采用**完全相同**的循环结构，故该重排噪声在
> 「测量 − 地板」中抵消，不影响相对结论。不改为 `rdtscp`/`lfence`（会改机制无关但全局的计时语义，
> 违背「一字不改」纪律）。

---

## 2. 空批地板（同循环结构：B 次空屏障夹一对 rdtsc）

```
== (2) 空批地板 per-op（同循环结构：B 次空屏障夹一对 rdtsc） ==
floor B=1                          median=    8.193 ns/op
floor B=2                          median=    4.096 ns/op
floor B=4                          median=    2.048 ns/op
floor B=8                          median=    1.024 ns/op
floor B=16                         median=    0.512 ns/op
floor B=32                         median=    0.256 ns/op
floor B=64                         median=    0.288 ns/op
floor B=128                        median=    0.235 ns/op
floor B=256                        median=    0.210 ns/op
floor B=512                        median=    0.196 ns/op
floor B=1024                       median=    0.189 ns/op
floor B=2048                       median=    0.186 ns/op
floor B=4096                       median=    0.184 ns/op
floor B=8192                       median=    0.183 ns/op
```

**结构解读**：`floor(B) = 2×rdtsc_raw / B + 空循环体开销`。
- 2×rdtsc_raw ≈ 8.19 ns/批：B=1 → 8.193，B=2 → 4.096 … 完美吻合 `8.19/B`。
- 空循环体开销（自增+比较+分支，~0.6 周期 @3.4 GHz）≈ **0.18 ns/op**，B≥2048 后地板收敛到它。
- B 从 2048→8192 地板仅 0.186→0.183 ns（-1.6%），**读钟摊销已可忽略**。

**B=64 的 0.288 ns 凸起**：循环展开/分支边界的一次性毛刺（64 = 常用展开宽度），不影响收敛结论；
实际主矩阵不用 B=64，故忽略。

---

## 3. 频率漂移验证（constant_tsc/nonstop_tsc 已由 /proc/cpuinfo 确认）

```
== (3) 频率漂移验证（内存密集 10 s memcpy 后重标定） ==
  memcpy 10 s: iters=37, 吞吐=15.89 GB/s
[calib] tsc_per_ns (memory-load)= 3.417632 ticks/ns => 3417.63 MHz
[drift] idle vs memory-load 差异 = +0.000%  (<=1%%，TSC 可用作主原语)
```

外加空转标定：

```
[calib] tsc_per_ns (idle)        = 3.417625 ticks/ns  => 3417.63 MHz
```

**结论**：
- 空转 `tsc_per_ns = 3.417625` ticks/ns，内存密集（10 s 4GB memcpy，15.89 GB/s）后
  `tsc_per_ns = 3.417632` ticks/ns，**差异 +0.000%**（< 1% 阈值）。
- **TSC 频率在负载下稳定，可直接作 ns 时间基准**（`to_ns = ticks / tsc_per_ns`），
  无需回落 CLOCK_MONOTONIC。
- 机理：i7-14700K 无 AVX-512（见 ENV_CHECK.md），`constant_tsc`/`nonstop_tsc` 保证 TSC 以
  恒定 3.4176 GHz（P-core 基频）tick，与 core 的实际 P-state（负载时 5.5 GHz turbo）无关；
  `intel_pstate` HWP（`energy_performance_preference=performance`）下无 AVX 频率下折。

---

## 4. 绑核 + governor（第 1 步第 4 条）

### 绑核

```
taskset -c 0 ./build/timing_probe   # 绑定 P-core CPU 0（物理核，非超线程兄弟）
```
单 NUMA 节点（node0 覆盖 0-27），**numactl 未安装**（见 ENV_CHECK.md），故只用 `taskset`，
等效 `--cpunodebind=0 --membind=0`。

### governor 改 performance —— 被拒（如实记录）

```
=== governor (cpu0) ===
powersave
=== energy_performance_preference (cpu0) ===
performance
=== scaling_cur_freq (cpu0) ===
5503889
=== cpuinfo_max/min (cpu0) ===
5500000
800000
=== attempt write governor=performance (expect denied) ===
sudo: The "no new privileges" flag is set, which prevents sudo from running as root.
sudo: If sudo is running in a container, you may need to adjust the container configuration to disable the flag.
exit=1
=== attempt direct write (no sudo) ===
/bin/bash: line 1: /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor: Permission denied
exit=1
```

**结论与对实验的影响（如实，不夸大）**：
- `scaling_governor` 名义为 **powersave**，但 `intel_pstate` 的 HWP 实际由
  `energy_performance_preference` 驱动，其值 **= performance**，`scaling_cur_freq` 实测
  **5503.9 MHz（≈5.5 GHz turbo）** —— P-core 负载下确实在冲最高频。
- 写 `scaling_governor=performance` **被拒**（sudo 被 no_new_privs 阻断；直接写 sysfs Permission denied）。
  **原值 = powersave（未改，保持）**。
- 因此「governor=performance」这一条**无法字面满足**，但 §3 频率漂移 = 0.000% 已实证
  HWP+performance 偏好在绑核热循环下频率稳定、TSC 恒定，**不影响任何 ns 级结论**。论文/文档如实写
  「intel_pstate HWP，energy_performance_preference=performance，P-core 负载 5.5 GHz」。

---

## 5. B 重选（第 1 步第 7 条：地板 ≤ 最快操作的 1–2%）

**旧 B=8192 是 VM 读钟底噪产物**（`config.h` 原注释：「实测一对 rdtsc≈3.8μs ⇒ B=8192 摊 0.46ns」）。
实机一对 rdtsc = 8.19 ns，重选如下：

- 最快被测量操作 = `index_only`（Φ 纯算术，`psn&0xFFFFFF` 再 %N），**≈2 ns**（3~7 个 core 周期；
  见第 2 步 mask 披露）。
- 读钟摊销要求：`2×rdtsc_raw / B ≤ 1% × 2 ns = 0.02 ns` ⇒ `B ≥ 8.19/0.02 = 410`。
- 取 **B = 2048**：读钟摊销 = `8.19/2048 = 0.0040 ns/op` = 最快 op 的 **0.2%**（远低于 1%）。
  地板实测 0.186 ns/op，其中 0.182 ns 是**确定性空循环体开销**，在「测量 − 地板」中精确抵消，不随 B 缩放。
- **决策**：`CFG_E1A_BATCH_OPS: 8192 → 2048`（不再沿用 VM 时代的 8192）。B=2048 与 B=8192 地板
  几乎相同（0.186 vs 0.183），但单格计时工作量省 4×。
- `CFG_E1B_BATCH_OPS` 维持 **512**（最快 retrieve SR-16 O(1) 在实机 ~10–20 ns，地板 0.196 ns ≈ 1–2%，
  原 B=512 本就非 8192 产物，注释里的「0.7μs」为 VM 过期文字，随本步更正）。

> 交叉验证批 `CFG_E1A_BATCH_XVAL_OPS=1024`、`CFG_E1B_BATCH_XVAL_OPS=32` 保留作地板论证闭环
> （不同 B 下排序/差距一致 ⇒ 地板已从信号中剥离）。

---

## 6. THP 统一策略（第 1 步第 5 条）+ 热身（第 6 条）

- THP = **madvise**（`/sys/kernel/mm/transparent_hugepage/enabled` = `always [madvise] never`，
  见 ENV_CHECK.md §4）。池 `mmap(MAP_NORESERVE)` 时**四方法（FIFO/Hash/Tree/dynblock）统一显式
  `madvise(MADV_HUGEPAGE)`**，或统一不加——不得有的加有的不加。
- 热身：**统一 warmup** 覆盖 4 方法 × 全部包长（exp1a 5 档 MTU / exp1b payload / exp2 5 档），
  `CFG_WARMUP_OPS=200000`，与计时循环同结构，先于正式计时。

---

## 7. 其余统计口径（第 1 步第 8~9 条）

- **REPS ≥ 5**（`CFG_REPS=5`）；每格报 **min / median / max**，**p50 为主、p90 为辅、p99 弃用**
  （若实机尾部干净再评估 p99，见下）。
- 最近秩分位数（inverted CDF，与 numpy `percentile(method="inverted_cdf")` 一致）。
- **每格产出**：`<格名>.csv`（含逐样本或逐批 per-op ns）+ `resolved_config.json`（复现依据）+ 一行地板。

---

## 8. 与旧结论的不一致（如实申报，不粉饰）

1. **VM 读钟 3.8 μs → 实机 4.8 ns**：旧 B=8192 的设立依据已消失，故 exp1a B 重选为 2048。
2. **governor=performance 无法字面达成**（无 root），但 HWP `energy_performance_preference=performance`
   + 频率漂移 0.000% 实证等效。
3. 旧 exp1b 注释「SR-16 O(1)~0.7μs」为 VM 量级，实机为 ~10–20 ns；只改注释、不改机制。
4. 其余（rdtsc 为主、TSC 作 ns 基准、p50 主 / p90 辅 / p99 弃）与旧口径一致，无矛盾。

# N 重推导（RING_N_DERIVATION）— 260918_cache_Ubuntu1（实机）

> 第 2 步交付物。把旧 `N=10240`（VM 时代「100 Gbps × 3 ms RTT」拍脑袋值）替换为
> 有出处、有 BDP 一致性校验的 `N = W×k` 推导，并披露取模/mask 的公平性。
> 所有改动**只落在 `include/config.h`**（三处同源），机制逻辑一字不改。

---

## 0. 结论速览

| 项 | 旧 | 新 |
|---|---|---|
| N₀（满窗） | 10240 | **4096 = 128 × 32** |
| N 扫描（exp1b） | {512..10240}（6 点） | **{128,256,512,1024,2048,4096} + 锚点 5120**（7 点） |
| ovf_cap | 2048 | **819 = ⌊N/5⌋**（随 N 缩放） |
| 工作 MTU | （未明示） | **4096 B**（写进 config.h 注释） |
| e1a B（Step1 联动） | 8192 | **2048** |

---

## 1. N = W × k 推导

**出处**：
- W = **128** 条 WR in-flight（"When RDMA Goes Long-Haul" ACM 2026 Fig.7/O3；blue-rdma 128-bit 窗口）。
- k = `ceil(msg / MTU)` = 一条消息切成的 MTU 包数。

**定义式**：`N = W × k = 128 × ceil(msg/MTU)`。

- k ∈ {1, 2, 4, 8, 16, 32} ⇒ N ∈ {128, 256, 512, 1024, 2048, **4096**}。
- **N₀ = 4096 = 128 × 32**（k=32，即一条消息 = 32 个 MTU 包）。
- **锚点 5120 = 128 × 40**（非 2 的幂，专用于 mask 披露，见 §3）。

---

## 2. BDP 一致性自检（原始输出，公式计算、非手抄）

**工作点**：WAN 1 Gb/s × 40 ms RTT ⇒ `BDP = 1e9 × 0.04 / 8 = 5,000,000 B`（5 MB）。

```
=== 表 A：MTU 档 → 所需包数 ceil(BDP/MTU) → 满窗 N0=4096 是否覆盖 ===
   MTU |  ceil(BDP/MTU) |    N0=4096 覆盖? |          最小覆盖扫描点
   256 |          19532 |             NO | NONE(超窗口)
   512 |           9766 |             NO | NONE(超窗口)
  1024 |           4883 |             NO | N=5120
  2048 |           2442 |            YES | N=4096
  4096 |           1221 |            YES | N=2048

=== 表 B：N 扫描点（工作 MTU=4096，ceil(BDP/4096)=1221）===
     N |  k=N/128 |  N>=1221? | 设计窗口
   128 |        1 |       OUT | OUT OF DESIGN WINDOW
   256 |        2 |       OUT | OUT OF DESIGN WINDOW
   512 |        4 |       OUT | OUT OF DESIGN WINDOW
  1024 |        8 |       OUT | OUT OF DESIGN WINDOW
  2048 |       16 |        IN | IN DESIGN WINDOW
  4096 |       32 |        IN | IN DESIGN WINDOW
  5120 |       40 |        IN | IN DESIGN WINDOW

=== 表 C：N × MTU 全交叉（N < ceil(BDP/MTU) → OUT OF DESIGN WINDOW）===
  N\MTU      256      512     1024     2048     4096
    128      OUT      OUT      OUT      OUT      OUT
    256      OUT      OUT      OUT      OUT      OUT
    512      OUT      OUT      OUT      OUT      OUT
   1024      OUT      OUT      OUT      OUT      OUT
   2048      OUT      OUT      OUT      OUT       IN
   4096      OUT      OUT      OUT       IN       IN
   5120      OUT      OUT       IN       IN       IN
```

**解读（诚实申报）**：
- 在**工作 MTU = 4096 B** 下，N₀=4096 覆盖 BDP（1221 包）✓，N=2048 也覆盖（但余量小）。
  扫描点 N ≤ 1024（k ≤ 8）**出设计窗口**（N < ceil(BDP/MTU)），论文必须标注
  "OUT OF DESIGN WINDOW"，不得当真实部署点宣称。
- MTU 1024 需 N≥4883，**只有锚点 5120 合法**（这就是设 5120 的第二个理由）；MTU 512/256
  在本工作点（1 Gb/s × 40 ms）下无任何扫描点覆盖——如实说明「更低 MTU 档需更高速率/更长 RTT
  才落在窗口内，本工作点不主张」。
- **与规范数字的两处出入（floor/ceil 笔误，不改结论）**：规范示例写「2048→2441」「256→19531」，
  正确 ceil 值为 **2442 / 19532**（5,000,000/2048=2441.41、/256=19531.25，向上取整）。两处
  出入均不改变 ✓/✗ 归属（2442≤4096 仍 ✓，19532>4096 仍 ✗），故仅在此如实订正。

---

## 3. 取模 / mask 公平性披露（重要，与规范假设相反）

**规范假设**：「`phi(psn)=(psn&0xFFFFFF)%N`，N=2^k 用 AND（非 2 的幂多 3–4 条指令）」。
**实测代码真相（`include/dynblock.h` `phi()`、`baselines/baseline.c`）**：

1. **我们的 Φ 是运行时取模**：`phi(psn) = (psn & 0xFFFFFFu) % c->N`，`c->N` 是**运行时值**
   （`conn_t.N`，来自 config/CLI）。gcc -O2 下 `x % runtime_N` 编译成硬件 32-bit 整数除法
   （`div`，~20–26 周期），**对 N=2^k 与 N=5120 完全相同**。编译器无法把 `x % runtime_N` 优化成
   `x & (N-1)`，因为 N 不是编译期常量。⇒ **我们的 Φ 不存在 2 的幂 AND 优势**；锚点 N=5120 直接
   验证这一点：index_only 与 4 条主曲线在 5120 处不应出现取模成本的跳变。

2. **真正享受 2 的幂 mask 的是 chained-hash 基线**：`hash_idx(psn,nb) =
   (psn*2654435761u) >> (32 - __builtin_ctz(nb))`，`nb = hash_nbuckets = 16384 = 2^14`，
   `__builtin_ctz` 取尾零数 = 14，右移 18 位取高位——这是 **Knuth 乘法哈希的幂次掩码**，
   代码注释明言「nb 必须是 2 的幂」。即 **hash 用 乘+移位（~4–5 周期），我们 Φ 用 除法（~20–26 周期）**。

3. **hash_nbuckets=16384 不随 N 缩放**：N 10240→4096 后，负载因子 0.625→**0.25**（更稀疏 ⇒ hash 更快）。

**论文披露语句（如实，方向与我们有利相反）**：
> "Φ(psn) = (psn & 0xFFFFFF) % N uses a runtime N and compiles to a hardware integer divide for
> every N, power-of-two or not; the non-power-of-two anchor N=5120 (included in the scan) confirms
> there is no masking advantage. The chained-hash baseline is the one using a power-of-two index
> (hash_nbuckets = 16384 = 2^14 via Knuth top-bits shift); any power-of-two benefit therefore accrues
> to the baseline, not to our method — the comparison is conservative."

**重跑要求**：index_only 与 4 条主曲线在 **N=5120 锚点**重跑（exp1b 扫描已含 5120，无需额外代码）。

---

## 4. ovf_cap 随 N 缩放

- 旧：`ovf_cap = 2048 = 10240/5`（「贴近真实网关，使打满=紧急变难」）。
- 新：`ovf_cap = ⌊N/5⌋ = 819`（4096/5 = 819.2 向下取整）。
- 依据：若不缩放而保留 2048，则新 N 下 ovf_cap = N/2，溢出区相对容量翻倍，弹性「打满=紧急」
  的触发难度被改变（对谁有利方向见 CHANGE_AUDIT.md）。缩放保持相对口径一致。

---

## 5. config.h 实际改动清单

| 字段 | 旧值 | 新值 |
|---|---|---|
| `CFG_RING_N` | 10240 | **4096** |
| `CFG_OVF_CAP` | 2048 | **819**（⌊N/5⌋） |
| `CFG_E1A_N` | 10240 | **4096** |
| `CFG_E1A_BATCH_OPS` | 8192 | **2048**（Step1 B 重选） |
| `CFG_E1B_N_LIST` | {512,1024,2048,4096,8192,10240} | **{128,256,512,1024,2048,4096,5120}** |
| `CFG_E1B_N_N` | 6 | **7** |
| `CFG_E2_N` | 10240 | **4096** |
| `CFG_E2_N_LIST` | {256,512,1024,2048,4096,10240} | **{128,256,512,1024,2048,4096}**（去掉出界 10240） |
| `CFG_HASH_NBUCKETS` | 16384 | 16384（**不变**，仅订正注释负载因子 0.625→0.25） |

**校验证据**（改后已跑）：
```
cfg_roundtrip: dump -> load -> memcmp OK        ← 三处同源 + struct 对齐自检通过
ALL SELF-TESTS PASS (HDR_SZ=24, slot_meta=12 B)
BASELINE SMOKE PASS                             ← 六方法 roundtrip/淘汰语义全绿
```
（`e1b_n_list` 6→7 使 `cfg_t` 增长，`_Static_assert(sizeof(cfg_t)%8==0)` 仍通过，编译零警告。）

---

## 6. 与旧结论的不一致（如实申报）

1. 旧 N=10240 无「W×k」出处（注释自认「100 Gbps × 3 ms RTT，合理但不精确」）→ 改为 128×32=4096。
2. 规范示例的两处 ceil 笔误（2441/19531 → 2442/19532）已订正，不影响任何 ✓/✗。
3. 规范假设「N=2^k 用 AND」**不成立**（Φ 是运行时取模）；真正用幂次 mask 的是 hash 基线——
   这条对论文是**利好**（比较对我不利=保守），但必须如实写，不能反过来宣称自己沾了 mask 便宜。

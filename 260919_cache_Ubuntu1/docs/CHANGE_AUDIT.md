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
| 7 | exp1a「删除 24B slot header → stride=align16(S)、meta→8B/slot」**未实施** | 与「机制逻辑一字不改」冲突 + 8B meta 打包方式未 specify | 保留 24B header ⇒ stride=align16(24+S) 更大 ⇒ 空间利用率略低（**对己不利=保守**），但机制忠实 | 是（须写 stride=align16(24+S) 而非 align16(S)）→ **2026-09-19 被 #11 反转** |
| 8 | exp1b dynblock 环长=扫描点 N（原 harness 环长固定 ring_n=4096） | 锚点 5120 > ring_n=4096，固定环会在 N=5120 静默淘汰 ~20% 条目；且「窗口 N」在四方法上应同义 | 中性（`Φ=psn%N` 一字未改，仅实例化环长；让 FIFO/Hash/Tree 的 capacity=N 与 dynblock 环长=N 对齐） | 是（须写 dynblock 环长=窗口 N） |
| 9 | exp3 ablation 旋钮经 `--e3-preset=ablate` 覆盖：k_dwell 2→1、ovf_thresh 256→1、j_quiet 4→1、hist_decay 8→2（机制默认值一字未改） | 消融要测「去滞后」档；默认值由 selftest A10（缩）/A12（抗振荡）护栏验证，不能直接改默认 | 只作用于 ablation 条件；slow 对照仍用默认滞后（慢=对己不利=保守）。同相位序列下对比，公平 | 是（须写「ablation 为 per-run override，非默认变更」） |
| 10 | exp3 相位序列重写：5 档 MTU 跳变 + 256↔4096 ×16 振荡（38 相位）；e3_phases 容量 16→64 | 规范 Step3 exp3 要求「5 档 MTU 跳变 incl 256↔4096 16×」 | 中性（同一相位序列喂给两条件） | 是 → **2026-09-19 被 #15 取代** |
| 11 | **删除 24B 死块头**（stride_of→align16(S)、CFG_HDR_SZ→0、同步删 3 个 hdr_set 写入点 + selftest hdr_of） | 2026-09-19 第 1 条：24B 头只在 selftest 被读、热路径只写不读；stride=align16(24+S)=S+32 每槽多跨 64B cache line、256B 档浪费 12.5% | stride 缩短 32B/槽（5 档 MTU 均为 32B），**空间利用率对己有利**；但正确性保留 `m.psn!=psn` 校验（「零搜索、一次校验」，非性能优化） | 是（须写 stride=align16(S)、论文措辞「零搜索、一次校验」非「零比较」） |
| 12 | exp1b「第三项指标 n_cmp」降为「内部诊断」：n_cmp.csv 仍落盘留痕，但退出 RESULTS.md/图/论文 | 2026-09-19 第 2 条「n_cmp 退出正文」 | 中性（n_cmp=内部比较次数诊断，非计时性能指标；退出正文不改任何方法口径） | 是（论文只用 p50/p90；n_cmp 仅作复杂度证据留 CSV） |
| 13 | GBN 取包改 `retrieve_range` 统一口径：一次 Φ(start) + 顺序 meta 读 + 环回绕，输出天然 PSN 升序 | 2026-09-19 第 4A「GBN 区间提取」 | 中性（内部实现口径；range_equiv_test 验证与 per-key 参考逐字节一致） | 是（GBN 用 retrieve_range，环回绕语义注明） |
| 14 | 分配策略正交全矩阵：methods(5) × alloc(pooled/perstore)；node_alloc/node_release 抽象；perstore 工厂；b_cache 增 `perstore`/`n_free` | 2026-09-19 第 5 条「分配策略正交全矩阵」 | 中性（pooled/perstore 同一索引结构，唯一变量=分配策略；alloc_equiv_test 验证字节一致） | 是（须写 perstore 逐 store malloc/free、alloc_mode 列） |
| 15 | exp3 相位重设计：38 相位（5 档跳变+256↔4096 ×16 振荡）→ 11 相位（1 预热 + 5 升档 + 5 降档，每档稳定 16 纪元） | 2026-09-19 第 3 条「每档稳定 16 纪元再切换」；新指标=利用率时间序列 + 逐相位统计（排除切换后首 6 纪元）+ 收敛时间 | 中性（同一相位序列喂两条件） | 是（取代 #10） |
| 16 | SR `retrieve_set` 统一交付契约：按 PSN 升序交付（fifo/hash qsort、tree 中序零排序、dynblock 扫槽零排序）；exp1b reorder∈{0,1} 维度废止（升序上移到接口内） | 2026-09-19 第 4B「SR 统一交付契约」 | 诚实披露：SR 计时=定位+memcpy+升序交付全过程；tree/dynblock O(N) 零排序、fifo/hash O(k log k) qsort——大 N、小 k 下 hash 的 qsort 占优（见 CLAIM_GUARDRAILS） | 是（须写 retrieve_set 升序契约 + SR 各方法升序成本归属）；**tree 部分 2026-09-20 被 #18 修订** |
| 17 | SR `retrieve_set` 位图快路径：`conn_retransmit_set` 全环扫描 O(N) → 位图 O(n + span/64)（栈上 `bm[CFG_SR_BM_WORDS]`，无 malloc、无比较排序）；span 超限/24-bit 跨界/span>N 时退化 `conn_retransmit_set_ref`（O(N) 参考路径，正确性优先） | 2026-09-20 P0「只改 SR 取包路径」；`CFG_SR_BM_MAX_N=8192` | 对己有利（SR-64 增长压降 13.2×）；正确性由 `sr_equiv_test` 护栏（随机集逐字节一致） | 是（位图快路径 + 退化语义 + sr_equiv_test） |
| 18 | tree SR 主路径：中序 O(N) → `tree_search_set`（复制→排序→k 次 O(log N) 查找）；旧中序保留为 xval（`e1b_tree_inorder=1`，`tree_set_from_root`） | 2026-09-20 P1 基线公平性：tree 与 fifo/hash 同构（排序+k 次检索），不再以 O(N) 全遍历交付（**取代 #16 的「tree 中序零排序」口径**） | 对 tree 基线有利（更快）；但属公平对齐——不再让 tree 以 O(N) 全遍历独占零排序 | 是（须写 tree SR=排序+k 次 O(log N) 查找，旧中序作 xval） |
| 19 | 排序原语正交维度：`sort_u32_asc`（0=glibc qsort 主 / 1=内联插入排序 xval）；`e1b_sort_impl` 配置 + CSV `sort_impl` 列 | 2026-09-20 P1 排序原语交叉验证（证明排序路径结论不依赖 qsort 实现） | 中性（同一排序路径两原语，仅 SR 排序路径） | 是（sort_impl 列 + xval 一致性） |
| 20 | `psn_dynblock_fixed` → `ring_fixed` 命名统一（exp1a/exp1b/exp2 全表 + 图） | 2026-09-20 P2 命名清理（论文术语「PSN fixed block (S=4096)」） | 中性（仅命名，机制语义一字未改） | 是 |
| 21 | exp2 新列 `overhead_per_pkt` / `lower_bound_util` + 新实验 `exp2_tail`（尾包占比 f 敏感性，MTU=4096、N=4096） | 2026-09-20 P2 实验二(b)「单一全局 S 在尺寸尾下的稳定性」 | 中性（新指标/新工作量，不改任何方法口径） | 是 |

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
- **2026-09-19 解决**：用户第 1 条明确「删 24B 死块头、但保留 psn 校验」（不要求 8B meta 打包，`slot_meta_t` 维持 12B 不动），故 #7 的 8B-meta 顾虑不再成立；按第 1 条实施，见 #11。

### #9 exp3 ablation 旋钮 = per-run override

- **为什么不能改默认**：selftest `test_A10_shrink`（`bench/selftest.c:358-383`）与 `test_A12_antiosc`（`518-589`）的断言依赖默认 `j_quiet=4/k_dwell=2/hist_decay=8`；改默认会破坏「机制一字不改」的回归护栏。
- **实现**：`config.c` 的 `e3_preset("ablate")` 分支在运行时覆盖这四个旋钮（`CFG_E3_ABLATE_*`），机制默认值（§A）不动。slow 对照用 `--e3-preset=default`。
- **证据**：`build/sim --out-dir=... --e3-preset=ablate` 与 `--e3-preset=default` 各跑一遍，`resolved_config.json` 落盘各自旋钮（脚本比对，无手抄）。

### #10 exp3 相位序列（**2026-09-19 被 #15 取代**）

- **序列**（`CFG_E3_PHASES`，38 相位）：`{2,4096}` 预热 → `{8,256}→{8,512}→{8,1024}→{8,2048}→{8,4096}` 五档跳变 → `{2,256},{2,4096} ×16` 快速振荡。
- **容量**：`cfg_t.e3_phases[16]` → `[64]`；`config.c` 的 `F(e3_phases, T_PHASE, 64)` 与 `parse_phase_list(...,64,...)` 同步，`cfg_selftest` 往返自检覆盖。
- **偏差方向**：同一序列喂两条件，消融对比公平。
- **2026-09-19 被 #15 取代**：第 3 条改为「每档稳定 16 纪元再切换」的 11 相位设计，见 #15。

### #11 删除 24B 死块头（保留 psn 校验）

- **改动**：`stride_of(S)=align16(24+S)→align16(S)`；`payload_of→slot(p,i)`；`ovf_payload→e->blk`；`CFG_HDR_SZ 24→0`（deprecated）；同步删 3 个 `hdr_set` 写入点（`dynblock.c:99`、`dynblock.c:288`、`pool.c:77`）+ `selftest.c` 的 `hdr_of` 校验；`baseline.c:705`/`sim_main.c:55` 溢出字节口径 `align16(24+len)→align16(len)`。
- **正确性红线**：`conn_lookup` 的 `m.psn != psn` 校验**保留**（环回绕可能存 psn+N 旧包，不比 psn 会返回错包——正确性非性能）；论文措辞统一「零搜索、一次校验」。
- **依据**：`hdr_of()` 唯一调用点是 `selftest.c:78`；热路径只写不读 24B；5 档 MTU 下 `stride_of(S)=align16(24+S)=S+32`（每槽多跨一条 64B cache line、256B 档浪费 12.5%）。
- **证据**：`build/selftest` 全绿（stride 校验更新为 1504/48）；stride/pool 字节表见下；exp2 利用率反转（psn_dynblock @256B 84.43%→94.40%，反超 FIFO 88.89%）。

stride / pool 字节（N=4096，脚本计算）：

| MTU/S | stride 旧 | stride 新 | pool 旧(B) | pool 新(B) | 省(B) | 省% |
|---|---|---|---|---|---|---|
| 256 | 288 | 256 | 1179648 | 1048576 | 131072 | 11.11% |
| 512 | 544 | 512 | 2228224 | 2097152 | 131072 | 5.88% |
| 1024 | 1056 | 1024 | 4325376 | 4194304 | 131072 | 3.03% |
| 2048 | 2080 | 2048 | 8519680 | 8388608 | 131072 | 1.54% |
| 4096 | 4128 | 4096 | 16908288 | 16777216 | 131072 | 0.78% |

- **偏差方向**：空间利用率对己有利（stride 缩短 32B/槽，5 档均为 32B）；故 exp2 利用率反转后须在 CLAIM_GUARDRAILS 如实更新为「弹性利用率反超最优 FIFO」，不得隐瞒。

---

### #12 n_cmp 退出正文（第 2 条）

- 「第三项指标 n_cmp」→「内部诊断」：n_cmp.csv 仍落盘留痕（retrieve 前清零、批后累计，报每次取包平均比较次数），但退出 RESULTS.md/图/论文。
- 论文指标只用 p50/p90；n_cmp 仅作复杂度证据（FIFO O(N)≈N/2、tree O(log N)、hash O(1)、dynblock/index_only=0）。
- 交付排序不计 n_cmp（n_cmp 只计缓存内检索比较）。

### #13 GBN 区间提取（第 4A）

- `retrieve_range` 统一口径：一次 Φ(start) + 顺序 meta 读 + 环回绕（i 单调 +1 跨 N 归 0），输出天然 PSN 升序。
- **证据**：`range_equiv_test`（10 万次随机区间）验证新路径与 per-key 参考逐字节一致。

### #14 分配策略正交（第 5 条）

- methods(5) × alloc(2)：fifo / chained_hash / balanced_tree / psn_dynblock /（index_only 仅 pooled）；pooled=一次性预分配空闲链表复用（n_malloc=0），perstore=逐 store malloc、淘汰 free（n_malloc=n_free=实际 store 次数）。
- 抽象：`node_alloc`/`node_release`（pooled→pool_get/pool_put，perstore→malloc/free）；`b_cache` 增 `perstore`/`n_free`；perstore 工厂 `make_*_perstore`；`--alloc-mode=pooled|perstore` CLI。
- **证据**：`alloc_equiv_test` 验证 pooled==perstore 字节一致；`alloc_probe` 测裸 malloc/free（32/56/64/4128 B）；exp1a 增 alloc_mode 列；`alloc_sensitivity.md` + `fig_exp1a_alloc_sensitivity` 落盘。

### #15 exp3 相位重设计（第 3 条）

- 11 相位 = `{1,4096}` 预热 + `{16,256}→{16,512}→{16,1024}→{16,2048}→{16,4096}` 升档（5）+ `{16,4096}→{16,2048}→{16,1024}→{16,512}→{16,256}` 降档（5）；每档稳定 16 纪元再切换。
- 新指标：① 利用率时间序列（`fig_exp3_util`）② 逐相位统计（排除切换后首 6 纪元，mean/min/max+振幅）③ 收敛时间（切换后 S 首次命中目标 MTU 的纪元数，升档+降档共 10 次切换）。
- 保留 slow 对照（默认滞后）；方向分类升档/降档/持平（4096 平台 = 持平）。

### #16 SR 统一交付契约（第 4B）

- `retrieve_set` 统一按 PSN 升序交付命中包：fifo/hash 显式 qsort（`sorted_retrieve_set`）、tree 中序遍历零排序（`tree_retrieve_set`）、dynblock 位置映射扫槽零排序（`conn_retransmit_set`）。
- **证据**：`sr_order_test` 验证 4 方法升序契约。
- exp1b reorder∈{0,1} 维度废止（升序上移到 retrieve_set 内）；SR 计时=定位+memcpy+升序交付全过程。
- **诚实披露**：tree/dynblock 升序交付为 O(N) 零排序、fifo/hash 为 O(k log k) qsort；大 N、小 k 下 hash 占优（SR 非 PSN 主战场；PSN 主战场是 GBN 区间提取 O(1)）。

---

### #17 SR 位图快路径（P0）

- `conn_retransmit_set`：请求 PSN 集跨度 `span=hi-lo+1 ≤ CFG_SR_BM_MAX_N(=8192)` 且 24-bit 不跨界时，走位图快路径——栈上 `bm[CFG_SR_BM_WORDS]`（8192/64=128 字 = 1KB），无 malloc、无比较排序，扫请求集标位 + 按 PSN 序扫位图交付，O(n + span/64)。
- **退化语义**：span 超限 / `(lo&0xFFFFFF)+span > 2^24` 跨界 / `span > N` 时，退化 `conn_retransmit_set_ref`（O(N) 参考路径），正确性优先。
- **正确性红线**：`conn_lookup` 的 `m.psn != psn` 校验保留（机制逻辑一字不改，只改 SR 取包路径）。
- **证据**：`sr_equiv_test`（10 万次随机请求集）验证位图快路径与参考路径逐字节一致。

### #18 tree SR 主路径公平对齐（P1）

- 主路径（`e1b_tree_inorder=0`，默认）：`tree_search_set` = 复制请求 PSN → 排序 → k 次 O(log N) AVL 查找（与 fifo/hash 的 `sorted_retrieve_set` 同构）。
- xval（`e1b_tree_inorder=1`）：`tree_set_from_root` 中序 O(N) 全遍历 + psn_set 成员判定（旧路径保留作交叉验证）。
- **为什么改**：旧 #16 口径下 tree 以 O(N) 中序全遍历交付升序（独占零排序）；fifo/hash 却要显式 qsort——tree 既不在「排序」也不在「查找」上与其它方法同构。改为「排序+k 次查找」后，4 方法 SR 工作形状对齐：fifo=O(k log k) 排序+O(1) 查找、hash=O(k log k) 排序+O(1) 查找、tree=O(k log k) 排序+O(log N) 查找、dynblock=位图 O(n+span/64) 零排序。
- **偏差方向**：tree 基线更快（对基线有利），但这是公平对齐（不再以 O(N) 全遍历衡量 tree）。旧中序路径保留为 xval 可复核。

### #19 排序原语正交（P1）

- `sort_u32_asc(a,n,impl)`：impl=0 用 glibc qsort（主），impl=1 用内联插入排序（xval）。
- `e1b_sort_impl` 配置选主排序原语；CSV 增 `sort_impl` 列；xval（sort_impl=1）与主（sort_impl=0）一致 ⇒ 排序路径结论不依赖 qsort 实现（见 exp1b 关键结论第 6 条）。

### #20 ring_fixed 命名统一（P2）

- `psn_dynblock_fixed` → `ring_fixed`（exp1a/exp1b/exp2 全表 + 图 + 数字文件）；显示名统一「PSN fixed block (S=4096)」。
- 机制语义一字未改（S=4096 固定、去弹性）；仅命名与论文术语对齐。

### #21 exp2 新列 + exp2_tail（P2）

- exp2_space 增 `overhead_per_pkt = (allocated − payload)/N`、`lower_bound_util = pl/(align16(pl)+sizeof(slot_meta_t))`（理想上界，方法无关），并加 `ring_fixed` 方法列。
- 新 bench `exp2_tail_sensitivity.c`：包尺寸 `(1-f)@MTU + f@U[1,MTU]`，f ∈ {0,0.01,0.03,0.06,0.10,0.20,0.30}，MTU=4096、N=4096；输出 `final_S/utilization/overhead_per_pkt/n_ovf_ins/n_drop/n_resize`。
- 结论：弹性 S 被满 MTU 包钉在 4096，7 档 f 全程零扩缩/零溢出/零丢包，utilization 随 f 单调下降——单全局 S 在尺寸尾下稳定（graceful degradation）。

---

## 门（gate）结果（2026-09-20，如实报告）

### SR-64 伸缩性门（第 4B/P1 门槛：N=128→5120 增长 < 20%）—— **未过**

- **实测**：PSN(adaptive S) sr_64 p50 N=128→5120 = 864→1501 ns（**+74%**），> 20% 门槛。
- **对照**（同表，脚本计算）：index_only（Φ 纯算术，无 memcpy）1755→1778（**+1%**，扁平）；gbn_long64（连续 64KB memcpy）987→1292（**+31%**）。
- **诊断**：增长来自 64KB memcpy 的缓存局部性（源环 128KB→5MB 跨 L2→L3），SR-64 再叠加离散访问惩罚；位图扫描 O(span/64) 可忽略（index_only 扁平佐证），**非算法退化**。
- **旧版对比**：位图快路径前 sr_64 增长 **+972%**（1389→14886）；位图快路径把增长压降 **13.2×**、sr_64@5120 降至 1501 ns。
- **处置**：**保留现状、如实报告，未改基准/未改口径**（纪律：「若任何门未通过，保留现状并报告失败点与诊断」）。完整诊断见 `out/exp1b_lookup/exp1b_analysis_data.md` 关键结论第 5 条。

---

## 未登记为「变更」但需披露的环境事实

- VM（Ubuntu 24.04 + 7.0.0-31-generic）→ **实机 Ubuntu 22.04.5 / HWE 6.8.0-138 / i7-14700K**（见 ENV_CHECK.md）。
- 真网卡 ConnectX-5 Ex 处于 DOWN（无 link partner），soft-RoCE（rdma_rxe）可用（见 ENV_CHECK.md §3）。
- numactl 未安装，用 `taskset -c 0` 绑 P-core 等效（单 NUMA node）。

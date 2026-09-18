# README 待写清单（Step 12 合并进 README.md 前的工作稿）

本文件是累积清单，不是 README 本身。写 README 时逐条合并，合并后可删本文件。

## A. 设计口径 / 措辞红线

1. **resize 临界区含 mmap/munmap**（约 84 MB @ N=10240,S=4096）。事实：MAP_NORESERVE 下 mmap 不 touch 页（微秒级），真正成本在后续 store 首次写缺页与新池 munmap，均**不在热路径**且**只在罕见 resize 事件**。措辞按 §12.6「**不移动环内已有数据（O(1) 池切换）**」，**不得写「零停顿」**。
2. **gen 8-bit 回绕依赖**：`gen_switch` 前置「旧池已排空」检查 ⇒ 系统里永远不存在「两代之前」的活包 ⇒ gen 回绕安全。放宽该检查前必须重新论证（代码注释已写在 `src/dynblock.c` gen_switch 处）。
3. **N 定位**：N = 重传窗口长度（in-flight 包数上界），不是自由超参。定义式 `N >= ceil(link_rate_gbps × RTT_ms / MTU_B) × safety`。例：100 Gbps×1 ms=12.5 MB，MTU 4096→3052 包，×2→6104→取档。10240 ≈ 100 Gbps×3 ms @4096B（跨洲 WAN）。**红线：不得为让基线跑得快把 N 取成 2 的幂。**
4. **index_only 论证（N 无关）**：同一个 N 时，`phi=(psn&0xFFFFFF)%N` 与 `psn%N` 是**同一次整数取模**，唯一差别是我们要多做一次 24-bit 掩码；该掩码是 24-bit 回绕防御（RDMA PSN 本身 24-bit），上层保证 psn<2^24 时冗余、可改 debug-only assert。**若某配置 N 恰为 2 的幂**（基线取模退化为位与），**如实说明「该配置下基线少一步」**，不回避。exp1a 同时报「N 为 2 的幂 / 非 2 的幂」两种配置，证明索引成本差 ≤1 cycle。
5. **ovf_thresh 绝对数 vs 纪元长度=N**：攒够阈值所需**包数**恒定，但所需**纪元数**反比于 N ⇒ 滞后强度随 N 漂移（N 大时 1 纪元即攒够、k_dwell 几乎自动满足；N 小时要十来个纪元）。exp3 扫 N（1024/4096/10240）验证 S_trace 形态稳定；不稳则改比例式 `max(64, N/40)`（可作鲁棒性亮点）。
6. **baselines 容量统一从 cfg.ring_n 读**，不得各自硬编码常量；`hash_nbuckets=16384` 是 hash 自身参数（按目标负载因子 ≈0.6 与预期元素数 N=10240 选定：10240/16384=0.625；与 behavior_bench.c HASH_BUCKETS=16384 一致），README 说明「负载因子=活跃键数/16384 ≈ 0.625（*_bounded 活集恒 N）」。psn_bench.c 的 4096 是旧 lookup 基准遗留值，旧 lookup 数据与新数据不可直接比较。
7. **D2 语义差**：hash/tree 无界、fifo 满即冻结、只有 psn_mapping/psn_dynblock 是有界窗口+淘汰。exp2 无界版内存占用单独标注（一直涨，不是缓存）。
8. **D3 重复键**：chained_hash 是唯一「重复写内存无界膨胀」的方法（头插保留所有重复节点）。

## B. 计时纪律（写死）

9. **selftest 的计时不可用于论文**（无批量、best-of-3、机器负载敏感）。
10. **exp1a/exp1b 必须**：批量 B=128 + REPS=5 + 中位数 + 绑核；报跨 rep 的 std 与 P90/P99，让噪声可见；去噪后仍在噪声内 → 只报「在噪声内相当」，**不得写「我们更快」**。
11. guard cost 噪声内不可测（三次实测 delta −2.45 / +77.34 / +3.55 ns）。
12. **TSC 交叉验证**：2026-09-16 实测 `tsc_cross_validate(10^7)` dev = **−0.08%**（wall 1.4024 vs rdtsc 1.4013 ns/op），rdtsc 可信。

## C. 计数语义

13. `epoch_stores` = store **调用**次数（含溢出路径），**非**环槽写入。
14. 溢出包也执行 `evict_occupant`（先淘汰再溢出路径）。
15. `n_drain_residual` = **最近一次** drain 的残留数（每次重置）；`n_drain_skip` = **累计**。

## D. 预热口径

16. 预热照 `behavior_bench.c:978-980`（{5120,1056,512}×2N），但该集合**覆盖不全**：只暖到 psn_fixed 的 5120；fifo/hash@64B=96、tree@64B=112、psn_tiered@64B=288 均未暖。exp1a 必须**补齐各方法实际 malloc 尺寸类**。
17. 无界 fifo/hash/tree 每 store malloc 且**不 free**（不自我预热）；psn_fixed/dynamic 覆盖旧块（malloc 后 free 旧块）、psn_tiered 走 free-list 复用 ⇒ 旧 `store_samples.csv` **系统性高估** fifo/hash/tree 的 store 延迟（相对 psn_fixed）。

## E. 其它（可选）

18. residual>0（溢出区全是 len>SC_MAX 项）时 `ovf_count>=ovf_thresh` 恒成立 ⇒ 每纪元白跑一次「要扩 → S_new=ceil_class(L_ovf)=8192==S → 不切 → grow_streak 清零」。可选优化：`ceil_class(L_ovf)<=c->S` 时视为「已到顶」、不累计 grow_streak。非必须。

## F. VM 读钟底噪（2026-09-16 实测）

19. **本机（VM）每次读钟有 μs 级固定开销**：一次 rdtsc 往返约 3.6 μs、clock_gettime 约 3.7 μs、THREAD_CPUTIME 约 7 μs，而真实 conn_lookup 仅 ~2.4 ns（批量摊销）——信噪比 ~1:1500。**因此任何逐次计时都不可用；本文所有延迟均为批量摊销值。**（底噪来自「读钟这个动作」本身——clock_gettime 也是 3.7 μs——不是 TSC 的特性。）
20. 由此对 exp1 的硬约束：**exp1a / exp1b 必须批量**（B=128 + REPS=5 + 中位数，见 B 节）；**NAK 级事件（单次）也必须批量**。
21. **overhead.csv 的 check/resize/drain 是单次事件**，带 ~2×读钟底噪 ⇒ 结论只能是「≤ 底噪 + 未知」，不得说「很小」；该 CSV 已加 `#` 注释行与 `clock_floor_ns` 列标注。
22. latency_trace.csv 用批量摊销的 `lookup_avg_ns`（一对 rdtsc 包 16384 次，底噪摊销 4μs/16384≈0.24ns）+ 子批 max（`lookup_max_ns`，一对 rdtsc 包 1024 次，**含 ~4μs/1024≈3.9ns 底噪摊销**）+ `clock_ns_per_op`（读钟底噪），读者据此判断信号/本底比。

## G. 自适应两条升级路径（exp3 正式发现）

23. 自适应有**两条升级路径**：① 阈值路径（细水长流）——纪元末 `ovf_count >= ovf_thresh(256)`，连 k_dwell 纪元才扩；② 紧急路径（突发打满）——纪元中溢出打满 `ovf_cap(2048)` 时当场 `grow_locked`。本负载（分阶段，N=10240）下 4096 大包瞬间打满溢出，**② 先触发**，这是设计如此、不是缺陷。
24. 阈值路径由「中等溢出率」负载证明（`--e3-preset=mixed`，3% @4096 + 97% @1024，40 纪元）：`3%×N≈307 落在 (256,2048)`，故纪元末阈值先触发，`resize_events` 出现 `reason=grow`（阈值），且扩发生在混合阶段第 2 个纪元末（k_dwell=2）。

## H. Step 8 落定（本轮裁决）

25. **resize 的真实成本在 `overhead.csv` 的 `resize_ns`**（单事件，含 ~2×读钟底噪）：shrink 实测 22.6/27.0 μs（真实 ~14–19 μs）。`lookup_max_ns` **不能**作「resize 阻塞 lookup」的证据——`latency_probe` 在 `conn_store` 返回之后才跑、与 resize 不重叠；其尖峰是 VM 抖动（跨 run 位置漂移到 epoch 3/4/7/12/13/18/20/24/27 等、量级 50–150 ns、**绝无 ~4.7μs 量级**）。措辞结论不变：「不移动环内已有数据（O(1) 池切换）」，但 resize 本身 ~20μs 如实写。
26. `ovf_scan_max` 已移进规则 A（只在 `ovf_cnt >= ovf_thresh` 时扫，稳态/规则 B 零扫描）：稳态 `check_ns` 真实成本 **~3.5μs → ~0.3μs**（check_ns 列 ~7–8μs → ~4μs≈底噪）。`S_trace` 的 `L_ovf` 列语义随之收窄：非规则 A 纪元填 0（emergency 残留不再扫）。
27. **hit/miss 曲线机制**：SR 批量重传下，一次 NAK 重传 `[first_lost, first_lost+delay]` 窗口内**全部**丢包，其年龄 ~ Uniform[0, delay]；年龄 < N 才命中 ⇒ `hit_rate(delay) ≈ min(N/delay, 1)`。故 delay=2N 的 **49.3% 正确且预期**，**不是** pend_cap 截断（2%×20480≈410 条 << cap 4096，pend_overflow≈0）。若论文要「delay≤N 全命中、>N 归零」的干净台阶，需另测「单包在丢后 delay 纪元仍可查」的可取回率（与 SR 批量命中率是两种度量）。
28. `ovf_thresh` 记作 **τ_o**：绝对条数阈值（不依赖 N、不依赖 ovf_cap），「扩容灵敏度旋钮」（越小越灵敏，靠 k_dwell 滞后防误触）。可选补充实验：τ_o ∈ {128,256,512} 敏感性。

## I. Step 9 落定（本轮裁决）

29. **三种 *_bounded 淘汰一律 FIFO、不用 LRU**：机制淘汰 = 环回绕 = 最早进入者被覆盖，而 retrieve 是「取包不删」（不改变驻留）⇒ 基线用 LRU 会引入策略差异、把对照搞脏。tree_bounded 原「淘汰最小 key」不是缓存语义（乱序到达会踢掉与本次插入无关的包），已改 FIFO（avl_delete_key 物理摘除 + 插入序环，零 payload 拷贝）。**顺序 PSN 到达下三者等价**（位置淘汰 ≡ 插入序 FIFO ≡ 淘汰最小 PSN）；exp1a 主 store 序列为顺序 PSN ⇒ 主结果不受策略选择影响。chained_hash_bounded 保持「位置淘汰」= dynblock Φ 的最严格镜像（不改）。
30. **exp1a store 批量 B=1024（否决 128）**：读钟底噪 ~4μs/次，B=128 摊 62.5ns 偏移 ≈ 最快方法(~90ns)的 70%；B=1024 摊 7.8ns（≤10%）。CFG_E1A_TIMED_OPS=500000（池化 2441 点、p99≈24 点）。新增「空批地板」测量（同循环结构 + 同一对 rdtsc，floor 写进 CSV），把「机器读钟贵」从缺点变成被显式处理过的已知量。**（本条取代 §B-10 / §F-20 的 B=128）**
31. **n_cmp（每次取包 PSN 比较次数）为第三项指标，折进 3.2**：fifo O(N)、hash O(1)、tree O(log N)、dynblock 0（Φ 纯算术无搜索）。取包模式须写清（顺序/随机幸存者），否则 50.5 不可复现；与对照 E index_only 同口径（都计「比较」不计「运算」）。n_cmp CSV 随 exp1b 出。

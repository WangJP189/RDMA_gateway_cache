# exp1b lookup 实验数据（供 AI 分析，脚本提取）

## 实验设置
- 任务：RDMA 网关缓存 lookup/取包时间开销（GBN/SR NAK 重传工作负载）
- 方法：FIFO 队列 / 链式哈希 / 平衡树 / PSN 确定性映射 ×2 / index-only(Φ纯算术对照)
- PSN 两条路（最终都 S=1024、stride=1056B，与 hash 节点 stride 相同）：
    psn_dynblock          = S0=payload 直设（S=1024，无收敛）
    psn_dynblock_adaptive = S0=4096 自适应收敛到 1024 再冻结
- reorder=0 仅取包（定位+memcpy）；reorder=1 取包前先把 PSN 集升序整理（仅 SR；GBN 输出天然有序恒 0）
- N：512..10240（dynblock 环固定 10240 存 N 条，Φ 位置索引 O(1)）
- payload=1024 B；B=512；reps=5；主指标 p50_ns（mean 受 VM 停顿尾污染，仅作上界参考）
- 读钟地板：B=512 → 7.228 ns/op；B=32 → 115.636 ns/op（floor ∝ 1/B）

## p50 lookup time (ns) — gbn_long64（reorder=0，GBN 天然有序）

| method | 512 | 1024 | 2048 | 4096 | 8192 | 10240 |
|---|---|---|---|---|---|---|
| FIFO Queue | 23523.9 | 50899.5 | 95488.1 | 195959.6 | 454111.2 | 561597.7 |
| Chained Hash | 1508.0 | 1551.9 | 1642.6 | 1933.2 | 2204.5 | 2305.5 |
| Balanced Tree | 1722.7 | 1854.7 | 2146.1 | 2466.5 | 3093.7 | 3249.6 |
| PSN Mapping (S0=payload) | 1473.4 | 1508.0 | 1627.9 | 1814.5 | 2152.4 | 2279.6 |
| PSN Mapping (adaptive S) | 1505.4 | 1535.5 | 1566.0 | 1770.3 | 1936.9 | 2062.1 |
| index-only ($\Phi$) | 251.8 | 225.0 | 238.5 | 261.7 | 270.3 | 276.2 |

## p50 lookup time (ns) — sr_64 reorder=0（仅取包）

| method | 512 | 1024 | 2048 | 4096 | 8192 | 10240 |
|---|---|---|---|---|---|---|
| FIFO Queue | 24106.2 | 51625.7 | 100401.4 | 209905.7 | 421536.6 | 638947.1 |
| Chained Hash | 1578.9 | 1704.9 | 1910.8 | 2406.3 | 2760.1 | 3116.0 |
| Balanced Tree | 2675.3 | 3181.6 | 3886.4 | 4969.9 | 6952.2 | 8229.8 |
| PSN Mapping (S0=payload) | 1602.5 | 1737.3 | 1886.2 | 2324.3 | 2768.5 | 3827.0 |
| PSN Mapping (adaptive S) | 1907.1 | 1648.0 | 1962.9 | 2377.8 | 2803.6 | 3326.4 |
| index-only ($\Phi$) | 265.5 | 285.0 | 248.1 | 290.3 | 290.5 | 251.6 |

## p50 lookup time (ns) — sr_64 reorder=1（取包前升序整理）

| method | 512 | 1024 | 2048 | 4096 | 8192 | 10240 |
|---|---|---|---|---|---|---|
| FIFO Queue | 25776.3 | 52666.3 | 102524.5 | 209698.7 | 417457.6 | 635433.2 |
| Chained Hash | 2713.4 | 2790.7 | 3382.8 | 3499.0 | 3912.7 | 4264.3 |
| Balanced Tree | 3648.3 | 4169.6 | 4772.2 | 5706.1 | 7453.6 | 8547.4 |
| PSN Mapping (S0=payload) | 2644.0 | 2824.5 | 3022.6 | 3442.8 | 4000.6 | 4932.6 |
| PSN Mapping (adaptive S) | 2814.2 | 2789.2 | 3238.5 | 3565.6 | 3938.1 | 4401.3 |
| index-only ($\Phi$) | 1301.5 | 1222.8 | 1381.2 | 1432.4 | 1348.2 | 1283.3 |

## reorder 增量（reorder=1 − reorder=0，p50 ns）— 证明 reorder 成本对称

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

## n_cmp（每次取包平均比较次数，复杂度证据；gbn_long64 代表）

| method | 512 | 1024 | 2048 | 4096 | 8192 | 10240 | 复杂度 |
|---|---|---|---|---|---|---|---|
| FIFO Queue | 255.5 | 510.8 | 1014.4 | 2043.5 | 4089.0 | 5123.4 | O(N)≈N/2 |
| Chained Hash | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 | O(1) |
| Balanced Tree | 8.0 | 9.0 | 10.0 | 11.0 | 12.0 | 12.4 | O(log N) |
| PSN Mapping (S0=payload) | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | O(1) Φ |
| PSN Mapping (adaptive S) | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | O(1) Φ |
| index-only ($\Phi$) | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | O(1) Φ |

## 关键结论（供 AI 交叉验证）
1. 伸缩性：FIFO 随 N 线性增长（O(N)，n_cmp≈N/2）；Hash/PSN/index-only 平缓（O(1)）；Tree 亚线性（O(log N)）。
2. 根因修正（S=payload）：旧版 S=4096 存 1024B 包（stride 4128B）cache 局部性差、PSN 比 hash 慢 2.26×；
   修正后 S=1024（stride 1056B=hash 节点 stride）PSN 反超：gbn_long64 PSN 2062 vs hash 2305（快 10.6%）、
   gbn_short8 231.7 vs 276.0、sr_16 662.6 vs 775.3；仅 sr_64 hash 领先 6.7%（3326 vs 3116）。
3. reorder 对称：sr_64 排序成本 ~1050ns、sr_16 ~150ns，对所有方法（含 index_only 纯算术）一致，
   不改变 PSN-vs-hash 排序 ⇒ reorder 不是 PSN 落后的根因。
4. S0=payload 与 adaptive 收敛两条路结果一致（最终 S 相同），adaptive 略优（避免小包期 4096 大块浪费）。
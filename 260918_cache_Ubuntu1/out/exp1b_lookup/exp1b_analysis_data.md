# exp1b lookup 实验数据（供 AI 分析，脚本提取）

## 实验设置
- 任务：RDMA 网关缓存 lookup/取包时间开销（GBN/SR NAK 重传工作负载）
- 方法：FIFO 队列 / 链式哈希 / 平衡树 / PSN 确定性映射 ×2 / index-only(Φ纯算术对照)
- PSN 两条路（最终都 S=1024、stride=1056B，与 hash 节点 stride 相同）：
    psn_dynblock          = S0=payload 直设（S=1024，无收敛）
    psn_dynblock_adaptive = S0=4096 自适应收敛到 1024 再冻结
- reorder=0 仅取包（定位+memcpy）；reorder=1 取包前先把 PSN 集升序整理（仅 SR；GBN 输出天然有序恒 0）
- N：128..4096 + 锚点 5120（非 2 的幂；dynblock 环长=N，Φ=psn%N 位置索引 O(1)）
- payload=1024 B；B=512；reps=5；主指标 p50_ns（实机尾部干净，p50 稳健）
- 读钟地板：B=512 → 0.196 ns/op；B=32 → 0.283 ns/op（floor ∝ 1/B）

## p50 lookup time (ns) — gbn_long64（reorder=0，GBN 天然有序）

| method | 128 | 256 | 512 | 1024 | 2048 | 4096 | 5120 |
|---|---|---|---|---|---|---|---|
| FIFO Queue | 4459.3 | 8243.9 | 16111.8 | 32688.6 | 64949.3 | 130083.6 | 174082.3 |
| Chained Hash | 995.2 | 1013.0 | 1015.9 | 1017.1 | 1083.2 | 1214.1 | 1244.2 |
| Balanced Tree | 1061.1 | 1090.0 | 1142.5 | 1205.0 | 1336.7 | 1576.1 | 1644.1 |
| PSN Mapping (S0=payload) | 971.6 | 972.2 | 972.2 | 974.5 | 1010.3 | 1131.6 | 1148.2 |
| PSN Mapping (adaptive S) | 973.8 | 973.1 | 971.6 | 972.8 | 1008.6 | 1126.8 | 1150.4 |
| index-only ($\Phi$) | 192.6 | 192.6 | 192.6 | 192.6 | 192.6 | 192.6 | 192.6 |

## p50 lookup time (ns) — sr_64 reorder=0（仅取包）

| method | 128 | 256 | 512 | 1024 | 2048 | 4096 | 5120 |
|---|---|---|---|---|---|---|---|
| FIFO Queue | 4621.0 | 8616.1 | 16812.7 | 35024.1 | 71067.4 | 139543.7 | 175430.5 |
| Chained Hash | 1013.4 | 1024.8 | 1047.9 | 1065.3 | 1213.0 | 1495.8 | 1575.4 |
| Balanced Tree | 1249.6 | 1450.5 | 1683.4 | 1951.8 | 2381.0 | 3013.9 | 3331.7 |
| PSN Mapping (S0=payload) | 1009.4 | 1026.0 | 1058.9 | 1081.3 | 1187.9 | 1450.2 | 1652.8 |
| PSN Mapping (adaptive S) | 1007.6 | 1026.8 | 1059.3 | 1080.5 | 1180.4 | 1450.1 | 1683.7 |
| index-only ($\Phi$) | 193.2 | 191.8 | 191.8 | 191.8 | 191.8 | 191.8 | 191.9 |

## p50 lookup time (ns) — sr_64 reorder=1（取包前升序整理）

| method | 128 | 256 | 512 | 1024 | 2048 | 4096 | 5120 |
|---|---|---|---|---|---|---|---|
| FIFO Queue | 5329.3 | 9336.0 | 17482.8 | 35726.2 | 71858.7 | 140568.5 | 176352.6 |
| Chained Hash | 1699.6 | 1716.1 | 1742.9 | 1765.7 | 1910.8 | 2208.5 | 2365.3 |
| Balanced Tree | 1940.9 | 2120.0 | 2359.5 | 2592.5 | 2949.9 | 3515.0 | 3746.9 |
| PSN Mapping (S0=payload) | 1686.8 | 1714.1 | 1752.3 | 1781.1 | 1883.9 | 2157.8 | 2347.1 |
| PSN Mapping (adaptive S) | 1687.7 | 1715.8 | 1752.2 | 1781.9 | 1889.8 | 2152.8 | 2375.1 |
| index-only ($\Phi$) | 903.1 | 908.8 | 911.5 | 906.7 | 907.6 | 909.4 | 911.5 |

## reorder 增量（reorder=1 − reorder=0，p50 ns）— 证明 reorder 成本对称

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

## n_cmp（每次取包平均比较次数，复杂度证据；gbn_long64 代表）

| method | 128 | 256 | 512 | 1024 | 2048 | 4096 | 5120 | 复杂度 |
|---|---|---|---|---|---|---|---|---|
| FIFO Queue | 64.7 | 128.2 | 255.5 | 510.8 | 1014.4 | 2043.5 | 2556.5 | O(N)≈N/2 |
| Chained Hash | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 | 1.0 | O(1) |
| Balanced Tree | 6.0 | 7.0 | 8.0 | 9.0 | 10.0 | 11.0 | 11.4 | O(log N) |
| PSN Mapping (S0=payload) | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | O(1) Φ |
| PSN Mapping (adaptive S) | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | O(1) Φ |
| index-only ($\Phi$) | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | O(1) Φ |

## 关键结论（脚本计算，禁手抄）
1. 伸缩性（gbn_long64, reorder=0）：FIFO p50 从 N=128 的 4459 ns 涨到 N=5120 的 174082 ns（×39），n_cmp≈N/2（N=5120 时 2556.5）；Hash n_cmp=1.0 恒定、PSN/index_only n_cmp=0（Φ 纯算术）——O(N) vs O(1) 直接证据。
2. PSN(adaptive S) vs Chained Hash @ N=4096（p50 ns；PSN 快为正 %）：
   GBN-long64：PSN 1126.8 vs hash 1214.1（PSN +7.2%）
   GBN-short8：PSN 124.8 vs hash 140.2（PSN +11.0%）
   SR-16：PSN 305.1 vs hash 322.9（PSN +5.5%）
   SR-64：PSN 1450.1 vs hash 1495.8（PSN +3.1%）
3. reorder 对称（sr_16 @ N=5120）：各方法 reorder=1−0 增量 = 49 / 100 / 98 / 106 / 103 / 110 ns（index_only 纯算术也一致）⇒ reorder 成本与缓存结构无关。
4. reorder 对称（sr_64 @ N=5120）：各方法 reorder=1−0 增量 = 922 / 790 / 415 / 694 / 691 / 720 ns（index_only 纯算术也一致）⇒ reorder 成本与缓存结构无关。
5. S0=payload 与 adaptive 收敛两条路结果一致（最终 S=1024）：gbn_long64 @ N=4096 二者 p50 1131.6 / 1126.8 ns。
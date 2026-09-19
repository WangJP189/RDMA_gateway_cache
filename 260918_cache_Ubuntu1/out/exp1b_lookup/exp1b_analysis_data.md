# exp1b lookup 实验数据（供 AI 分析，脚本提取）

## 实验设置
- 任务：RDMA 网关缓存 lookup/取包时间开销（GBN/SR NAK 重传工作负载）
- 方法：FIFO 队列 / 链式哈希 / 平衡树 / PSN 确定性映射 ×2 / index-only(Φ纯算术对照)
- PSN 两条路（最终都 S=1024、stride=1056B，与 hash 节点 stride 相同）：
    psn_dynblock          = S0=payload 直设（S=1024，无收敛）
    psn_dynblock_adaptive = S0=4096 自适应收敛到 1024 再冻结
- 交付契约（第 4B）：retrieve_set 统一按 PSN 升序交付——fifo/hash qsort、tree 中序、dynblock 扫槽；
  retrieve_set 计时 = 定位 + memcpy + 升序交付全过程（各方法升序交付成本已含在内）
- N：128..4096 + 锚点 5120（非 2 的幂；dynblock 环长=N，Φ=psn%N 位置索引 O(1)）
- payload=1024 B；B=512；reps=5；主指标 p50_ns（实机尾部干净，p50 稳健）
- 读钟地板：B=512 → 0.196 ns/op；B=32 → 0.283 ns/op（floor ∝ 1/B）

## p50 lookup time (ns) — gbn_long64（retrieve_range，天然升序）

| method | 128 | 256 | 512 | 1024 | 2048 | 4096 | 5120 |
|---|---|---|---|---|---|---|---|
| FIFO Queue | 4462.5 | 8397.4 | 16508.7 | 35186.3 | 69513.6 | 139020.0 | 172974.6 |
| Chained Hash | 1016.3 | 1008.6 | 1010.1 | 1013.0 | 1065.7 | 1187.6 | 1216.5 |
| Balanced Tree | 1070.5 | 1102.0 | 1142.3 | 1188.5 | 1300.2 | 1521.8 | 1577.6 |
| PSN Mapping (S0=payload) | 992.6 | 992.3 | 995.7 | 996.4 | 1026.7 | 1104.0 | 1132.3 |
| PSN Mapping (adaptive S) | 994.9 | 991.8 | 994.9 | 996.9 | 1024.8 | 1101.7 | 1130.5 |
| index-only ($\Phi$) | 181.1 | 181.2 | 181.8 | 181.2 | 181.3 | 181.1 | 181.3 |

## p50 lookup time (ns) — sr_64（retrieve_set，第 4B 升序交付）

| method | 128 | 256 | 512 | 1024 | 2048 | 4096 | 5120 |
|---|---|---|---|---|---|---|---|
| FIFO Queue | 6296.1 | 10038.6 | 18059.6 | 34489.6 | 67262.7 | 140474.0 | 163426.6 |
| Chained Hash | 2574.4 | 2617.9 | 2651.4 | 2677.3 | 2824.8 | 3046.6 | 3099.3 |
| Balanced Tree | 1745.8 | 3223.5 | 5388.5 | 7660.0 | 10078.7 | 14498.4 | 16370.1 |
| PSN Mapping (S0=payload) | 1391.5 | 2433.8 | 4109.1 | 6622.9 | 8666.8 | 12819.5 | 14846.1 |
| PSN Mapping (adaptive S) | 1388.7 | 2433.3 | 4100.6 | 6629.5 | 8656.6 | 12875.3 | 14885.5 |
| index-only ($\Phi$) | 1737.8 | 1736.7 | 1740.7 | 1740.9 | 1738.1 | 1737.4 | 1737.9 |

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
1. 伸缩性（gbn_long64）：FIFO p50 从 N=128 的 4462 ns 涨到 N=5120 的 172975 ns（×39），n_cmp≈N/2（N=5120 时 2556.5）；Hash n_cmp=1.0 恒定、PSN/index_only n_cmp=0（Φ 纯算术）——O(N) vs O(1) 直接证据。
2. PSN(adaptive S) vs Chained Hash @ N=4096（p50 ns；PSN 快为正 %）：
   GBN-long64：PSN 1101.7 vs hash 1187.6（PSN +7.2%）
   GBN-short8：PSN 92.2 vs hash 121.1（PSN +23.8%）
   SR-16：PSN 8542.6 vs hash 578.4（PSN -1377.0%）
   SR-64：PSN 12875.3 vs hash 3046.6（PSN -322.6%）
3. SR 升序交付（第 4B，sr_64 @ N=5120，retrieve_set p50 ns；fifo/hash 含 qsort、tree/dynblock 零排序）：
   FIFO Queue = 163426.6
   Chained Hash = 3099.3
   Balanced Tree = 16370.1
   PSN Mapping (S0=payload) = 14846.1
   PSN Mapping (adaptive S) = 14885.5
   index-only ($\Phi$) = 1737.9
4. S0=payload 与 adaptive 收敛两条路结果一致（最终 S=1024）：gbn_long64 @ N=4096 二者 p50 1104.0 / 1101.7 ns。
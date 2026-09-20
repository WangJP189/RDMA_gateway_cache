# exp1b lookup 实验数据（供 AI 分析，脚本提取）

## 实验设置
- 任务：RDMA 网关缓存 lookup/取包时间开销（GBN/SR NAK 重传工作负载）
- 方法：FIFO 队列 / 链式哈希 / 平衡树 / PSN 确定性映射 ×2 / 固定块对照 / index-only(Φ纯算术对照)
- PSN 两条路（最终都 S=1024、stride=1056B，与 hash 节点 stride 相同）：
    psn_dynblock          = S0=payload 直设（S=1024，无收敛）
    psn_dynblock_adaptive = S0=4096 自适应收敛到 1024 再冻结
    ring_fixed            = S=4096 固定（去弹性；与 exp1a 同名同义）
- 交付契约（第 4B）：retrieve_set 统一按 PSN 升序交付——fifo/hash 显式 sort_u32_asc、
  tree=tree_search_set（排序 + k×O(log N) 查找）、dynblock 位置映射扫槽（零排序）；
  retrieve_set 计时 = 定位 + memcpy + 升序交付全过程（各方法升序交付成本已含在内）
- 排序原语正交（sort_impl）：0=glibc qsort（主）/1=内联插入排序（xval）；只作用 SR 排序路径，GBN 两遍一致
- N：128..4096 + 锚点 5120（非 2 的幂；dynblock 环长=N，Φ=psn%N 位置索引 O(1)）
- payload=1024 B；B=512；reps=5；主指标 p50_ns（实机尾部干净，p50 稳健）
- 读钟地板：B=512 → 0.195 ns/op；B=32 → 0.256 ns/op（floor ∝ 1/B）

## p50 lookup time (ns) — gbn_long64（retrieve_range，天然升序；sort_impl=0 主）

| method | 128 | 256 | 512 | 1024 | 2048 | 4096 | 5120 |
|---|---|---|---|---|---|---|---|
| FIFO Queue | 4325.7 | 8393.9 | 16259.8 | 32907.1 | 64853.1 | 129702.7 | 162226.6 |
| Chained Hash | 1045.3 | 1059.4 | 1063.6 | 1063.0 | 1130.2 | 1264.6 | 1393.8 |
| Balanced Tree | 1105.9 | 1115.5 | 1149.0 | 1194.6 | 1321.6 | 1562.7 | 1638.0 |
| PSN Mapping (S0=payload) | 1025.4 | 1027.0 | 1029.3 | 1029.4 | 1061.3 | 1164.6 | 1185.3 |
| PSN Mapping (adaptive S) | 1026.1 | 1025.7 | 1029.2 | 1036.4 | 1058.6 | 1164.0 | 1186.2 |
| PSN fixed block (S=4096) | 1106.2 | 1103.6 | 1141.5 | 1358.0 | 1517.6 | 1629.1 | 1675.6 |
| index-only ($\Phi$) | 203.6 | 203.4 | 203.5 | 203.3 | 203.5 | 203.4 | 203.4 |

## p50 lookup time (ns) — sr_64（retrieve_set，第 4B 升序交付；sort_impl=0 主）

| method | 128 | 256 | 512 | 1024 | 2048 | 4096 | 5120 |
|---|---|---|---|---|---|---|---|
| FIFO Queue | 6315.7 | 10123.9 | 18327.8 | 36842.7 | 72177.3 | 141088.0 | 175854.1 |
| Chained Hash | 2645.4 | 2672.3 | 2697.7 | 2722.1 | 2890.5 | 3196.9 | 3274.0 |
| Balanced Tree | 2876.2 | 3080.4 | 3330.4 | 3589.5 | 3984.5 | 4633.5 | 4962.5 |
| PSN Mapping (S0=payload) | 1128.0 | 1144.3 | 1173.7 | 1200.5 | 1307.5 | 1584.9 | 1667.1 |
| PSN Mapping (adaptive S) | 1122.1 | 1143.6 | 1172.9 | 1198.0 | 1321.1 | 1584.7 | 1668.4 |
| PSN fixed block (S=4096) | 1176.8 | 1194.0 | 1288.5 | 1571.7 | 1785.8 | 1990.4 | 2142.5 |
| index-only ($\Phi$) | 1748.7 | 1747.4 | 1750.1 | 1750.0 | 1773.6 | 1748.4 | 1749.7 |

## n_cmp（每次取包平均比较次数，复杂度证据；gbn_long64 代表，sort_impl=0）

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

## 口径声明（第 35 条：固定重传报文数）
- 每批固定 K 个报文，K 在所有 N 下恒定（CSV pkts_per_op 列）：gbn_long64=64 / gbn_short8=8 / sr_16=16 / sr_64=64。
- GBN = K 个连续 PSN（retrieve_range）；SR = K 个互异 PSN（无放回，部分 Fisher–Yates，retrieve_set）。
- 原「有放回抽样」使 SR 实际搬运包数随 N 变（N=128≈50.6 → N=5120≈63.6，约 +26%），在 N 曲线混入假增长；本次改无放回已消除。
- main_metric = batch_p50_ns：每批固定 K 个报文的 retrieve 摊到单批的 p50（主/次图取 K=64）。

## 归因声明（SR-64 随 N 增长的来源）
- ring_bytes = N × stride(S)（stride=align16(S)=1024B；CSV ring_bytes 列）：N=128 → 128 KB，N=5120 → 5 MB。
- SR-64 p50 随 N 的增长归因于 64KB 连续 memcpy 的缓存局部性（源环 128 KB→5 MB 跨 L2→L3，SR 再叠加离散访问惩罚），非算法退化（index_only Φ 纯算术扁平佐证）。
1. 伸缩性（gbn_long64）：FIFO p50 从 N=128 的 4326 ns 涨到 N=5120 的 162227 ns（×38），n_cmp≈N/2（N=5120 时 2556.5）；Hash n_cmp=1.0 恒定、PSN/index_only n_cmp=0（Φ 纯算术）——O(N) vs O(1) 直接证据。
2. PSN(adaptive S) vs Chained Hash @ N=4096（p50 ns；PSN 快为正 %）：
   GBN-long64：PSN 1164.0 vs hash 1264.6（PSN +8.0%）
   GBN-short8：PSN 115.3 vs hash 122.2（PSN +5.6%）
   SR-16：PSN 390.4 vs hash 602.1（PSN +35.2%）
   SR-64：PSN 1584.7 vs hash 3196.9（PSN +50.4%）
3. SR 升序交付（第 4B，sr_64 @ N=5120，retrieve_set p50 ns；fifo/hash/tree 需排序检索、dynblock 零排序）：
   FIFO Queue = 175854.1
   Chained Hash = 3274.0
   Balanced Tree = 4962.5
   PSN Mapping (S0=payload) = 1667.1
   PSN Mapping (adaptive S) = 1668.4
   PSN fixed block (S=4096) = 2142.5
   index-only ($\Phi$) = 1749.7
4. S0=payload 与 adaptive 收敛两条路结果一致（最终 S=1024）：gbn_long64 @ N=4096 二者 p50 1164.6 / 1164.0 ns。
5. SR-64 自洽门（第 35 条）：PSN(adaptive) sr_64 p50 N=128→5120 = 1122→1668 ns（+49%）。
   对照：index_only（Φ 纯算术，无 memcpy）1749→1750（+0%，扁平 → 自洽）；gbn_long64（连续 64KB memcpy）1026→1186（+16%）。
   旧版（有放回抽样，含假增长）sr_64 增长 +74%（864→1501 ns）；本次固定 K 后为 +49%，压降 25 个百分点。
   门判定：index_only 平坦=是（增长 <5%）；增长 49% 明显小于旧版 74%。
   归因：剩余增长来自 64KB memcpy 缓存局部性（源环 128 KB→5 MB 跨 L2→L3），非算法退化（见上「归因声明」）。
6. 排序原语一致性：sr_64 @ N=5120 PSN(adaptive) 主(qsort) 1668.4 vs xval(插入排序) 1669.0 ns（差 0.60 ns，等价）。
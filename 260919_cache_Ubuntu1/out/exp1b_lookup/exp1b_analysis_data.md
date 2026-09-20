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
- 读钟地板：B=512 → 0.293 ns/op；B=32 → 0.375 ns/op（floor ∝ 1/B）

## p50 lookup time (ns) — gbn_long64（retrieve_range，天然升序；sort_impl=0 主）

| method | 128 | 256 | 512 | 1024 | 2048 | 4096 | 5120 |
|---|---|---|---|---|---|---|---|
| FIFO Queue | 4451.9 | 8228.7 | 16124.7 | 32828.7 | 64971.9 | 130042.1 | 162798.6 |
| Chained Hash | 1003.4 | 1015.8 | 1019.5 | 1031.5 | 1107.5 | 1333.0 | 1368.5 |
| Balanced Tree | 1067.8 | 1087.6 | 1133.6 | 1179.8 | 1303.3 | 1559.8 | 1625.1 |
| PSN Mapping (S0=payload) | 986.5 | 990.4 | 993.0 | 993.7 | 1045.7 | 1224.4 | 1262.8 |
| PSN Mapping (adaptive S) | 986.6 | 990.4 | 993.3 | 995.3 | 1035.1 | 1243.2 | 1291.7 |
| PSN fixed block (S=4096) | 1042.1 | 1040.2 | 1135.5 | 1485.9 | 1791.6 | 1931.3 | 1988.8 |
| index-only ($\Phi$) | 181.5 | 181.5 | 181.2 | 181.3 | 181.5 | 181.3 | 181.5 |

## p50 lookup time (ns) — sr_64（retrieve_set，第 4B 升序交付；sort_impl=0 主）

| method | 128 | 256 | 512 | 1024 | 2048 | 4096 | 5120 |
|---|---|---|---|---|---|---|---|
| FIFO Queue | 6197.3 | 10192.5 | 18365.7 | 37085.6 | 72342.2 | 141122.6 | 175888.9 |
| Chained Hash | 2575.3 | 2587.9 | 2614.8 | 2666.4 | 2804.1 | 3149.8 | 3083.6 |
| Balanced Tree | 2833.8 | 2943.4 | 3220.0 | 3419.4 | 3752.6 | 4321.9 | 4626.6 |
| PSN Mapping (S0=payload) | 864.5 | 959.7 | 1033.9 | 1080.9 | 1220.5 | 1558.8 | 1512.1 |
| PSN Mapping (adaptive S) | 864.2 | 960.7 | 1034.7 | 1082.3 | 1215.1 | 1579.6 | 1500.6 |
| PSN fixed block (S=4096) | 852.2 | 991.6 | 1127.9 | 1363.5 | 1812.2 | 1992.4 | 1870.7 |
| index-only ($\Phi$) | 1754.9 | 1749.9 | 1751.0 | 1751.7 | 1753.1 | 1758.2 | 1778.5 |

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
1. 伸缩性（gbn_long64）：FIFO p50 从 N=128 的 4452 ns 涨到 N=5120 的 162799 ns（×37），n_cmp≈N/2（N=5120 时 2556.5）；Hash n_cmp=1.0 恒定、PSN/index_only n_cmp=0（Φ 纯算术）——O(N) vs O(1) 直接证据。
2. PSN(adaptive S) vs Chained Hash @ N=4096（p50 ns；PSN 快为正 %）：
   GBN-long64：PSN 1243.2 vs hash 1333.0（PSN +6.7%）
   GBN-short8：PSN 107.1 vs hash 124.9（PSN +14.2%）
   SR-16：PSN 352.7 vs hash 567.7（PSN +37.9%）
   SR-64：PSN 1579.6 vs hash 3149.8（PSN +49.9%）
3. SR 升序交付（第 4B，sr_64 @ N=5120，retrieve_set p50 ns；fifo/hash/tree 需排序检索、dynblock 零排序）：
   FIFO Queue = 175888.9
   Chained Hash = 3083.6
   Balanced Tree = 4626.6
   PSN Mapping (S0=payload) = 1512.1
   PSN Mapping (adaptive S) = 1500.6
   PSN fixed block (S=4096) = 1870.7
   index-only ($\Phi$) = 1778.5
4. S0=payload 与 adaptive 收敛两条路结果一致（最终 S=1024）：gbn_long64 @ N=4096 二者 p50 1224.4 / 1243.2 ns。
5. SR-64 伸缩性门（门槛 +<20%）：PSN(adaptive) sr_64 p50 N=128→5120 = 864→1501 ns（+74%）。
   对照：index_only（Φ 纯算术，无 memcpy）1755→1778（+1%，扁平）；gbn_long64（连续 64KB memcpy）987→1292（+31%）。
   诊断：增长来自 64KB memcpy 的缓存局部性（源环 128KB→5MB 跨 L2→L3），SR-64 再叠加离散访问惩罚；
   位图扫描 O(span/64) 可忽略（index_only 扁平佐证），非算法退化。旧版（位图快路径前）sr_64 增长 +972%（1389→14886），
   位图快路径把增长压降 13.2×、sr_64@5120 降至 1501 ns（旧版 14886）。此门未过，保留现状并如实报告。
6. 排序原语一致性：sr_64 @ N=5120 PSN(adaptive) 主(qsort) 1500.6 vs xval(插入排序) 1497.6 ns（差 2.99 ns，等价）。
# Pareto — 脚本提取（store_summary.csv × lookup_summary.csv）

> x=store p50 @ payload=1024 pooled B=2048（exp1a）；y=SR-64 retrieve_set p50 @ N=4096 sort_impl=0（exp1b）。


| method | store p50 (ns) | SR-64 lookup p50 (ns) |
|---|---|---|
| FIFO Queue | 23.191 | 141122.6 |
| Chained Hash | 23.198 | 3149.8 |
| Balanced Tree | 53.512 | 4321.9 |
| PSN Mapping | 22.851 | 1579.6 |
| index-only ($\Phi$) | 1.099 | 1758.2 |

## 主导关系（脚本计算）
- PSN Mapping 同时优于 FIFO Queue：store 22.851 < 23.191 ns 且 lookup 1579.6 < 141122.6 ns
- PSN Mapping 同时优于 Chained Hash：store 22.851 < 23.198 ns 且 lookup 1579.6 < 3149.8 ns
- PSN Mapping 同时优于 Balanced Tree：store 22.851 < 53.512 ns 且 lookup 1579.6 < 4321.9 ns
- index-only（Φ 地板）：store 1.099 ns（无 payload 拷贝的不可约成本），但 SR 交付仍走排序路径，lookup 1758.2 ns 略高于 PSN 位图快路径 1579.6 ns

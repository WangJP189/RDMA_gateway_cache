# exp1a store — 脚本提取（store_summary.csv）

## B=8192 主矩阵 · p50 (ns) — 主指标

| method | 64 B | 1024 B | 4096 B |
|---|---|---|---|
| FIFO Queue | 5.042 | 31.779 | 430.020 |
| Chained Hash | 6.825 | 33.842 | 424.965 |
| Balanced Tree | 78.000 | 95.739 | 522.401 |
| PSN Mapping (adaptive S) | 9.449 | 31.266 | 400.378 |
| PSN Mapping (fixed S=4096) | 12.001 | 53.327 | 407.690 |
| index-only ($\Phi$) | 2.234 | 2.075 | 2.014 |

## B=8192 主矩阵 · p90 (ns) — 次指标

| method | 64 B | 1024 B | 4096 B |
|---|---|---|---|
| FIFO Queue | 6.153 | 38.799 | 472.139 |
| Chained Hash | 9.376 | 43.597 | 463.056 |
| Balanced Tree | 87.083 | 110.157 | 582.834 |
| PSN Mapping (adaptive S) | 12.440 | 39.751 | 450.677 |
| PSN Mapping (fixed S=4096) | 19.045 | 81.980 | 455.731 |
| index-only ($\Phi$) | 2.295 | 3.968 | 4.603 |

## B=1024 交叉验证 · p50 (ns) — 排序/差距一致性

| method | 64 B | 1024 B | 4096 B |
|---|---|---|---|
| FIFO Queue | 8.790 | 33.988 | 442.338 |
| Chained Hash | 10.157 | 33.891 | 463.728 |
| Balanced Tree | 76.767 | 91.026 | 547.819 |
| PSN Mapping (adaptive S) | 12.111 | 33.305 | 411.475 |
| PSN Mapping (fixed S=4096) | 14.943 | 58.112 | 414.991 |
| index-only ($\Phi$) | 5.469 | 6.056 | 5.567 |

## 收敛后 S（dynblock）

- PSN Mapping (adaptive S): 64 B→S=128, 1024 B→S=1024, 4096 B→S=4096
- PSN Mapping (fixed S=4096): 64 B→S=4096, 1024 B→S=4096, 4096 B→S=4096

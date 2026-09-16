# exp1a store — 脚本提取（store_summary.csv）

## B=8192 主矩阵 · p50 (ns) — 主指标

| method | 64 B | 1024 B | 4096 B |
|---|---|---|---|
| FIFO (bounded) | 5.042 | 31.779 | 430.020 |
| chained hash (bounded) | 6.825 | 33.842 | 424.965 |
| balanced tree (bounded) | 78.000 | 95.739 | 522.401 |
| dynblock (adaptive S) | 9.449 | 31.266 | 400.378 |
| dynblock (fixed S=4096) | 12.001 | 53.327 | 407.690 |
| index-only ($\Phi$) | 2.234 | 2.075 | 2.014 |

## B=8192 主矩阵 · p90 (ns) — 次指标

| method | 64 B | 1024 B | 4096 B |
|---|---|---|---|
| FIFO (bounded) | 6.153 | 38.799 | 472.139 |
| chained hash (bounded) | 9.376 | 43.597 | 463.056 |
| balanced tree (bounded) | 87.083 | 110.157 | 582.834 |
| dynblock (adaptive S) | 12.440 | 39.751 | 450.677 |
| dynblock (fixed S=4096) | 19.045 | 81.980 | 455.731 |
| index-only ($\Phi$) | 2.295 | 3.968 | 4.603 |

## B=1024 交叉验证 · p50 (ns) — 排序/差距一致性

| method | 64 B | 1024 B | 4096 B |
|---|---|---|---|
| FIFO (bounded) | 8.790 | 33.988 | 442.338 |
| chained hash (bounded) | 10.157 | 33.891 | 463.728 |
| balanced tree (bounded) | 76.767 | 91.026 | 547.819 |
| dynblock (adaptive S) | 12.111 | 33.305 | 411.475 |
| dynblock (fixed S=4096) | 14.943 | 58.112 | 414.991 |
| index-only ($\Phi$) | 5.469 | 6.056 | 5.567 |

## 收敛后 S（dynblock）

- dynblock (adaptive S): 64 B→S=128, 1024 B→S=1024, 4096 B→S=4096
- dynblock (fixed S=4096): 64 B→S=4096, 1024 B→S=4096, 4096 B→S=4096

# exp2 space — 脚本提取（space_summary.csv）

> N=4096 满窗；utilization = payload_bytes / allocated_bytes（cache_footprint_bytes，sizeof 实测）。


## 空间利用率 (%) — 5 方法 × 5 档 MTU

| method | 256 B | 512 B | 1024 B | 2048 B | 4096 B |
|---|---|---|---|---|---|
| FIFO Queue | 88.89 | 94.12 | 96.97 | 98.46 | 99.22 |
| Chained Hash | 78.05 | 87.67 | 93.43 | 96.60 | 98.27 |
| Balanced Tree | 80.00 | 88.89 | 94.12 | 96.97 | 98.46 |
| PSN Mapping (adaptive S) | 84.43 | 91.56 | 95.59 | 97.75 | 98.86 |
| PSN Mapping (fixed S=4096) | 6.18 | 12.36 | 24.72 | 49.43 | 98.86 |

## 分配总量 allocated_bytes (B) — 满窗 N=4096

| method | 256 B | 512 B | 1024 B | 2048 B | 4096 B |
|---|---|---|---|---|---|
| FIFO Queue | 1179648 | 2228224 | 4325376 | 8519680 | 16908288 |
| Chained Hash | 1343488 | 2392064 | 4489216 | 8683520 | 17072128 |
| Balanced Tree | 1310720 | 2359296 | 4456448 | 8650752 | 17039360 |
| PSN Mapping (adaptive S) | 1241904 | 2290480 | 4387632 | 8581936 | 16970544 |
| PSN Mapping (fixed S=4096) | 16970544 | 16970544 | 16970544 | 16970544 | 16970544 |

## block_S（dynblock 槽大小；非 dynblock=0）

- PSN Mapping (adaptive S): 256 B→S=256, 512 B→S=512, 1024 B→S=1024, 2048 B→S=2048, 4096 B→S=4096
- PSN Mapping (fixed S=4096): 256 B→S=4096, 512 B→S=4096, 1024 B→S=4096, 2048 B→S=4096, 4096 B→S=4096

## 关键结论（脚本计算，供正文）

- FIFO（最优基线）利用率区间：88.89%–99.22%
- 256 B：FIFO−PSN(弹性)=4.46 pp，FIFO−PSN(fixed)=82.71 pp
- 512 B：FIFO−PSN(弹性)=2.56 pp，FIFO−PSN(fixed)=81.76 pp
- 1024 B：FIFO−PSN(弹性)=1.38 pp，FIFO−PSN(fixed)=72.25 pp
- 2048 B：FIFO−PSN(弹性)=0.71 pp，FIFO−PSN(fixed)=49.03 pp
- 4096 B：FIFO−PSN(弹性)=0.36 pp，FIFO−PSN(fixed)=0.36 pp
- 弹性最大劣势点：256 B，差 4.46 pp（<=5% 断言成立）
- fixed 最大塌陷点：256 B，利用率 6.18%（vs FIFO 88.89%）——弹性槽大小机制的价值所在

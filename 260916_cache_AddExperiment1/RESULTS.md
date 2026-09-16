# RESULTS — 实验结果（数字由脚本从 store_summary.csv 提取）

> 生成：`python3 paper_figures/plot_exp1a_store.py`；表内数字禁止手抄。


## exp1a — store time cost（存时间开销）


**图：** `paper_figures/fig_exp1a_store.pdf`（同目录 `fig_exp1a_store.png` @300dpi）。


**图注（名词性短语）：** Store time cost versus packet size.


图内注：N=10240, batch B=8192, R=5 runs；单位 ns。横轴 packet size（64/1024/4096 B）、纵轴 store time cost（ns），双对数。主指标 p50，次指标 p90（p99 仅 CSV 留痕，~1% 批次遭 ~2ms VM 调度停顿，不可用）。序列：FIFO/hash/tree 三个 bounded 基线、dynblock（自适应收敛后冻结）、dynblock（fixed S=4096，虚线对照）、index-only（Φ 不可约成本）。


### 主矩阵 B=8192 · p50 (ns)

| method | 64 B | 1024 B | 4096 B |
|---|---|---|---|
| FIFO (bounded) | 5.042 | 31.779 | 430.020 |
| chained hash (bounded) | 6.825 | 33.842 | 424.965 |
| balanced tree (bounded) | 78.000 | 95.739 | 522.401 |
| dynblock (adaptive S) | 9.449 | 31.266 | 400.378 |
| dynblock (fixed S=4096) | 12.001 | 53.327 | 407.690 |
| index-only ($\Phi$) | 2.234 | 2.075 | 2.014 |


### 主矩阵 B=8192 · p90 (ns)

| method | 64 B | 1024 B | 4096 B |
|---|---|---|---|
| FIFO (bounded) | 6.153 | 38.799 | 472.139 |
| chained hash (bounded) | 9.376 | 43.597 | 463.056 |
| balanced tree (bounded) | 87.083 | 110.157 | 582.834 |
| dynblock (adaptive S) | 12.440 | 39.751 | 450.677 |
| dynblock (fixed S=4096) | 19.045 | 81.980 | 455.731 |
| index-only ($\Phi$) | 2.295 | 3.968 | 4.603 |


### B=1024 交叉验证 · p50 (ns) — 排序/差距一致性

| method | 64 B | 1024 B | 4096 B |
|---|---|---|---|
| FIFO (bounded) | 8.790 | 33.988 | 442.338 |
| chained hash (bounded) | 10.157 | 33.891 | 463.728 |
| balanced tree (bounded) | 76.767 | 91.026 | 547.819 |
| dynblock (adaptive S) | 12.111 | 33.305 | 411.475 |
| dynblock (fixed S=4096) | 14.943 | 58.112 | 414.991 |
| index-only ($\Phi$) | 5.469 | 6.056 | 5.567 |


### 收敛后 S（dynblock）

- dynblock (adaptive S): 64 B→S=128, 1024 B→S=1024, 4096 B→S=4096
- dynblock (fixed S=4096): 64 B→S=4096, 1024 B→S=4096, 4096 B→S=4096


---


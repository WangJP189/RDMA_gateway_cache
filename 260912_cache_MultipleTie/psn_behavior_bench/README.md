# 实验三：存/取行为（任务 A–E）

独立复刻「档位标记数组 + 分档环形数组」机制，验证存/取行为的正确性与延迟特性。

```bash
make
./behavior_bench -o ./out          # 默认 N=10240, L=1024, B=128, R=200, 丢包率 0~10%
python3 plot_behavior.py --dir ./out --out ./out
```

## 任务

| 任务 | 内容 | 产出 csv |
|------|------|---------|
| A | 顺序存储 | `A_store_sequential.csv` |
| B | 乱序存储 → 顺序取（零整理） | `B_store_out_of_order.csv`、`B_retrieve_ordered.csv` |
| C | 随机丢包取回（丢包率 1%） | `C_retrieve_found.csv`、`C_retrieve_lost.csv` |
| D | 突发丢包取回（连续 512 包） | 打印 + 判定准确率 |
| E | 丢包率扫描（0%~10%） | `E_loss_sweep.csv` |

## 选项

`-n` 环长 · `-l` payload 大小 · `-B` 批量计时粒度 · `-R` 批数 · `-p` 丢包率
· `-d` 突发丢包长度 · `-s` 随机种子 · `-o` 输出目录 · `-h` 帮助

延迟用 `rdtsc` 批量（B=128 包）计时并标定 TSC→ns，避免 VM 的 `rdtscp/lfence` VM-exit 噪声。
随机序列用 xorshift32 固定种子，可复现。

图：`cdf_store.png/pdf`、`cdf_retrieve.png/pdf`、`loss_sweep.png/pdf`、`correctness.png/pdf`。

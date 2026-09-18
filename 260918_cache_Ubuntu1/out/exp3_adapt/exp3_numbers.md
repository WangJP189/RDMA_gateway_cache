# exp3 弹性消融 — 脚本提取（S_trace.csv / resize_events.csv / summary.csv / resolved_config.json）

> 两条件同一相位序列（5 档 MTU 跳变 + 256↔4096 ×16 振荡），仅弹性旋钮不同。


## 弹性旋钮

| 旋钮 | ablation | slow 对照 |
|---|---|---|
| k_dwell（同向纪元数） | 1 | 2 |
| ovf_thresh（阈值扩条数） | 1 | 256 |
| j_quiet（缩前安静纪元数） | 1 | 4 |
| hist_decay（溢出史清空纪元数） | 2 | 8 |

## 五档跳变响应延迟（MTU 跳变后 S 首次命中目标的纪元数）

| 跳变 | ablation | slow 对照 |
|---|---|---|
| 4096 B → 256 B | 0 | 2 |
| 256 B → 512 B | 0 | 0 |
| 512 B → 1024 B | 0 | 0 |
| 1024 B → 2048 B | 0 | 0 |
| 2048 B → 4096 B | 0 | 0 |

## 振荡段（256↔4096 ×16，32 相位）跟踪

| 方向 | 相位数 | ablation 收敛 | slow 收敛 |
|---|---|---|---|
| grow（256→4096） | 16 | 16/16（延迟 0 纪元） | 16/16（延迟 0 纪元） |
| shrink（4096→256） | 16 | 16/16（延迟 0 纪元） | 6/16（未收敛 10） |
| 相位末跟踪率（S==MTU） | 32 | 100.0% | 68.8% |

## resize 计数 / 成本

| 指标 | ablation | slow 对照 |
|---|---|---|
| n_resize（总） | 37 | 17 |
| grow（overflow_grow/grow） | 20 | 10 |
| shrink | 17 | 7 |
| resize_ns（单次切换） | mean 2241 / max 18161 | mean 139906 / max 434235 |
| drain_ns（单次排空） | mean 363212 / max 888176 | mean 365633 / max 854752 |

> resize_ns 语义：gen_switch 临界区耗时（pool_alloc/pool_free 的 mmap/munmap + 指针切换）。
> 慢对照的均值被 6 次振荡扩的同步 munmap 拉高：其 gen_switch 被延后到 old_live 恰好归零的当次 store，旧 16.9 MB 池（4096 个已提交页）的 munmap 落在计时区内（≈0.4 ms）；
> ablation 因 j_quiet=1/k_dwell=1 提前一个纪元排空，旧池经 release_old_if_drained 在普通 store 里释放（不计时），故 resize_ns 只剩 mmap（≈1 µs）。两者最终都 munmap 同一池，resize_ns 衡量的只是「临界区内的峰值延迟」，不是累计 mmap/munmap 总量。


## 稳定性（丢包 / 命中）

| 指标 | ablation | slow 对照 |
|---|---|---|
| n_drop（溢出满+延后切换丢弃） | 0 | 19656 |
| n_ovf_ins（溢出插入数） | 16400 | 8200 |
| hit / miss（NAK 重传） | 8759 / 0 | 8366 / 393 |
| hit_rate | 1.0000 | 0.9551 |

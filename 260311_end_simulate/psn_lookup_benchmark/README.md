# PSN 查找延迟基准测试（单机实验）

本目录用于在**单台 Ubuntu 虚拟机**上完成论文的「PSN 查找延迟 CDF」实验：对比你的
**PSN Mapping（环形数组直接映射）** 与论文中另外三种缓存结构，证明你的设计在
「O(1) 存储/查找 + 按序存储与提取」上的优势。

## 1. 对比的四种缓存结构

| 缩写 | 结构 | 存储 | 按 PSN 查找 | 是否按序 |
|------|------|------|-------------|----------|
| `fifo` | FIFO Queue | 到达顺序数组 | 线性扫描 **O(n)** | 是（但无索引） |
| `chained_hash` | Chained Hash（拉链哈希） | PSN 哈希到桶 | 平均 **O(1)**，桶内链扫描 | 否（乱序） |
| `balanced_tree` | Balanced Tree（AVL） | 按 PSN 有序 | **O(log n)**，指针跳转 | 是（中序遍历） |
| `psn_mapping` | **PSN Mapping（本文）** | 环形数组 `psn % RING_SIZE` | **O(1)** 一次数组访问 | **是（连续存储）** |

> `psn_mapping` 对齐你工程里的真实实现：[pkt_cache.c](../../260311_end_simulate/pkt_cache.c)
> 中 `ring_index = psn % RING_BUFFER_SIZE`，`RING_BUFFER_SIZE = 10240`。

## 2. 程序文件

- `psn_bench.c` — 基准测试（C，单文件，无第三方依赖）
- `Makefile` — 编译脚本
- `plot_cdf.py` — 画图脚本（Python + matplotlib）

## 3. 在 Ubuntu 24.04 上运行（逐步）

### 步骤 0：把本目录拷贝到虚拟机

把 `psn_lookup_benchmark/` 整个目录复制到 Ubuntu 虚拟机（例如 `~/psn_lookup_benchmark`）。
Windows 上代码跑不了，这一步在虚拟机里做。

### 步骤 1：编译

```bash
cd ~/psn_lookup_benchmark
make
# 等价于: gcc -O2 -Wall -std=gnu11 psn_bench.c -o psn_bench
```

### 步骤 2：先跑一次查找延迟 CDF（核心实验）

```bash
./psn_bench -n 10240 -m 200000 -p both -o ./out
```

- `-n 10240`：缓存占用 10240 个报文（= 满环形数组，对齐真实场景）
- `-m 200000`：每种方法 20 万次查找（CDF 样本量）
- `-p both`：随机 + 顺序两种访问模式都测
- `-o ./out`：结果写到 `./out/`

输出：

1. 终端打印一张汇总表（min / p50 / p90 / p99 / p99.9 / max / mean）；
2. `out/cdf_<方法>_<模式>.csv` —— 每个方法、每种模式各一个，存的是**已排序的逐次查找延迟（纳秒）**；
3. `out/summary.csv` —— 汇总统计（给表格用）。

### 步骤 3：画 CDF 图

```bash
pip install numpy matplotlib        # 装依赖（若已装跳过）
python3 plot_cdf.py --dir ./out --out ./out
```

生成 `out/cdf_lookup.png` 和 `out/cdf_lookup.pdf`，左右两个子图分别是随机/顺序访问的 CDF。

### 步骤 4：（可选，强烈建议）画「复杂度随占用增长」曲线

这是证明「O(1) vs O(n) vs O(log n)」最直观的一张图：

```bash
./psn_bench --sweep -m 100000 -o ./out
python3 plot_cdf.py --dir ./out --out ./out --sweep
```

生成 `out/scaling.csv` 与 `out/scaling.png/pdf`：横轴缓存占用 N，纵轴中位查找延迟（log-log）。

### 步骤 5：（可选）按序提取对比（对应「按序存储与提取」创新点）

```bash
./psn_bench --mode range -n 10240 --range-len 64 --range-num 5000 -o ./out
```

输出 `out/range_summary.csv`：连续提取一段 PSN（GBN/SR 重传场景）的每包平均/中位延迟。

---

## 4. 测试指标怎么选（论文里怎么论证）

### 4.1 为什么是「查找延迟」而不是「吞吐量」

你的缓存结构核心创新是**单次操作的复杂度**：收到 NACK/SR 请求时，网关要**按 PSN 立刻找到
对应缓存报文**再重传。这是数据通路上的关键路径，属于**延迟敏感**操作。吞吐量会受报文长度、
网络带宽干扰，而「按 PSN 定位」这一操作本身的延迟，最能反映四种结构的**算法复杂度差异**。
所以测**单次查找延迟**（单位：纳秒）。

### 4.2 为什么用 CDF 而不是只看平均值

- 平均值会被极少数慢样本拉高，掩盖分布形态；
- 网关重传是**尾延迟敏感**的：一次 NACK 处理慢，整条连接的重传就慢；
- CDF 一张图同时给出中位（P50）与尾部（P99 / P99.9），能同时回答两个问题：
  1. 典型情况多快（曲线左端、P50）；
  2. 最坏情况多慢、分布是否收敛（曲线右端、P99.9）。

**推荐报告的量**：P50（典型延迟）+ P99 / P99.9（尾延迟）。这三者是 CDF 上的关键读数，
表格里直接引用即可。

### 4.3 预期结果与解读（写论文的「结果」段落）

- `psn_mapping` 的 CDF 曲线**最靠左且最陡**：P50 与 P99.9 几乎重合，说明延迟**恒定、与占用无关**（O(1)）。
- `chained_hash` 次之，P50 接近 PSN mapping，但尾部略长（桶内链扫描 + 哈希计算）。
- `balanced_tree` 再靠右，P50 是 log(n) 量级（~十几到几十 ns），且随 N 增长（log 增长）。
- `fifo` 最差且 CDF **非常平缓**（从头部命中到扫到尾部，延迟跨越几个数量级），P99.9 是微秒级，随 N 线性增长。

**一句话结论**：PSN Mapping 用 O(1) 直接下标替代了其他结构的扫描/哈希/树遍历，
在查找延迟的**均值、中位、尾延迟**三个维度上都最优，且**延迟不随缓存占用增长**。

### 4.4 关于测量开销（论文里要诚实说明）

单次测量用 rdtsc/rdtscp，自身有 ~几纳秒固定开销，对所有方法**一致**；因此图里的**绝对纳秒值**
含该偏移，**结论应基于相对差异与曲线形态**，而不是拿 PSN mapping 的绝对 P50 去和外部系统比。

---

## 5. 论文「实验」小节怎么写（模板）

> **5.x 单机查找延迟实验**
>
> **目的**：验证 PSN Mapping 缓存结构相对 Chained Hash、Balanced Tree、FIFO Queue
> 在「按 PSN 定位缓存报文」这一关键路径上的延迟优势。
>
> **方法**：在单台主机上，四种结构各自缓存 N 个连续 PSN 的报文，重复 M 次「按 PSN 查找」，
> 用 CPU 时间戳计数器（rdtsc）逐次计时，统计查找延迟的累积分布（CDF）及 P50/P99/P99.9。
> 访问模式分随机与顺序两种，以分别刻画乱序 NACK 与顺序 ACK 场景。
>
> **结果**：如图 X 所示，PSN Mapping 的查找延迟与缓存占用无关，P50 为 ~a ns，P99.9 为 ~b ns，
> 优于 Chained Hash（~c / ~d）、Balanced Tree（~e / ~f）与 FIFO Queue（~g / ~h）。
> 随着缓存占用 N 增大，PSN Mapping 中位延迟保持不变，而 FIFO 线性增长、Balanced Tree 对数增长，
> 验证了 O(1) 复杂度的设计目标。
>
> **结论**：PSN Mapping 以「PSN 对环形数组的直接下标映射」实现 O(1) 查找，同时保留了
> PSN 顺序的连续存储，兼顾查找与按序提取，适合作为 RDMA 网关的数据包缓存结构。

---

## 6. 常见问题

- **结果抖动大**：虚拟机里先关掉无关程序；可加 `--cpu 0` 绑定核心；`-m` 调大到 50 万更稳。
- **想只看随机访问**：`-p random`。
- **想换缓存占用**：`-n 4096`（注意 psn_mapping 的环形数组固定 10240，N 不应超过它才有意义）。
- **没有 matplotlib**：`apt install python3-matplotlib` 或 `pip install numpy matplotlib`。
- **arm 机器**：本程序 rdtsc 仅 x86_64；若用 ARM 需改计时（一般不用，Ubuntu 虚拟机多为 x86_64）。

## 7. 参数速查

| 参数 | 说明 | 默认 |
|------|------|------|
| `-n N` | 缓存占用（报文数） | 10240 |
| `-m M` | 每种方法查找次数 | 200000 |
| `-p` | random / sequential / both | both |
| `-s` | 随机种子 | 固定 |
| `-o DIR` | 输出目录 | `.` |
| `--cpu N` | 绑定 CPU | 不绑定 |
| `--sweep` | 扫描不同 N | 关 |
| `--mode lookup/range` | 查找 / 按序提取 | lookup |
| `--range-len L` | range 每段长度 | 64 |
| `--range-num R` | range 段数 | 5000 |

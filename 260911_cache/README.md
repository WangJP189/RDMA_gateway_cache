# RDMA 网关缓存 — 论文实验工作目录（260911_cache）

> 本目录是论文「RDMA 网关缓存方法（PSN 映射）」的**唯一编译/运行目录**。
> 所有代码、实验、数据、图都在这里，编译与运行只在本目录下进行。

---

## 一、项目是什么（一句话）

一个 **RDMA（RoCEv2）网关**：部署在发送端与接收端之间，用 AF_PACKET 抓取
RoCEv2 DATA 报文，按 **PSN（包序列号）** 把报文缓存到内存。缓存结构采用自研的
**PSN 映射（环形数组直接下标）** 方法。

**论文核心卖点**：传统缓存方法"底层简单、上层复杂"（底层顺序 append，上层再
整理乱序包）；我们用 `ring_index = psn % RING_BUFFER_SIZE` 把每个包**按 PSN 顺序
直接落到对应内存槽**，存储天然有序、查找 O(1)，把上层的排序负担下放到底层映射。

---

## 二、目录结构

```
260911_cache/
├── Makefile                   # 编译网关主程序 rdma_gateway
├── pkt_cache.h / pkt_cache.c  # ★ 核心：流表、连接表、环形数组缓存、PSN 映射、老化
│                              #   （含本论文新增的「动态内存块」机制）
├── pkt_recv.h / pkt_recv.c    # 报文接收、按 PSN 判断丢包/查找（O(1)）
├── sr_control.h / sr_control.c# 重传控制（论文正文已删去这部分说明，代码保留）
├── rdma_opcode.h / .c         # RDMA 报文解析/封装
├── gateway_msg.h              # 网关间消息定义
├── simulate.h / .c            # 仿真报文注入（软实现验证用）
├── main.c                     # 主程序（菜单式交互）
├── ANALYSIS.md                # 实验数据解读摘要（供写论文用）
├── psn_lookup_benchmark/      # 实验一：时间开销（查找延迟）
└── psn_space_bench/           # 实验二：空间开销（内存利用率）
```

---

## 三、架构与关键机制

### 3.1 整体架构

```
RoCEv2 报文 ──> AF_PACKET 抓包 ──> 解析 (rdma_opcode) ──> 流表/连接表
                                                              │
                                   [connection_cache_array]   │
                                   ┌───────────────────────┐  │
                                   │ ring_buf[10240]       │<─┘  按 PSN 落槽
                                   │  └─ [mem_block_header][data] │
                                   └───────────────────────┘
```

每个连接持有一个 `connection_cache_array`，核心是 `ring_buf`（环形数组）：

```c
struct connection_cache_array {
    uintptr_t *ring_buf;   // 索引 = psn % RING_BUFFER_SIZE，值 = 内存块地址
    uint32_t array_length;
    uint32_t start_psn, end_psn, cur_psn;
    uint64_t last_active_stamp;
};
```

**存储**：`ring_index = psn % RING_BUFFER_SIZE`，`ring_buf[ring_index] = 内存块地址`，O(1)。
**查找/丢包判断**：同样是 `ring_buf[psn % RING_BUFFER_SIZE]` 一次数组访问，O(1)。

### 3.2 动态内存块机制（本论文新增）

论文把缓存从「固定 5KB 内存块」改为「按包大小动态分配」：

| 模式 | 分配方式 | 开关 |
|------|---------|------|
| 固定模式（现状） | 每包 `malloc(MEM_BLOCK_SIZE=5120)` | 注释掉 `DYNAMIC_BLOCK` |
| **动态模式（新机制）** | 每包 `malloc(header + data_len)`，对齐到 16B | 定义 `DYNAMIC_BLOCK`（默认开启） |

在 [pkt_cache.h](pkt_cache.h) 中通过宏切换：

```c
#define DYNAMIC_BLOCK 1   // 1 = 动态块（默认）；注释掉 = 固定 5KB
#define BLOCK_ALIGN 16    // 动态块对齐粒度（与 glibc malloc 16B 对齐一致）
```

实现见 [pkt_cache.c](pkt_cache.c) 的 `mem_block_alloc_size()`：

```c
static size_t mem_block_alloc_size(int data_len) {
#ifdef DYNAMIC_BLOCK
    size_t need = sizeof(struct mem_block_header) + (size_t)data_len;
    return (need + (BLOCK_ALIGN - 1)) & ~((size_t)BLOCK_ALIGN - 1);
#else
    (void)data_len;
    return MEM_BLOCK_SIZE;
#endif
}
```

效果：小包（64B）不再浪费 5KB，内部碎片从 ~5KB 压到 16B 对齐粒度。

---

## 四、实验

论文用「时间 + 空间」两组单机微基准论证 PSN 映射方法的优势。

### 实验一：时间开销（查找延迟）— `psn_lookup_benchmark/`

对比 4 种缓存结构按 PSN 查找的延迟：

| 方法 | 复杂度 |
|------|--------|
| `fifo` | O(n) 线性扫描 |
| `chained_hash` | O(1) 均摊（无序） |
| `balanced_tree` | O(log n) |
| `psn_mapping` | O(1) 且按序存储 |

```bash
cd psn_lookup_benchmark
make
./psn_bench -o ./out                 # 查找延迟 CDF + summary
./psn_bench -o ./out --sweep         # 中位延迟 vs 缓存占用 N
./psn_bench -o ./out --mode range    # 按序提取连续 PSN 区间
python3 plot_cdf.py --dir ./out --out ./out --all   # 画图
```

产出：`cdf_lookup.png/pdf`、`scaling.png/pdf`、`range_summary.csv`、`summary.csv`。

**结论**：PSN Mapping 查找 P50 ≈ 9ns（哈希 18ns / 树 33ns / FIFO 2.1µs），
且延迟不随缓存占用 N 增长（FIFO 线性、树对数、PSN 平线）。

### 实验二：空间开销（内存利用率）— `psn_space_bench/`

对比 6 种内存组织方式的空间利用率：

| key | 方法 | 分配方式 |
|-----|------|---------|
| `fifo` | FIFO 队列 | 指针数组 + 每包一个报文结构 |
| `chained_hash` | 链式哈希 | 桶数组 + 节点 + payload |
| `balanced_tree` | AVL 树 | 节点 + payload |
| `psn_map_fixed` | PSN 映射（固定 5KB，现状） | 每包 `malloc(5120)` + 环形数组 |
| `psn_map_dynamic` | PSN 映射（动态块，新机制） | 每包 `malloc(header+payload)` + 环形数组 |
| `contiguous` | 直接叠加包（理想下界） | N 个 `[header+payload]` 紧密拼接 |

```bash
cd psn_space_bench
make
./space_bench -o ./out                        # 空间利用率总表
./space_bench -o ./out --multiflow            # 多流缓存摊销
python3 plot_space.py --dir ./out --out ./out --multiflow   # 画图
```

产出：`space_summary.csv`、`space_multiflow.csv`、
`space_utilization.png/pdf`、`space_absolute.png/pdf`、`space_multiflow.png/pdf`。

**结论**：固定 5KB 块对 64B 小包利用率仅 ~1%；动态块机制把利用率抬到随包大小
单调爬升、最终逼近 `contiguous` 下界（~99%）。

> 详见 [ANALYSIS.md](ANALYSIS.md)，含可直接进论文的对比表与图解读。

---

## 五、编译与运行总览

```bash
# 1. 编译网关主程序
make                              # 产出 rdma_gateway

# 2. 时间实验
cd psn_lookup_benchmark && make && ./psn_bench -o ./out --sweep && \
   python3 plot_cdf.py --dir ./out --out ./out --all && cd ..

# 3. 空间实验
cd psn_space_bench && make && ./space_bench -o ./out --multiflow && \
   python3 plot_space.py --dir ./out --out ./out --multiflow && cd ..
```

环境：Ubuntu 24.04（x86_64）、gcc、C（gnu11）、pthread；Python3 + numpy + matplotlib。

---

## 六、现状与局限（论文正文如实说明）

- **只有仿真/软实现，无实机**：报文注入靠 `simulate.c`（或 AF_PACKET 抓 SoftRoCE
  流量），未在真实 RDMA NIC 上验证。
- 论文卖点是**方法/数据结构层面**的论证 + 单机微基准，不是硬件性能。
- 绝对纳秒/字节值受虚拟机影响，结论基于**相对差异与曲线形态**。

## 七、关于测试环境（4 台 VM 是否必要）

**如果论文只讲"缓存"这件事，不需要 4 台虚拟机。**

- 4 台 VM 的拓扑（发送主机/网关 ×2/接收主机）是为了跑**端到端 RDMA 打流 + 网关
  转发 + 重传**的整链路验证；这与"缓存方法"的论证无关。
- "缓存方法"的两个证据（时间开销、空间开销）都是**纯单机微基准**，一台 VM 即可
  完成——它们只测"把 N 个包放进缓存、按 PSN 查找/提取"的数据结构开销，不需要网络。
- 因此：**单台 VMware（甚至宿主机 Linux 直接跑）即可完成全部论文实验**；只有当你
  需要展示"网关在真实 RoCEv2 流量下确实能缓存/转发"时，才需要多 VM 环境。

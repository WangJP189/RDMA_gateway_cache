# RDMA 网关缓存 — 档位动态内存块（260912_cache_MultipleTie）

> 本目录是论文「RDMA 网关缓存方法（PSN 映射 + 分档动态内存块）」的**编译/运行目录**。
> 它在本目录 `../260911_cache` 的**固定块 + 简单动态块**基础上，新增了论文
> 「**档位标记数组 + 分档环形数组**」动态内存块机制，并新增一组「存 / 取」行为实验。
>
> ⚠️ 请勿修改 `../260911_cache`，本目录是独立副本，所有实验均可在**单台 VMware** 本地完成。

---

## 一、一句话说清

一个 **RDMA（RoCEv2）网关**：用 AF_PACKET 抓取 RoCEv2 DATA 报文，按 **PSN（包序列号）**
把报文缓存到内存。缓存结构采用自研的 **PSN 映射（环形数组直接下标）** 方法。

**论文核心叙事**：传统缓存「底层简单、上层复杂」——底层顺序 append、上层再整理乱序包；
我们用 `ring_index = psn % RING_BUFFER_SIZE` 让每个包**按 PSN 直接落到对应内存槽**，
存储天然有序、查找 O(1)，把上层的排序负担**下放到底层映射**。

本目录在"固定 5KB 块 / 简单 malloc 动态块"之外，新增第三种内存组织方式——
**分档动态内存块**：用「档位标记数组 + 分档环形数组」实现 **O(1) 查找 + 按档复用内存块**，
在空间与时间之间取一个新的平衡点（详见 §3）。

---

## 二、目录结构

```
260912_cache_MultipleTie/
├── Makefile                   # 编译网关主程序 rdma_gateway
├── pkt_cache.h / pkt_cache.c  # ★ 核心：连接表、档位标记数组、分档环形数组、按档内存池、老化
├── pkt_recv.h / pkt_recv.c    # 报文接收、按 PSN 判断丢包/查找（O(1)）
├── sr_control.h / sr_control.c# 重传控制（代码保留，论文正文不展开）
├── rdma_opcode.h / .c         # RDMA 报文解析/封装
├── gateway_msg.h              # 网关间消息定义
├── simulate.h / .c            # 仿真报文注入（软实现验证用）
├── main.c                     # 主程序（菜单式交互）
├── README.md                  # 本文档
├── ANALYSIS.md                # 实验数据解读摘要（含全部表格 + 图解读 + 结论）
├── psn_lookup_benchmark/      # 实验一：时间开销（查找延迟 + 随 N 增长）
├── psn_space_bench/           # 实验二：空间开销（空间利用率 + 多流摊销）
└── psn_behavior_bench/        # 实验三：存/取行为（任务 A–E）
```

---

## 三、架构与关键机制

### 3.1 整体架构

```
RoCEv2 报文 ──> AF_PACKET 抓包 ──> 解析(rdma_opcode) ──> 流表/连接表
                                                            │
                                 [connection_cache_array]   │
                                 ┌───────────────────────┐  │
                                 │ tier_mark[10240]      │<─┘  1B/槽 记档位
                                 │ tier_ring[0..4]       │      每档一根环形数组
                                 │   └ [hdr|data]        │      块大小按档固定
                                 │ pool[0..4]            │      每档空闲块池(free-list)
                                 └───────────────────────┘
```

### 3.2 三种内存组织方式（本目录的演进主线）

| 模式 | 分配方式 | 查找 | 缺点 |
|------|---------|------|------|
| 固定块（现状） | 每包 `malloc(5KB)` | O(1) 单环 | 小包浪费严重（64B 利用率仅 1.2%） |
| 简单动态块（260911 方案） | 每包 `malloc(header+len)` | O(1) 单环 | 每包 malloc 有分配器开销 + 外部碎片 + 块大小不一破坏 cache 局部性 |
| **分档动态块（本目录新机制）** | 按包大小分档，每档 `align16(header+档界)`，档内复用 | **O(1) 双数组** | 非 2 幂包大小会向上取整（空间略低于简单动态），固定数组开销 5×（见 §3.4） |

### 3.3 分档动态块机制（档位标记数组 + 分档环形数组）

核心数据结构（见 [pkt_cache.h](pkt_cache.h)）：

```c
#define TIER_COUNT 5
#define TIER_MARK_SIZE RING_BUFFER_SIZE      // = 10240

struct mem_block_pool {                       // 每档一个 slab 式空闲块池
    void *free_head;                          // 空闲块链表头（复用块内首 8B 作 next）
    size_t block_size;                        // 本档块大小（对齐到 16B）
    uint64_t alloc_count, reuse_count;        // 统计：真正 malloc 次数 / 复用次数
};

struct connection_cache_array {
    uint8_t   *tier_mark;                     // 档位标记数组：1B/槽，psn%TIER_MARK_SIZE → tier
    uintptr_t *tier_ring[TIER_COUNT];         // 分档环形数组：每档一根，各 10240×8B
    uint32_t   tier_ring_len[TIER_COUNT];     // 每档环长
    struct mem_block_pool pool[TIER_COUNT];   // 每档空闲块池
    /* ... start_psn/end_psn/cur_psn、last_active_stamp 等连接状态 ... */
};
```

**档位边界**：`g_tier_boundary[5] = {256, 512, 1024, 2048, 4096}`，由建联协商的
MTU（仿真里为全局 `g_mtu`）按 2 幂划分：`boundary[t] = mtu >> (4 - t)`。
包大小 L 落到哪个档由一次分支判断决定（`classify_tier`，O(1)）：

```c
if (L <= 256) tier = 0; else if (L <= 512) tier = 1; ... else tier = 4;
```

**块大小**：`tier_block_size(tier) = align16(sizeof(header=24) + boundary[tier])`，
即 288 / 544 / 1056 / 2080 / 4128 字节（header 为 `data_len + recv_stamp + psn` 共 24B）。

**存储（store）** —— O(1)：
```c
tier = classify_tier(data_len);
block = pool_alloc(conn, tier);            // 优先取 free-list，否则 malloc 一次
写 header + payload 到 block;
set_cached_block(conn, psn, tier, block);  // 写 tier_ring[tier][psn % len] + tier_mark[psn % MARK]
```

**查找/取回（retrieve）** —— O(1) 且**只有 2 次数组访问**：
```c
tier  = conn->tier_mark[psn % TIER_MARK_SIZE];      // 第 1 次数组访问：读档位
block = conn->tier_ring[tier][psn % tier_ring_len[tier]]; // 第 2 次：读块指针
```

**为什么要有 tier_mark（标记数组）？** 有了 5 根分档环，查找时若不知道档位，
就得对 5 根环各试一次（O(5) 搜索）。标记数组把「包大小 → 档位」的映射**预存**在
`psn % TIER_MARK_SIZE` 这个固定下标里，查找时一次读出档位、再进对应环，从而保持
**2 次确定性数组访问**的 O(1)。

**为什么分档能解决"简单 malloc"的三个问题？**
1. **分配器开销**：每档一个 free-list，稳态下 `pool_alloc` 直接弹头结点，不再每次 `malloc`（实测复用率 **94.6%**）。
2. **外部碎片**：同档块大小一致，free-list 复用时天然消除「大小不一导致的洞」。
3. **cache 局部性**：同档块大小一致，同一档的块在地址上更规整，利于预取。

### 3.4 诚实的代价（以空间换时间 + 换内存管理）

- **查找多一次数组访问**：单环 PSN 映射查找 1 次数组访问（~9ns），分档后 2 次（~28ns，
  见实验三），但仍是 O(1) 平线，远快于 FIFO（~2µs）/树/哈希。
- **固定数组开销 5×**：5 根环（各 80KB）+ 标记数组 10KB = **410KB/连接**（单环仅 80KB）。
  满载 N=10240 时每包摊 ~41B。
- **非 2 幂包大小向上取整**：1280/1400/1500 落入 2048 档，利用率 60%~71%，低于简单动态的 ~97%；
  但在档界附近（256/512/1024/2048/4096）利用率 78%~98%，仍远好于固定 5KB 块。

---

## 四、实验（三大组，任务 A–G）

| 实验 | 目录 | 回答的问题 | 任务 |
|------|------|-----------|------|
| 一：时间 | `psn_lookup_benchmark/` | 查找有多快、随 N 怎么涨 | — |
| 二：空间 | `psn_space_bench/` | 每种组织方式占多少内存 | F（包大小 6 档）、G（多流） |
| 三：行为 | `psn_behavior_bench/` | 存/取行为、丢包判定、正确性 | A/B/C/D/E |

### 实验一：时间开销（查找延迟）— `psn_lookup_benchmark/`

对比 4 种缓存结构按 PSN 查找的延迟：`fifo` O(n)、`chained_hash` O(1 无序)、
`balanced_tree` O(log n)、`psn_mapping` O(1 且按序)。

```bash
cd psn_lookup_benchmark && make
./psn_bench -o ./out                  # 查找延迟 CDF + summary
./psn_bench -o ./out --sweep          # 中位延迟 vs 缓存占用 N（复杂度曲线）
python3 plot_cdf.py --dir ./out --out ./out --all
```

**结论**：PSN Mapping 查找 P50 ≈ **9ns**（哈希 18 / 树 33 / FIFO 2.1µs），
且随 N 增长为**平线**（FIFO 线性、树对数、PSN O(1)）。

### 实验二：空间开销 — `psn_space_bench/`

对比 7 种组织方式（新增 `psn_map_tiered`）：`fifo`、`chained_hash`、`balanced_tree`、
`psn_map_fixed`（固定 5KB）、`psn_map_dynamic`（简单动态）、
**`psn_map_tiered`（分档动态，本文）**、`contiguous`（理想下界）。

```bash
cd psn_space_bench && make
./space_bench -o ./out                 # 空间利用率总表（包大小 64~4096 共 10 档）
./space_bench -o ./out --multiflow     # 多流摊销（任务 G）
python3 plot_space.py --dir ./out --out ./out --multiflow
```

**结论**：分档动态块在档界附近利用率 78%~98%，**始终优于固定 5KB 块（1.2%~80%）**，
代价是 5× 固定数组开销与 2 幂取整（详见 [ANALYSIS.md](ANALYSIS.md) §2）。

### 实验三：存/取行为（任务 A–E）— `psn_behavior_bench/`

独立复刻「档位标记数组 + 分档环形数组」机制，做存/取行为与正确性实验：

| 任务 | 内容 | 证明 |
|------|------|------|
| A | 顺序存储（psn 0..N-1 递增 store） | 存储正确、payload+PSN 全量校验一致 |
| B | 乱序存储（随机置换 store）→ 顺序取 | 乱序 store 后按序取回，**排序/交换操作数 = 0**（零整理） |
| C | 随机丢包取回（丢包率 1%） | 丢包判定 O(1) 且准确 |
| D | 突发丢包取回（连续丢 512 包） | 突发丢包同样 100% 判定 |
| E | 丢包率扫描（0%~10%） | store/retrieve 延迟**与丢包率无关**、判定恒 100% |

```bash
cd psn_behavior_bench && make
./behavior_bench -o ./out             # 默认 N=10240, L=1024, B=128, R=200, 丢包率 0~10%
python3 plot_behavior.py --dir ./out --out ./out
```

**结论**（详见 [ANALYSIS.md](ANALYSIS.md) §3）：
store P50 ≈ 52ns（顺序/乱序基本一致）、retrieve P50 ≈ 29ns（found/lost 基本一致）、
丢包判定 100%、内存复用率 **94.6%**——证明「分档池」稳态下几乎不再 malloc。

---

## 五、编译与运行总览

```bash
# 0. 编译网关主程序
make

# 1. 实验一：时间
cd psn_lookup_benchmark && make && ./psn_bench -o ./out --sweep && \
   python3 plot_cdf.py --dir ./out --out ./out --all && cd ..

# 2. 实验二：空间
cd psn_space_bench && make && ./space_bench -o ./out --multiflow && \
   python3 plot_space.py --dir ./out --out ./out --multiflow && cd ..

# 3. 实验三：行为
cd psn_behavior_bench && make && ./behavior_bench -o ./out && \
   python3 plot_behavior.py --dir ./out --out ./out && cd ..
```

环境：Ubuntu 24.04（x86_64）、gcc `-O2 -Wall -std=gnu11`、pthread；
Python3 + numpy + matplotlib（≥300dpi，输出 PNG + PDF）。

---

## 六、现状与局限（论文正文如实说明）

- **只有软实现 + 单机微基准，无实机打流**：报文注入靠 `simulate.c`（或 AF_PACKET 抓
  SoftRoCE 流量），未在真实 RDMA NIC 上验证端到端转发/重传。
- 论文卖点是**方法/数据结构层面**的论证 + 单机微基准，不是硬件性能。
- 绝对纳秒/字节值受 VMware 影响，结论基于**相对差异与曲线形态**。
- 三个实验全部**单台 VMware 本地完成**，不需要 4 台虚拟机的端到端拓扑（那只是为了
  演示真实 RoCEv2 流量下的网关转发，与"缓存方法"的论证无关）。

# RDMA 网关缓存项目 — 项目记忆（供新会话快速理解）

> 用途：把本文件内容复制到新会话开头，让新会话快速理解项目背景、现有代码与已完成的实验。
> 当前论文定位：**只讲"缓存"这件事**，核心卖点是我们的 **PSN 地址映射缓存方法**。

---

## 1. 项目是什么（一句话）

一个 **RDMA（RoCEv2）网关**：部署在发送端与接收端之间，用 AF_PACKET 抓取 RoCEv2 DATA 报文，按 **PSN（包序列号）** 把报文缓存到内存；缓存结构采用自研的 **PSN 映射（环形数组直接下标）** 方法。

## 2. 论文的核心卖点（重点！）

**网关缓存是现状**（已经存在"网关要缓存待确认数据包"这一基础/技术），我们做的是**对缓存方法本身的优化**。

- **传统方法 = "底层简单、上层复杂"**：底层一个包一个包地往缓存里存（顺序 append）；到了一定阈值，上层再把乱序到达的数据包**整理/排序/重组**。复杂度在上层。
- **我们的方法 = "以空间换时间"**：底层硬件（现在能支撑相对简单的操作）通过一个**简单的映射机制**——`ring_index = psn % RING_BUFFER_SIZE`——把每个包**按 PSN 顺序直接落到对应的内存槽位**。这样存储天然有序，上层几乎不用再做整理。
- **一句话创新**：用 O(1) 的 PSN→内存槽直接映射，同时获得「O(1) 存储/查找」与「按序存储与提取」，把上层的排序负担下放到底层映射。

## 3. 网络拓扑（4 台 VMware 虚拟机）

```
发送方主机(vmware2) ── 发送方网关(vmware1)
                              │
                              │  (网关间)
接收方网关(vmware3) ── 接收方主机(vmware4)
```

- VMnet3 = 192.168.30.0/24（发送方侧）
- VMnet4 = 192.168.40.0/24（网关间）
- VMnet5 = 192.168.50.0/24（接收方侧）
- 收发主机用 SoftRoCE（`rxe`），`ib_send_bw` 打流；网关用 AF_PACKET 抓包。

## 4. 现有代码（`260311_end_simulate/`，最终可编译运行版本）

| 文件 | 作用 |
|------|------|
| `pkt_cache.h` / `pkt_cache.c` | **核心**：流表、连接表、环形数组缓存、PSN 映射、老化 |
| `pkt_recv.c` | 报文接收、按 PSN 判断丢包/查找（`ring_buf[psn % RING_BUFFER_SIZE]` O(1)） |
| `sr_control.c` | **重传控制**（论文要删除这部分说明） |
| `simulate.c` / `simulate.h` | 仿真报文注入（用于无实机的软实现验证） |
| `rdma_opcode.c/h`、`gateway_msg.h` | RDMA 报文解析/封装 |
| `main.c` | 主程序（菜单式交互） |
| `Makefile` | 编译 |

### 关键常量（`pkt_cache.h`）

```c
#define RING_BUFFER_SIZE 10240   // 环形数组大小（存储内存块首地址）
#define TABLE_SIZE       2048    // 连接表/流表哈希桶数
#define MEM_BLOCK_SIZE   5120    // 固定 5KB 内存块（论文要改成"按包大小动态调节"）
#define PSN_MASK         0xFFFFFF// 24 位 PSN 掩码（0~16777215）
```

### 关键结构体

```c
// 每个连接的缓存（环形数组）
struct connection_cache_array {
    uintptr_t *ring_buf;   // 记录内存块地址的环形数组，索引 = psn % RING_BUFFER_SIZE
    uint32_t array_length;
    uint32_t start_psn, end_psn, cur_psn;
    uint64_t last_active_stamp;
};

// 内存块头（嵌在内存块开头）整体布局：[mem_block_header][RDMA 数据]，总大小 ≤ MEM_BLOCK_SIZE
struct mem_block_header {
    int data_len;         // 有效 RDMA 数据包长度
    uint64_t recv_stamp;  // 缓存时间戳（毫秒）
    uint32_t psn;         // 该内存块对应的 PSN
};
```

### 核心存储逻辑（`cache_rdma_packet()`）

```c
unsigned char *mem_block = malloc(MEM_BLOCK_SIZE);   // 现在：每个包固定 malloc 5KB
header->psn = psn;
memcpy(mem_block + sizeof(struct mem_block_header), data, data_len);
int ring_index = psn % RING_BUFFER_SIZE;             // ★ PSN 映射核心
conn->ring_buf[ring_index] = (uintptr_t)mem_block;   // 直接下标存储
```

**查找/丢包判断**（`is_psn_expired` / `is_packet_lost`）：同样是 `ring_buf[psn % RING_BUFFER_SIZE]` 一次数组访问，O(1)。

## 5. 已完成的实验（务必保留）

`260311_end_simulate/psn_lookup_benchmark_init/`（以及 `_20260907_1`、`_20260907_2` 两个运行副本）里有一个**单机微基准** `psn_bench.c` + `plot_cdf.py` + `README.md`：

- 对比 4 种缓存结构：`fifo`（线性扫描 O(n)）、`chained_hash`（拉链哈希 O(1) 均摊）、`balanced_tree`（AVL O(log n)）、`psn_mapping`（环形数组 O(1)）。
- 已产出**时间开销**三组结果：
  1. 查找延迟 CDF（随机/顺序访问）→ `cdf_lookup.png/pdf`
  2. 中位延迟随缓存占用 N 的 scaling（log-log）→ `scaling.png/pdf`（**最有力**：PSN Mapping 平线、树对数、FIFO 线性）
  3. 按序提取连续 PSN 区间（range）→ `range_summary.csv`
- 计时用**批量计时 + 普通 rdtsc**（避免 VMware 里 lfence/rdtscp 触发 VM exit 的 ~3.4µs 固定开销淹没 ns 级差异）。
- 结论：PSN Mapping 查找 P50 ≈ 11ns（哈希 58%、树 31%、FIFO 1/182），且延迟不随 N 增长。
- **这个实验必须保留，新实验在此基础之上追加。**

## 6. 其他历史版本（背景参考，非当前主线）

- `DPDK20260330/`：DPDK 版本网关（`tables/conn_table.h` 等），接口 `add_to_connection_cache` / `cache_rdma_packet` / `age_expired_packets`。
- `260106_parse_cache_re/`：早期版本，固定 5KB 块（`MEM_BLOCK_SIZE 5120`）。

## 7. 现状与局限（论文正文要如实说明）

- **只有仿真/软实现，没有实机**：报文注入靠 `simulate.c`（或 AF_PACKET 抓 SoftRoCE 流量），未在真实 RDMA NIC 上验证。
- 缓存目前是**固定 5KB 内存块**（小包会浪费内存，见下文"空间开销"实验要解决的问题）。
- 因此论文卖点是**方法/数据结构层面**的论证 + 单机微基准，不是硬件性能。

## 8. 技术栈 / 环境

- Linux（Ubuntu 24.04 虚拟机，x86_64）、gcc、C（gnu11）、pthread。
- 微基准：rdtsc 计时 + Python(numpy/matplotlib) 画图。
- 用户在 Windows 主机编辑，代码复制到 Ubuntu 虚拟机编译运行。

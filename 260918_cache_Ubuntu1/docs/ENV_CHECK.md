# 环境核查（ENV_CHECK）— 260918_cache_Ubuntu1（实机）

> 本文件记录第 0 步「环境核查」的全部原始输出，回答三件事（①真 RDMA 网卡 ②soft-RoCE ③内核/rdma-core 版本），
> 并给出「VM(260916) vs 实机(260918) 差异表」。
>
> 原则：第 0 步只贴输出、不改任何代码。所有命令在 `260918_cache_Ubuntu1/` 建好后、任何改动之前执行。

---

## 0. 结论速览（先答三件事）

**① 有没有真 RDMA 网卡？—— 有。**

- Mellanox **ConnectX-5 Ex（MT28800）**，双端口：
  - `rocep1s0f0` ↔ `enp1s0f0np0`（DOWN / DISABLED，无载波）
  - `rocep1s0f1` ↔ `enp1s0f1np1`（DOWN / DISABLED，无载波）
- `mlx5_core` / `mlx5_ib` 内核驱动已加载，`ibv_devices` 能枚举到两个 hca。
- **但两个端口当前都未接线**（`state DOWN / physical_state DISABLED`），本机有线网卡均 `NO-CARRIER`，
  唯一在线的网络接口是 Wi-Fi（`wlp0s20f3`）。
- 对本任务的直接影响：**无**。exp1a / exp1b / exp2 是纯 CPU/内存微基准（store/lookup/空间利用率），
  不触碰网卡；RDMA 网卡只服务于论文的「实机 + 真 RDMA 硬件」环境陈述。

**② 能否启用 soft-RoCE？—— 能。**

- `modinfo rdma_rxe` 存在（`/lib/modules/6.8.0-138-generic/kernel/drivers/infiniband/sw/rxe/rdma_rxe.ko`），
  但模块当前**未加载**（`lsmod | grep rxe` 为空）。
- 启用路径：`modprobe rdma_rxe && rdma link add rxe0 type rxe netdev <if>`。
  候选 netdev：`eno1`（板载，DOWN）、`enp1s0f0np0`/`enp1s0f1np1`（ConnectX-5 端口，DOWN）、
  `wlp0s20f3`（Wi-Fi，UP）。本实验不需要软 RoCE，故**本任务不启用**；仅确认能力存在。

**③ 内核版本 / rdma-core 版本是多少？—— kernel 6.8.0-138-generic，rdma-core 39.0-1。**

- 内核：`6.8.0-138-generic #138~22.04.1-Ubuntu SMP PREEMPT_DYNAMIC`（Ubuntu 22.04 **HWE** 内核，非 22.04 原配 5.15）。
- rdma-core：`39.0-1`（libibverbs / ibverbs-providers / ibverbs-utils 同版本 39.0-1）。
- 旧论文写的 `Ubuntu 24.04 + 7.0.0-31-generic` 是虚拟机上的，**必须改为**：`Ubuntu 22.04.5 LTS（HWE 内核 6.8.0-138-generic）+ rdma-core 39.0`。

---

## 1. 系统 / 硬件原始输出

### uname -a
```
Linux cnicasus1-System-Product-Name 6.8.0-138-generic #138~22.04.1-Ubuntu SMP PREEMPT_DYNAMIC Fri Aug  7 13:43:15 UTC  x86_64 x86_64 x86_64 GNU/Linux
```

### /etc/os-release
```
PRETTY_NAME="Ubuntu 22.04.5 LTS"
NAME="Ubuntu"
VERSION_ID="22.04"
VERSION="22.04.5 LTS (Jammy Jellyfish)"
VERSION_CODENAME=jammy
ID=ubuntu
```

### lscpu（关键行）
```
Architecture:                            x86_64
CPU(s):                                  28
On-line CPU(s) list:                     0-27
Vendor ID:                               GenuineIntel
Model name:                              Intel(R) Core(TM) i7-14700K
CPU family:                              6
Model:                                   183
Thread(s) per core:                      2
Core(s) per socket:                      20
Socket(s):                               1
CPU max MHz:                             5600.0000
CPU min MHz:                             800.0000
BogoMIPS:                                6835.20
Virtualization:                          VT-x
L1d cache:                               768 KiB (20 instances)
L1i cache:                               1 MiB (20 instances)
L2 cache:                                28 MiB (11 instances)
L3 cache:                                33 MiB (1 instance)
NUMA node(s):                            1
NUMA node0 CPU(s):                       0-27
```

> 注意：i7-14700K 为 8 P-core（超线程 ×2）+ 12 E-core 混合架构，`lscpu` 报告的 `Core(s) per socket: 20` 是
> 异构核心数之和；28 = 8×2（P 超线程）+ 12（E）。**P-core 与 E-core 频率/延迟不同**，第 1 步绑核必须绑到
> **P-core**（见 TIMING_PROTOCOL.md）。

### nproc
```
28
```

### free -g
```
               total        used        free      shared  buff/cache   available
Mem:              62           5          47           1           8          54
Swap:              1           0           1
```

### lsblk（块设备，仅磁盘行）
```
nvme0n1     259:0    0 465.8G  0 disk
├─nvme0n1p5 259:5    0 174.8G  0 part /
```

### model name / TSC 相关 flags
```
model name	: Intel(R) Core(TM) i7-14700K
```
```
rdtscp
constant_tsc
nonstop_tsc
avx2
```
> `avx512f` **不存在**（Raptor Lake i7-14700K 无 AVX-512）。`constant_tsc` / `nonstop_tsc` / `rdtscp` 均在 ——
> 第 1 步频率漂移验证的前提成立。

### lscpu | grep -i -E 'cache|numa|mhz|model name'
```
Model name:                              Intel(R) Core(TM) i7-14700K
CPU max MHz:                             5600.0000
CPU min MHz:                             800.0000
L1d cache:                               768 KiB (20 instances)
L1i cache:                               1 MiB (20 instances)
L2 cache:                                28 MiB (11 instances)
L3 cache:                                33 MiB (1 instance)
NUMA node(s):                            1
NUMA node0 CPU(s):                       0-27
```

---

## 2. NUMA

### numactl --hardware
```
/bin/bash: line 1: numactl: command not found
numactl NOT FOUND
```
> 本机**未安装 numactl**，且为单 NUMA 节点（node0 覆盖全部 28 逻辑核）。第 1 步绑核只用 `taskset`，
> 不用 `--cpunodebind/--membind`；在 ENV_CHECK / TIMING_PROTOCOL 中如实说明「numactl 未安装，单 NUMA 节点，
> 等效于 --cpunodebind=0 --membind=0」。

---

## 3. 工具链

### gcc
```
gcc (Ubuntu 11.4.0-1ubuntu1~22.04.3) 11.4.0
```

### make
```
GNU Make 4.3
```

### python3
```
Python 3.10.12
```

### numpy / matplotlib
```
2.2.6 3.10.9
```

---

## 4. 内存子系统

### /sys/kernel/mm/transparent_hugepage/enabled
```
always [madvise] never
```
> THP = **madvise**（非 always）。第 1 步第 5 条：池 mmap 统一施加 madvise 策略时必须显式处理 ——
> 四方法要么都 `MADV_HUGEPAGE`，要么都不加。

### /proc/meminfo | grep -i huge
```
AnonHugePages:     59392 kB
ShmemHugePages:  1050624 kB
FileHugePages:         0 kB
HugePages_Total:       0
HugePages_Free:        0
HugePages_Rsvd:        0
HugePages_Surp:        0
Hugepagesize:       2048 kB
```

---

## 5. CPU 频率策略

### scaling_governor (cpu0)
```
powersave
```
> 默认 **powersave**。第 1 步第 4 条要求设为 performance（记录原值 = powersave）。

### intel_pstate no_turbo
```
0
```
> `no_turbo = 0`（turbo 开启）。第 1 步第 3 条频率漂移验证正是针对 turbo/AVX 频率偏移。

---

## 6. RDMA 栈

### /sys/class/infiniband/
```
rocep1s0f0
rocep1s0f1
```

### rdma link
```
link rocep1s0f0/1 state DOWN physical_state DISABLED netdev enp1s0f0np0
link rocep1s0f1/1 state DOWN physical_state DISABLED netdev enp1s0f1np1
```

### ibv_devinfo -v | head -40
```
hca_id:	rocep1s0f0
	transport:			InfiniBand (0)
	fw_ver:				16.35.4030
	node_guid:			6cb3:1103:0088:1dce
	sys_image_guid:		6cb3:1103:0088:1dce
	vendor_id:			0x02c9
	vendor_part_id:		4121
	hw_ver:				0x0
	board_id:			MT_0000000009
	phys_port_cnt:		1
	max_mr_size:		0xffffffffffffffff
	page_size_cap:		0xfffffffffffff000
	max_qp:				131072
	max_qp_wr:			8192
	...
	max_sge:			30
	max_cq:				16777216
	max_qp_rd_atom:		16
	atomic_cap:			ATOMIC_HCA (1)
```

### modinfo rdma_rxe | head -5
```
filename:       /lib/modules/6.8.0-138-generic/kernel/drivers/infiniband/sw/rxe/rdma_rxe.ko
alias:          rdma-link-rxe
license:        Dual BSD/GPL
description:    Soft RDMA transport
author:         Bob Pearson, Frank Zago, John Groves, Kamal Heib
```

### lspci | grep -i -E 'mellanox|infiniband|ethernet'
```
01:00.0 Ethernet controller: Mellanox Technologies MT28800 Family [ConnectX-5 Ex]
01:00.1 Ethernet controller: Mellanox Technologies MT28800 Family [ConnectX-5 Ex]
05:00.0 Ethernet controller: Realtek Semiconductor Co., Ltd. RTL8125 2.5GbE Controller (rev 05)
```

### dpkg -l | grep -i -E 'rdma-core|ibverbs|libibverbs'
```
ii  ibverbs-providers:amd64                    39.0-1
ii  ibverbs-utils                              39.0-1
ii  libibverbs-dev:amd64                       39.0-1
ii  libibverbs1:amd64                          39.0-1
ii  rdma-core                                  39.0-1
```

### systemctl status rdma | head -5
```
Unit rdma.service could not be found.
```

### ip -br link（网络接口总览）
```
lo               UNKNOWN        00:00:00:00:00:00 <LOOPBACK,UP,LOWER_UP>
eno1             DOWN           cc:28:aa:77:f4:46 <NO-CARRIER,BROADCAST,MULTICAST,UP>
enp1s0f0np0      DOWN           6c:b3:11:88:1d:ce <NO-CARRIER,BROADCAST,MULTICAST,UP>
enp1s0f1np1      DOWN           6c:b3:11:88:1d:cf <NO-CARRIER,BROADCAST,MULTICAST,UP>
wlp0s20f3        UP             d4:f3:2d:aa:d0:2a <BROADCAST,MULTICAST,UP,LOWER_UP>
```

### lsmod | grep -i -E 'rxe|mlx5'（RDMA 相关内核模块）
```
mlx5_ib               499712  0
ib_uverbs             192512  2 rdma_ucm,mlx5_ib
ib_core               507904  10 rdma_cm,ib_ipoib,rpcrdma,iw_cm,ib_iser,ib_umad,rdma_ucm,ib_uverbs,mlx5_ib,ib_cm
mlx5_core            2510848  1 mlx5_ib
mlxfw                  36864  1 mlx5_core
```
（`rdma_rxe` 未加载；`mlx5_core/mlx5_ib` 已加载 —— 真网卡驱动在线。）

---

## 7. VM(260916) vs 实机(260918) 差异表

| 维度 | VM 260916（旧） | 实机 260918（新） | 对实验/论文的影响 |
|---|---|---|---|
| 物理/虚拟 | 虚拟机 | **裸机实机** | 无 VM exit / 无 hypervisor 调度抖动 |
| OS | Ubuntu 24.04 | **Ubuntu 22.04.5 LTS (jammy)** | 论文环境表述改 |
| 内核 | `7.0.0-31-generic`（VM） | **`6.8.0-138-generic`**（22.04 HWE） | 论文内核版本改 |
| CPU | 虚拟 vCPU（未记录型号） | **Intel i7-14700K**（8P+12E，28 线程，5.6 GHz turbo） | P/E 异构：绑核必须绑 P-core |
| rdtsc 读钟成本 | **~3.8 μs/次**（VM exit 底噪） | 待第 1 步实测（预期 ~20 ns 量级，低 2 个数量级） | **B 可大幅缩小**（旧 B=8192 是为盖 VM 读钟） |
| 调度停顿尾 | ~2 ms 停顿污染 ~1% 批次（p99 弃用） | 待第 1 步实测（实机尾部应干净得多） | 重新评估 p99 是否可用 |
| RDMA 网卡 | 无真网卡（soft-RoCE 环境） | **Mellanox ConnectX-5 Ex ×2 端口**（当前 DOWN）+ soft-RoCE 可用 | 论文「实机 + 真 RDMA 硬件」成立 |
| rdma-core | （未记录） | **39.0-1** | 论文 rdma-core 版本写入 |
| NUMA | 未记录 | 单 NUMA 节点；**numactl 未安装** | 绑核只用 taskset |
| THP | 未记录 | **madvise** | 第 1 步第 5 条：mmap 统一 madvise 策略 |
| CPU governor | 未记录 | **powersave**（需改 performance） | 第 1 步第 4 条 |
| 编译工具链 | 未记录 | gcc 11.4.0 / make 4.3 / py 3.10.12 / np 2.2.6 / mpl 3.10.9 | 出图字体依赖 mpl 3.10（Times 兼容替代） |

### 必须改的论文环境表述（原文 vs 改后）

- **原文**：`Ubuntu 24.04 + 7.0.0-31-generic`（虚拟机）
- **改后**：`Ubuntu 22.04.5 LTS（HWE 内核 6.8.0-138-generic），Intel Core i7-14700K（8 P-core @ 5.6 GHz turbo / 12 E-core），
  62 GB RAM，Mellanox ConnectX-5 Ex（MT28800，双端口）RDMA NIC，rdma-core 39.0`。

---

## 8. 遗留事项（转入第 1 步）

1. 原语成本（`rdtsc_raw` vs `clock_gettime(CLOCK_MONOTONIC)`）—— 待测。
2. 空批地板 per-op —— 待测。
3. **频率漂移验证**（constant_tsc/nonstop_tsc 已确认；仍需空转 vs 内存密集两次 ns/tick 标定）。
4. 绑核（taskset，P-core）+ governor 改 performance（记录原值 powersave；**改 governor 可能需 sudo，待验证**）。
5. THP madvise 策略统一。
6. B 重选（旧 B=8192 是 VM 读钟底噪产物，实机应重选）。

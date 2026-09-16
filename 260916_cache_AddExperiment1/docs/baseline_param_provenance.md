# baseline 参数溯源表（fifo / chained_hash / balanced_tree + *_bounded + psn_dynblock）

**目的**：本次 hash 桶数冲突（behavior_bench.c 16384 vs psn_bench.c 4096 vs config.h 4096）
的根因是「同一基线在不同文件里配置不同」。本文把每个基线方法的关键参数逐项列出，
**每条附出处（文件:行号）**，以后遇到「哪个值才对」直接查表，不再翻两个文件对账。

旧代码根目录：`260912_cache_ModifyExperiment/`（只读参考，未改动任何文件）。
新实现：`260916_cache_AddExperiment1/baselines/baseline.c`。

---

## 0. 统一数据包头（mem_block_header）

| 项 | 值 | 出处 |
|---|---|---|
| 字段 | `int data_len(4); uint64_t recv_stamp(8); uint32_t psn(4)` | behavior_bench.c:55-59 |
| 尺寸 | **24 B**（data_len@0, pad@4, recv_stamp@8, psn@16, pad@20） | 同上 |
| 新项目实现 | `include/dynblock.h` 的 `struct mem_block_header`（显式 `_pad0/_pad1`，`_Static_assert == 24`） | dynblock.h:28-35 |

> 三处同构：旧 behavior_bench.c 头、新 dynblock.h 头、R1_warmup_coverage.md §2 的推导
> 均为 24 B，字段偏移一致（data_len@0 / recv_stamp@8 / psn@16）。baseline.c 直接复用
> dynblock.h 的头，不另定义。

---

## 1. fifo

| 项 | 值 | 出处 |
|---|---|---|
| 结构 | `struct fifo_node { next(8); hdr(24); data[] }` = **32 B** + payload | behavior_bench.c:179-183 |
| 容量语义 | **无界**：每 store 尾插一个节点，**永不淘汰、永不 free**（无界增长，D2） | fifo_store behavior_bench.c:190-206 |
| 桶数/哈希 | 无 | — |
| 淘汰 | 无 | — |
| 重复 key | 重复 psn 各生成一个节点（不覆盖、不合并），尾插保留全部 | fifo_store :201-205 |
| store 路径 | `malloc(sizeof(node)+len)`，每包一次 | :194-195 |
| retrieve | 从头线性扫描，O(N)，逐节点比 psn | :208-217 |
| 新实现 | baseline.c `fifo_store` / `fifo_retrieve`（逐字段一致，`_Static_assert sizeof==32`） | baseline.c |

---

## 2. chained_hash

| 项 | 值 | 出处 |
|---|---|---|
| 结构 | `struct hash_node { next(8); hdr(24); data[] }` = **32 B** + payload | behavior_bench.c:243-247 |
| 桶数 | **16384**（`HASH_BITS=14`，`HASH_BUCKETS=(1<<14)`） | behavior_bench.c:49-50 |
| 哈希函数 | Knuth 乘法取高位：`(psn * 2654435761u) >> (32 - HASH_BITS)` = `>> 18` | behavior_bench.c:255-258 |
| 负载因子 | 预期元素 N=10240 → 10240/16384 = **0.625**（工程惯例 0.5–1.0 内） | 本轮裁决 |
| 容量语义 | **无界**：每 store 头插桶链，永不淘汰 | hash_store :260-273 |
| 淘汰 | 无 | — |
| 重复 key | **头插保留所有重复节点**（唯一「重复写内存无界膨胀」的方法，D3） | hash_store :270-272 |
| retrieve | 桶查找 + 链扫描，O(1) 平均 / 高常数（指针追逐 cache miss） | :275-284 |
| 新实现 | baseline.c `hash_store` / `hash_retrieve`（`hash_idx` 同乘法哈希，`__builtin_ctz(nb)` 求 shift） | baseline.c |

> **冲突裁决（2026-09-16）**：选 16384 + 乘法哈希（方案 A）。理由：① §1.4 字面要求
> 从 behavior_bench.c 复制「语义不变」；② 16384 是「按目标负载因子 0.625 设计」的工程
> 正确值，4096（负载 2.5）是偏载配置；③ 不让自己占便宜（4096 会让基线更慢）。
> `psn_bench.c:48` 的 `HASH_NBUCKETS=4096` 是旧 lookup 基准遗留值，**旧 lookup 数据与新数据
> 不可直接比较**（README 已写明）。补「桶数敏感性」：exp1b 跑 nbuckets ∈ {4096,16384} 两组。

---

## 3. balanced_tree

| 项 | 值 | 出处 |
|---|---|---|
| 结构 | `struct avl_node { l(8); r(8); h(4); pad(4); hdr(24); data[] }` = **48 B** + payload | behavior_bench.c:315-320 |
| 容量语义 | **无界**：每 store AVL 插入，永不淘汰 | avl_insert :380-401 |
| 桶数/哈希 | 无（AVL 按 psn 键排序） | — |
| 淘汰 | 无 | — |
| 重复 key | 插入比较 `if (psn < t->hdr.psn) 左 else 右` ⇒ **相等 psn 走右子树**（重复 key 仍插入） | avl_insert :396-399 |
| retrieve | 二分下降 O(log N) | tree_retrieve :410-421 |
| 平衡 | 标准 AVL 旋转（rot_r/rot_l/avl_balance） | :343-378 |
| 新实现 | baseline.c `avl_insert_new` / `tree_retrieve`（旋转/平衡逐字段一致，`_Static_assert sizeof==48`） | baseline.c |

---

## 4. *_bounded 变体（对照 D，本项目新增，无旧出处）

设计目标（本轮裁决）：消除「分配策略」与「活集大小」两个 confound —— 预分配空闲链表 +
容量 N + 滑动淘汰 ⇒ **活集恒为 N ⇒ hash 负载恒 N/16384=0.625**，与 psn_dynblock 唯一差异
回到「索引结构」本身。

| 项 | fifo_bounded | chained_hash_bounded | balanced_tree_bounded |
|---|---|---|---|
| 容量 | N（=cfg.ring_n） | N | N |
| 分配 | 预分配池（一次 malloc N 个节点，复用首 8B 作 free-list） | 同左 | 同左 |
| 淘汰（满时） | 淘汰 head（FIFO 滑动） | **位置淘汰** slot=psn%N（同 dynblock Φ） | 淘汰最早插入（FIFO，插入序环队列 fifo_psn） |
| 淘汰成本 | O(1) | O(链长)≈O(1)（负载 0.625） | O(log N) |
| 节点尺寸 | 32 B + payload（不变） | 32 B + payload（不变） | 48 B + payload（不变） |
| 桶数/哈希 | — | 16384，乘法哈希 | — |
| store 路径 | **malloc=0**（池取块） | malloc=0 | malloc=0 |
| 实现 | baseline.c `make_fifo_bounded` :172 | `make_chained_hash_bounded` :300（含 slot_owner[N]） | `make_balanced_tree_bounded` :497（fifo_psn 环 + avl_delete_key :391） |

> **淘汰策略：三种 *_bounded 一律 FIFO（按插入/PSN 序），不用 LRU**。理由：机制的淘汰 =
> 环回绕 = 最早进入者被覆盖；而 retrieve 是「取包不删」（不改变驻留），所以基线若用 LRU 就
> 变成另一套策略、对照不干净。三结构的「自然」FIFO 化方式不同：fifo 天然 FIFO（淘汰 head）；
> hash 用位置淘汰 slot=psn%N（与 dynblock 同一 eviction 语义，seq 下即 FIFO，slot_owner 记录
> slot→节点）；tree 用插入序环队列（fifo_psn）淘汰队头 + avl_delete_key 物理摘除（不搬数据）。
> 三者 seq 负载下活集均为「最近 N 个 psn」，语义一致；store 延迟均计入各自淘汰成本
> （与 dynblock 的 evict_occupant 同口径）。**（2026-09-16 修正 tree_bounded：原「淘汰最小
> key」是排序集合的语义、不是缓存，已改为 FIFO。）**
>
> **顺序 vs 乱序到达下三者是否一致**：fifo_bounded 与 balanced_tree_bounded 是**严格插入序
> FIFO**（与到达顺序无关，任何到达序下都踢最早插入者）；chained_hash_bounded 是**位置淘汰**
> slot=psn%N（踢谁取决于 PSN 值落到哪个环位）：顺序到达 = 踢最早 PSN = FIFO，乱序到达 = 踢
> 「同环位的前一包」（与 dynblock Φ 完全一致，非 FIFO）。⇒ 三者只在【顺序 PSN 到达】下等价；
> exp1a 主 store 序列为顺序 PSN（存 0..N-1）⇒ 主结果不受策略选择影响（README 已写明这一前提）。

---

## 5. psn_dynblock（我们）

| 项 | 值 | 出处 |
|---|---|---|
| 结构 | `slot_meta_t`(12B) + 环池 `pool_t`（stride=align16(24+S)） | include/dynblock.h:52-66 |
| 容量 | **有界** N=ring_n（重传窗口长度） | config.h CFG_RING_N |
| 分配 | 池 init 时 mmap(MAP_NORESERVE)，store=memcpy，**malloc=0**（不变量 I3） | dynblock.c conn_init :34-46 |
| 淘汰 | 位置淘汰 `Φ(psn)=(psn&0xFFFFFF)%N`，O(1) | dynblock.c evict_occupant :67-77 |
| 桶数/哈希 | 无（直接地址计算） | — |
| 重复 key | 位置覆盖（同 slot 新包覆盖旧包） | conn_store :94-102 |
| retrieve | `Φ(psn)` 一次取模 + slot 校验，O(1) | conn_lookup :135-147 |

---

## 6. 交叉核对（节点尺寸无漂移）

| 节点 | 推导 | sizeof 断言 | 状态 |
|---|---|---|---|
| fifo_node | 8+24=32 | `_Static_assert(sizeof==32)` | ✓ |
| hash_node | 8+24=32 | `_Static_assert(sizeof==32)` | ✓ |
| avl_node | 8+8+4+pad4+24=48 | `_Static_assert(sizeof==48)` | ✓ |

与 R1_warmup_coverage.md §2 表一致（fifo/hash=32、avl=48）；malloc 尺寸类
`sizeof(node)+payload` = {96,112,1056,1072,4128,4144} 未漂移。

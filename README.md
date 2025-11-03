## 1. 程序的主要用途（这个程序主要是干什么的）

### 概述

该项目是一个用户态的 RDMA 报文捕获与重传/缓存模块，目标是作为 RDMA 网关/中间件的一部分，在用户态拦截（通过 AF_PACKET）RoCEv2/UDP 承载的 RDMA 报文，解析出 RDMA BTH/AETH 信息，并针对 Data/ACK/NACK 做缓存、确认清理与重传控制。

### 主要功能包括：

1. 在网卡界面上以混杂模式捕获以太网帧（pkt_recv.c:create_promiscuous_socket + receive_and_parse_frames）。
2. 解析 IP/UDP/BTH/AETH 等 RDMA 报文头（pkt_recv.c:process_rdma_packet、parse_bth_header、parse_aeth_header）。
3. 对接收到的数据报文按连接（5元/包含PKey/服务类型）进行有序缓存（pkt_cache.c 的 add_to_connection_cache、insert_packet_sorted）。
4. 处理 ACK：释放（清理）已被确认的缓存报文（pkt_recv.c:handle_ack_received）。
5. 处理 NACK：触发精确或批量重传（GBN、fallback），通过用户态改写源 MAC 并发送（retransmit_rdma_packet 被调用；在代码中为外部/未给出实现）。
6. 定期清理空闲/超时连接（pkt_cache.c:cleanup_expired_connections）。

### 用途定位

作为 RDMA 网关/中间件的一部分，用于实现对 RDMA 报文的透明代理、重传策略（例如当广域网导致丢包时进行本地/网关级别重传）、缓存数据以便重传、并为上层控制平面（例如 SR：Selective Retransmit）提供必要状态信息。

## 2. 总体代码逻辑（他是想怎么完成目标的 — 详细流程与关键算法）

整体分为抓取层、解析层、缓存/状态层与控制层四个部分

### 主要数据结构（在 pkt_cache.c）：

1. ``struct cached_packet``：单个缓存报文（包含 app_data、data_len、dest_qp、psn、timestamp、双向链表指针）。
2. ``struct connection_cache``：每个“连接”的缓存队列（双向链表 head/tail，count，total_bytes，min_psn/max_psn，last_activity，pthread_mutex_t lock）。
3. ``struct connection_key``：连接标识，用于哈希（src/dst IP、src/dst port、service_type、pkey，并注释中提到 future 字段 dest_qp/src_qp）。
4. ``struct hash_table_entry``：哈希表的桶链表节点（key + cache）。
5. ``struct cache_manager``：全局管理器（哈希表指针、大小、上限、锁、统计等），全局实例 g_cache_mgr。

### 流程（从接收到操作）：

1. 抓取层（pkt_recv.c）：

    使用原始套接字 AF_PACKET 在指定接口（例如 eth0）开启混杂模式（create_promiscuous_socket）。

    循环 recvfrom 捕获数据帧并把缓冲传给 process_rdma_packet。

2. 解析层（rdma_opcode.c的process_rdma_packet、parse_bth_header、parse_aeth_header）

    解析以太网 / IP / UDP，过滤 RoCEv2 端口（代码用 UDP 4791）。

    定位 BTH（Base Transport Header）并解析：opcode、pkey、dest_qp、psn 等（注意 BTH 中某些字段为 24 位）。

    通过 get_packet_type_rc()（rdma_opcode.c）将 opcode 映射为类型：DATA / ACK / NACK / READ_RESPONSE / UNKNOWN。

    如果期望有 AETH（is_aeth_expected 辅助），解析 AETH（例如 ack 中的 ePSN、syndrome）。

3. 缓存/状态层（pkt_cache.c）

    ``对 DATA 报文``：计算应用数据区位置并调用 add_to_connection_cache。add_to_connection_cache 会：
    根据 src/dst/ip/port/pkey/service_type 构造 connection_key（create_connection_key）。在全局哈希表通过 get_or_create_connection_cache 获取或新建 connection_cache（有全局锁mg->global_lock 来保护哈希表结构，且为每个 connection_cache 初始化自己的 mutex）。分配 cached_packet 并复制 app_data。调用 insert_packet_sorted 将新包按 PSN 插入到双向链表中（保持有序），更新 count、total_bytes、min_psn/max_psn，并检查每连接的包数/字节限额（g_cache_mgr->max_packets_per_conn / max_bytes_per_conn）。

    ``查找报文``（find_packets_by_psn_range）：按 PSN 范围检索并将匹配项移出（代码中有 TODO 关于是否真正从原链表摘除的讨论）。

    ``获取连接 PSN 范围``（get_connection_psn_range）用于重传范围判断或诊断。

4. 控制层：ACK/NACK/重传逻辑（在 pkt_recv.c & pkt_cache.c）

    ``ACK（process_ack_packet -> handle_ack_received）``：解析 AETH 得到 ePSN（ack_epsn）。反向构造 connection_key（注意：因为 ACK 从对端返回，因此 key 的 src/dst 需要对调）。遍历对应连接的链表并释放所有 PSN <= ePSN 的缓存报文（更新统计）——释放内存并更新 min_psn/max_psn。

    ``NACK（process_nack_packet -> handle_nack_received）``：解析 AETH 得到 ePSN 和 syndrome。尝试在缓存中找到精确的 ePSN 报文；若找到则通过 retransmit_rdma_packet 触发精确重传（单包或批量）。若找不到精确报文：调用 handle_nack_fallback_retransmit，该函数会获取当前 max_psn，再调用 find_packets_by_psn_range（nack_epsn..max_psn）来获取需要重传的报文集合，然后批量重传（GBN）。

    ``GBN 批量重传（trigger_batch_retransmit）``：遍历待重传链表，调用 retransmit_rdma_packet（用户态重写源 MAC 并发送），然后释放这些缓存节点（注：代码注释假设重传函数会复制数据，因此释放原内存）

    ``定期清理（cleanup_expired_connections）``：遍历哈希表，若某个 connection 的 last_activity 超过 connection_timeout 或 count==0，则销毁该连接缓存并从哈希表移除。

5. 并发与锁策略：

    全局哈希表操作使用 mgr->global_lock 保护（查找、插入 entry、删除 entry 时）。每个 connection_cache 有自己的 cache->lock 保护链表的并发访问与统计更新。


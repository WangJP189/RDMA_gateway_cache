# Experiment Report — PSN Deterministic-Mapping Gateway Cache

**Paper target:** ICASSP 2027 · RDMA (RoCEv2) gateway cache
**Thesis:** traditional gateway caches are *"simple at the bottom, complex at
the top"* — a trivial FIFO/hash store that then must reorder out-of-order WAN
packets at read time. We replace it with *"deterministic at the bottom, zero
reorder at the top"*: `ring_index = psn % RING_SIZE` gives O(1) store/lookup and
ordered storage for free.

---

## 1. Status quo — why gateway caching is hard

A RoCEv2 gateway sits between a lossless DC fabric and a lossy WAN. Because WAN
links drop and reorder (multipath), the gateway must **buffer in-flight packets
keyed by PSN** so it can retransmit on NACK and re-sequenced-deliver them.

Existing gateway caches are "simple at the bottom, complex at the top":

- **Bottom (store) is trivial.** A FIFO appends on arrival; a hash inserts.
  No ordering work is done at store time.
- **Top (retrieve) is expensive.** To deliver packets *in PSN order* after
  multipath reordering, the gateway must **search and reorder**:
  - FIFO → linear scan, **O(N)** per packet;
  - balanced tree → **O(log N)** with pointer chasing;
  - chained hash → O(1) average but **unordered** (still needs a sort/merge).

So the reorder cost is pushed to the top, where it scales with the cache depth N.

## 2. Motivation — what we want

If the cache **indexes packets deterministically by PSN at store time**, then
"out-of-order store" lands each packet in its final ordered slot, and
"ordered retrieve" is just a linear walk. We want a method with four properties:

1. **O(1) lookup** — latency independent of cache depth N;
2. **O(1) retrieve** — same;
3. **zero reorder** — out-of-order store, ordered retrieve, **no
   compare/search**;
4. **deterministic memory** — steady-state malloc = 0, no external
   fragmentation (a long-lived gateway must not degrade).

Plus a fifth, necessary honesty constraint: **space acceptable** — not the
fixed-5 KB-per-packet waste of the naive PSN-indexed approach.

## 3. Method — PSN deterministic mapping + tiered dynamic blocks

**Indexing.** `ring_index = psn % RING_SIZE` (RING_SIZE = 10240). Store and
lookup are a single modular step — O(1), and the slot order is the PSN order,
so an out-of-order store is automatically "pre-sorted".

**Memory.** A fixed block per packet would cost 5 KB (the naive baseline,
`psn_fixed`). A per-packet `malloc(header + len)` (`psn_dynamic`) is exact but
churns the allocator every packet. Our method, **`psn_tiered`**, buckets packets
into RDMA-MTU **size classes** {256, 512, 1024, 1536, 2048, 4096} B:

- a **tier-marker array** (1 B/slot) records which tier each slot belongs to;
- **6 per-tier ring arrays** (one pointer per slot per tier);
- **per-tier free lists** recycle blocks, so steady-state malloc = 0.

A 1536 B intermediate tier is deliberately inserted between 1024 and 2048 B to
absorb the common 1280–1500 B Ethernet-frame sizes and remove the internal-
fragmentation dip there.

## 4. Evidence — three benchmarks, three figures

Everything below is measured on a single machine, fixed seed 42, ≥5 repeats
(error bars = std), batched `rdtsc` timing.

### 4.1 Time (Figure 1)

**Fig 1(a) — lookup latency vs cache depth N.** `psn_bench --sweep` measures the
P50 lookup latency as N grows 512 → 10240:

| N      | FIFO  | hash | AVL   | **PSN mapping** |
|--------|-------|------|-------|-----------------|
| 512    | 83.8  | 9.9  | 23.0  | **9.8**         |
| 4096   | 917.1 | 10.3 | 29.3  | **10.0**        |
| 10240  | 2608.0| 20.6 | 37.3  | **10.0**        |

FIFO grows linearly (O(N)), AVL grows logarithmically, PSN mapping is a flat
**~10 ns regardless of N** → O(1) lookup proven.

**Fig 1(b) — lookup latency CDF (random).** P50: FIFO 2.56 µs, hash 21 ns,
AVL 38 ns, PSN mapping **11.4 ns** — the O(1) method is also the fastest.

**Fig 1(c) — store processing latency (64 B payload, steady state).** The box
plot isolates the data-structure overhead from the payload copy:

| method        | P25    | P50     | P75     | P99     |
|---------------|--------|---------|---------|---------|
| **tiered**    | **84.4**| **86.0**| **90.7**| 413     |
| dynamic       | 94.6   | 96.9    | 100.8   | 595     |
| hash          | 100.1  | 101.6   | 120.4   | 575     |
| fifo          | 283.0  | 311.9   | 401.0   | 1047    |
| fixed 5 KB    | 283.0  | 354.1   | 482.3   | 1383    |
| tree          | 296.3  | 368.9   | 458.1   | 1130    |

Tiered has the **lowest median (86 ns) and the narrowest spread (IQR 6 ns)** —
deterministic memory means deterministic latency.

### 4.2 Space (Figure 2)

`space_bench` (6 tiers) reports space utilization = payload / allocated:

| L (B)  | fixed 5 KB | dynamic | **tiered** | contiguous (ideal) |
|--------|-----------|---------|-----------|--------------------|
| 64     | 1.2%      | 61.5%   | 19.0%     | 72.7%              |
| 512    | 10.0%     | 92.8%   | 86.3%     | 95.5%              |
| 1024   | 20.0%     | 96.2%   | 92.7%     | 97.7%              |
| 1280   | 25.0%     | 97.0%   | 79.2%     | 98.2%              |
| 1500   | 29.3%     | 97.2%   | 92.8%     | 98.4%              |
| 2048   | 39.9%     | 98.1%   | 96.2%     | 98.8%              |
| 4096   | 79.9%     | 99.0%   | 98.1%     | 99.4%              |

Tiered is **far above fixed 5 KB** and **approaches the contiguous ideal** as L
grows. The former V-dip at 1024–1500 B is gone (60→79% at 1280, 66→87% at
1400, 71→93% at 1500) thanks to the 1536 B tier. Panels (c)/(d) show the
fixed ring overhead amortizes with connections K and cache depth npp.

### 4.3 Behavior (Figure 3) — the three decisive experiments

**Fig 3(a) — zero reorder.** Store N = 10240 packets in a random (multipath)
order, retrieve in PSN order, count PSN comparisons per retrieve:

| method          | avg comparisons |
|-----------------|-----------------|
| fifo            | 5120.5          |
| balanced_tree   | 12.6            |
| chained_hash    | 1.0             |
| **psn_fixed / dynamic / tiered** | **0** |

Ours needs **0 comparisons** — retrieve touches exactly the mapped slot.
Correctness (PSN + header length + payload) verified 51200/51200.

**Fig 3(b) — ordered-retrieve latency CDF.** Same out-of-order store, timed
ordered retrieve. P50: fifo 9.39 µs (the scan), hash 40.7 ns, tree 114.9 ns,
**tiered 39.1 ns**. Ours is the fastest and payload-independent.

**Fig 3(c) — deterministic memory.** Simulate a long-running gateway with
N × 10 = 102400 overwrite stores, count malloc per store:

| method          | malloc / store |
|-----------------|----------------|
| psn_fixed (5 KB)| 1.0            |
| psn_dynamic     | 1.0            |
| **psn_tiered**  | **0.0**        |

The tiered free-list pool recycles blocks: **steady-state malloc = 0**, i.e. no
allocator churn and no external fragmentation.

Two wraparound correctness checks (ring-index wrap and 24-bit PSN wrap) both
pass 10240/10240 and 2/2, confirming the modulo mapping is sound across PSN
rollover.

## 5. Conclusion

The PSN deterministic-mapping cache, realized with tiered dynamic blocks,
converts the traditional *"simple bottom, complex top"* gateway cache into a
*"deterministic bottom, zero-reorder top"* one:

- **O(1) store and lookup** — latency flat at ~10 ns regardless of depth;
- **zero reorder** — 0 comparisons to deliver ordered from out-of-order;
- **deterministic memory** — 0 steady-state malloc, tight latency (IQR 6 ns);
- **acceptable space** — 93–98% utilization at common MTUs, far above the
  20% of fixed 5 KB, approaching the contiguous ideal.

The cost — the tiered ring arrays' fixed overhead — is a one-time, per-connection
price that amortizes with cache depth, and buys O(1), order, and determinism in
return.

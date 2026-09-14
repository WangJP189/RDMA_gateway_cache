# Experiment 3(a) — Average PSN comparisons vs. cache depth N

Micro-benchmark proving the **zero-reorder** claim as a *scaling* curve: as the
cache depth N grows, we count the average number of PSN **equality** comparisons
performed inside one retrieval, for four cache structures:

| method        | structure                        | expected |
|---------------|----------------------------------|----------|
| `fifo`        | arrival-order linked list        | O(N) linear scan |
| `chained_hash`| 4096-bucket chained hash         | ~1 + load/2, slow rise |
| `balanced_tree`| AVL tree                         | O(log N) |
| `psn_tiered`  | PSN-modulo tiered ring (ours)    | **0 (direct index, no comparison)** |

## Files

```
psn_zeroreorder/
├── zeroreorder_bench.c            # C benchmark (reuses data structures from
│                                  #   psn_behavior_bench/behavior_bench.c)
├── Makefile                       # make / make run / make plot
├── out/
│   └── zeroreorder_scaling.csv    # header: method,N,avg_cmp,std_cmp
├── plot_zeroreorder.py            # SIGCOMM-style log-log line plot
├── fig_zeroreorder_scaling.pdf    # output vector figure
└── README.md
```

## Reproduce

```bash
cd 260912_cache_ModifyExperiment/psn_zeroreorder

make          # compile zeroreorder_bench (gcc -O2, links -lm)
make run      # run the full N sweep (5 repeats) -> out/zeroreorder_scaling.csv
make plot     # draw fig_zeroreorder_scaling.pdf from the CSV
```

`make run` is one-shot: the binary internally sweeps all N points and all
5 repeats, then writes the whole CSV.

## Experiment parameters (fixed, for reviewers)

- **seed = 42** — xorshift32 PRNG, a distinct stream per (N, repeat).
- **repeats = 5** — each (N, method) point is measured 5 independent times;
  `avg_cmp` is the mean, `std_cmp` the sample standard deviation (error bar).
- **N sweep = {512, 1024, 2048, 4096, 8192, 10240}** — matches the Fig-1 lookup
  scaling sweep for cross-comparison.
- **payload L = 64 B** (the metric is comparison count, independent of payload).
- **chained hash buckets = 4096** (`HASH_BITS = 12`), so collisions occur
  within the sweep (load factor up to 2.5 at N = 10240).
- **tier boundaries = {256, 512, 1024, 1536, 2048, 4096} B**, 6 tiers.

## Procedure

For each N, each method, each repeat:
1. insert N PSNs in a **shuffled** order (out-of-order arrival, multipath);
2. reset the comparison counter;
3. run N random queries — uniform over the inserted PSNs, with replacement —
   counting the equality comparisons inside each retrieve;
4. `avg_cmp = (total equality comparisons) / N`.

Only PSN **equality** comparisons (`==`) inside the retrieve operation are
counted — pointer, bound, and ordering tests are excluded. `psn_tiered`
retrieves by `slot = psn % ring_len` with no comparison, so its count is
exactly 0. Retrieval correctness (PSN + payload) is checked on every retrieve
and printed at the end of `make run`.

## CSV schema

`method,N,avg_cmp,std_cmp` — one row per (N, method). `method` ∈
{fifo, balanced_tree, chained_hash, psn_tiered}.

## Expected / validation

- `psn_tiered` — `avg_cmp == 0` at every N (strictly);
- `fifo` — grows ~linearly (≈ N/2);
- `balanced_tree` — grows ~logarithmically (≈ log₂ N);
- `chained_hash` — slowly rises (≈ 1 + N/8192).

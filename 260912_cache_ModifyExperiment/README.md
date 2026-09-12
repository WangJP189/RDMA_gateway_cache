# RDMA Gateway Cache — Redesigned Experiments (ICASSP 2027)

This folder contains the redesigned benchmark suite for the **PSN deterministic
mapping** gateway cache (`ring_index = psn % RING_SIZE`). Three benchmarks prove
three claims, and three combined figures present them in SIGCOMM style.

The original (unmodified) experiments live in
`../260912_cache_MultipleTie/`; everything here is self-contained.

## Folder layout

```
260912_cache_ModifyExperiment/
├── psn_lookup_benchmark/   time cost  (O(1) lookup vs N, CDF)
│   ├── psn_bench.c         -- seed=42, --reps K (default 5), scaling std
│   ├── Makefile            -- gcc -O2 -Wall -std=gnu11 ... -lm
│   └── out/                -- scaling.csv, summary.csv, cdf_*_random.csv
├── psn_space_bench/        space cost (6 tiers incl. 1536 B)
│   ├── space_bench.c       -- TIER_COUNT 6 {256,512,1024,1536,2048,4096}
│   ├── Makefile
│   └── out/                -- space_summary.csv, space_multiflow.csv
├── psn_behavior_bench/     behavior (3 experiments A/B/C + store samples)
│   ├── behavior_bench.c    -- zero-reorder / retrieve-CDF / deterministic-mem
│   ├── Makefile
│   └── out/                -- zero_reorder.csv, retrieve_samples.csv,
│                             deterministic_mem.csv, store_samples.csv
└── paper_figures/
    ├── plot_figures.py     -- English, serif, ColorBrewer, bold-ours, 300 dpi
    ├── fig1_time.{png,pdf}
    ├── fig2_space.{png,pdf}
    └── fig3_behavior.{png,pdf}
```

## Reproduce

```bash
cd 260912_cache_ModifyExperiment

# 1. build
make -C psn_lookup_benchmark
make -C psn_space_bench
make -C psn_behavior_bench

# 2. run (each writes into its own out/)
(cd psn_lookup_benchmark && ./psn_bench -m 500000 -B 512 --reps 5 -o out \
   && ./psn_bench --sweep -m 500000 -B 512 --reps 5 -o out)
(cd psn_space_bench     && ./space_bench -o out --multiflow)
(cd psn_behavior_bench  && ./behavior_bench -o out)

# 3. figures
python3 paper_figures/plot_figures.py
```

## What each experiment proves

| Experiment | Output CSV | Claim |
|-----------|-----------|-------|
| Fig 1(a)  lookup vs N        | `scaling.csv`                  | O(1) lookup, latency independent of N |
| Fig 1(b)  lookup CDF         | `cdf_*_random.csv`             | O(1) lookup distribution |
| Fig 1(c)  store box plot     | `store_samples.csv`            | O(1) store, narrowest + lowest median |
| Fig 2     space (4 panels)   | `space_summary.csv`, `space_multiflow.csv` | tiered ≈ ideal bound, >> fixed 5 KB |
| Fig 3(a)  zero reorder       | `zero_reorder.csv`             | 0 PSN comparisons (zero reorder) |
| Fig 3(b)  retrieve CDF       | `retrieve_samples.csv`         | O(1) ordered retrieve |
| Fig 3(c)  deterministic mem  | `deterministic_mem.csv`        | 0 malloc in steady state |

See `EXPERIMENT_REPORT.md` for the full narrative (status quo → motivation →
method → evidence → conclusion).

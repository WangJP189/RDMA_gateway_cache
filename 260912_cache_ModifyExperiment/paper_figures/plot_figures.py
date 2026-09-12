#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Paper combined-figure generator (English, ICASSP-2027 style).

Merges the three benchmarks into three figures, each arguing one aspect:

  fig1_time.png/pdf      Time cost: O(1) lookup / O(1) store, latency independent
                         of cache depth, deterministic store tail.
  fig2_space.png/pdf     Space cost: tiered dynamic blocks remove the fixed-5KB
                         small-packet waste and approach the contiguous bound.
  fig3_behavior.png/pdf  Behavior: zero reorder, O(1) ordered retrieve,
                         deterministic (malloc-free) memory.

Usage (from 260912_cache_ModifyExperiment/):
    python3 paper_figures/plot_figures.py

Inputs (already-run benchmarks):
    psn_lookup_benchmark/out/   scaling.csv, cdf_*_random.csv
    psn_space_bench/out/        space_summary.csv, space_multiflow.csv
    psn_behavior_bench/out/     zero_reorder.csv, retrieve_samples.csv,
                                deterministic_mem.csv, store_samples.csv

Outputs (paper_figures/):  fig{1,2,3}_*.{png,pdf} at 300 dpi.

Style: serif (Times New Roman / Liberation Serif), colorblind-friendly
ColorBrewer-inspired mapping — traditional methods in the gray family, PSN
variants in the blue family, our method (psn_tiered / psn_mapping) as the dark
blue highlight (bold, LineWidth=3).
"""

import os
import csv
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Patch

# ---- serif font (metric-compatible Times New Roman substitute) ----
plt.rcParams["font.family"] = "serif"
plt.rcParams["font.serif"] = ["Times New Roman", "Liberation Serif",
                              "DejaVu Serif"]
plt.rcParams["axes.unicode_minus"] = False
plt.rcParams["font.size"] = 9

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
LOOKUP = os.path.join(ROOT, "psn_lookup_benchmark", "out")
SPACE = os.path.join(ROOT, "psn_space_bench", "out")
BEHAV = os.path.join(ROOT, "psn_behavior_bench", "out")
OUT = HERE

# ---- unified color mapping (gray family = traditional, blue family = PSN,
#      dark blue = ours) ----
COLOR = {
    "fifo": "#bdbdbd",
    "chained_hash": "#969696",
    "balanced_tree": "#636363",
    "psn_fixed": "#9ecae1",
    "psn_dynamic": "#6baed6",
    "psn_tiered": "#08306b",
    "psn_mapping": "#08306b",
    "contiguous": "#111111",
}
LABEL = {
    "fifo": "FIFO (arrival order)",
    "chained_hash": "Chained hash",
    "balanced_tree": "Balanced tree (AVL)",
    "psn_fixed": "PSN fixed 5 KB",
    "psn_dynamic": "PSN dynamic",
    "psn_tiered": "PSN tiered (ours)",
    "psn_mapping": "PSN mapping (ours)",
    "contiguous": "Contiguous (ideal)",
}
ORDER_LOOKUP = ["fifo", "chained_hash", "balanced_tree", "psn_mapping"]
ORDER_6 = ["fifo", "chained_hash", "balanced_tree", "psn_fixed",
           "psn_dynamic", "psn_tiered"]
ORDER_SPACE = ["fifo", "chained_hash", "balanced_tree", "psn_map_fixed",
               "psn_map_dynamic", "psn_map_tiered", "contiguous"]
SPACE_LABEL = {
    "fifo": "FIFO (arrival order)",
    "chained_hash": "Chained hash",
    "balanced_tree": "Balanced tree (AVL)",
    "psn_map_fixed": "PSN fixed 5 KB",
    "psn_map_dynamic": "PSN dynamic",
    "psn_map_tiered": "PSN tiered (ours)",
    "contiguous": "Contiguous (ideal)",
}
SPACE_COLOR = {
    "fifo": "#bdbdbd",
    "chained_hash": "#969696",
    "balanced_tree": "#636363",
    "psn_map_fixed": "#9ecae1",
    "psn_map_dynamic": "#6baed6",
    "psn_map_tiered": "#08306b",
    "contiguous": "#111111",
}

OURS = {"psn_tiered", "psn_mapping", "psn_map_tiered"}


def read_csv(path):
    with open(path, "r", encoding="utf-8") as f:
        return list(csv.DictReader(f))


def read_scaling():
    return read_csv(os.path.join(LOOKUP, "scaling.csv"))


def read_cdf(method):
    p = os.path.join(LOOKUP, "cdf_%s_random.csv" % method)
    vals = []
    with open(p, "r", encoding="utf-8") as f:
        next(f)  # header "latency_ns"
        for line in f:
            line = line.strip()
            if line:
                vals.append(float(line))
    return np.array(sorted(vals))


def read_long(path):
    """Read a long-format CSV (method, latency_ns) -> {method: np.array}."""
    data = {}
    with open(path, "r", encoding="utf-8") as f:
        next(f)  # header
        for line in f:
            line = line.strip()
            if not line:
                continue
            parts = line.split(",")
            data.setdefault(parts[0], []).append(float(parts[1]))
    return {k: np.array(v) for k, v in data.items()}


def read_zero_reorder():
    rows = read_csv(os.path.join(BEHAV, "zero_reorder.csv"))
    return {r["method"]: (float(r["avg_cmp"]), float(r["std_cmp"])) for r in rows}


def read_deterministic_mem():
    rows = read_csv(os.path.join(BEHAV, "deterministic_mem.csv"))
    return {r["method"]: (float(r["malloc_per_store"]), float(r["std"]))
            for r in rows}


def read_space_summary():
    return read_csv(os.path.join(SPACE, "space_summary.csv"))


def read_space_multiflow():
    return read_csv(os.path.join(SPACE, "space_multiflow.csv"))


def style_ax(ax):
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)
    ax.grid(alpha=0.3, linewidth=0.5)


def save(fig, name):
    for ext in ("png", "pdf"):
        fig.savefig(os.path.join(OUT, "%s.%s" % (name, ext)), dpi=300,
                    bbox_inches="tight")
    plt.close(fig)
    print("[OK] %s.png/pdf" % name)


def plot_cdf(ax, curves, colors, labels, bold=(), xlabel="Latency (ns)",
             ylabel="Cumulative probability"):
    """curves: {name: sorted np.array}. bold: set of names drawn LineWidth=3."""
    for nm, v in curves.items():
        if len(v) == 0:
            continue
        y = np.arange(1, len(v) + 1) / len(v)
        lw = 3.0 if nm in bold else 1.5
        ax.plot(v, y, lw=lw, color=colors[nm], label=labels[nm])
    ax.set_xscale("log")
    ax.set_xlabel(xlabel)
    ax.set_ylabel(ylabel)
    style_ax(ax)


# ============================== Figure 1: time ==============================
def fig_time():
    fig = plt.figure(figsize=(11, 8.5))
    gs = fig.add_gridspec(2, 2, height_ratios=[1, 0.85])
    ax_a = fig.add_subplot(gs[0, 0])
    ax_b = fig.add_subplot(gs[0, 1])
    ax_c = fig.add_subplot(gs[1, :])  # bottom full-width box plot

    # (a) lookup latency vs cache depth N (complexity curve)
    sc = read_scaling()
    for nm in ORDER_LOOKUP:
        pts = sorted((int(r["N"]), float(r["median_ns"]), float(r["median_std"]))
                     for r in sc if r["method"] == nm)
        xs = [p[0] for p in pts]
        ys = [p[1] for p in pts]
        es = [p[2] for p in pts]
        lw = 3.0 if nm in OURS else 1.5
        ax_a.errorbar(xs, ys, yerr=es, marker="o", ms=4, lw=lw,
                      capsize=3, elinewidth=1, color=COLOR[nm],
                      label=LABEL[nm])
    ax_a.set_xscale("log")
    ax_a.set_yscale("log")
    ax_a.set_xlabel("Cache depth N (packets)")
    ax_a.set_ylabel("Lookup latency P50 (ns)")
    ax_a.set_title("(a) Lookup latency vs. cache depth", fontweight="bold")
    ax_a.annotate("FIFO O(n)", xy=(10240, 2608), xytext=(1500, 4000),
                  fontsize=8, color=COLOR["fifo"])
    ax_a.annotate("AVL O(log n)", xy=(10240, 37.3), xytext=(1800, 120),
                  fontsize=8, color=COLOR["balanced_tree"])
    ax_a.annotate("PSN mapping O(1)", xy=(10240, 10.0), xytext=(2000, 4),
                  fontsize=8, color=COLOR["psn_mapping"])
    ax_a.legend(fontsize=8, loc="upper left")
    style_ax(ax_a)

    # (b) lookup latency CDF (random access)
    curves = {nm: read_cdf(nm) for nm in ORDER_LOOKUP}
    plot_cdf(ax_b, curves, COLOR, LABEL, bold=OURS,
             xlabel="Lookup latency (ns)")
    ax_b.set_title("(b) Lookup latency CDF (random)", fontweight="bold")
    ax_b.legend(fontsize=8, loc="lower right")

    # (c) store latency box plot (64 B payload, steady state)
    store = read_long(os.path.join(BEHAV, "store_samples.csv"))
    names = ORDER_6
    data = [store[nm] for nm in names]
    bp = ax_c.boxplot(data, labels=[LABEL[nm] for nm in names],
                      widths=0.55, patch_artist=True, showfliers=True,
                      flierprops=dict(marker="o", ms=2, alpha=0.4),
                      whis=(5, 95), medianprops=dict(color="black", lw=1.4))
    for patch, nm in zip(bp["boxes"], names):
        patch.set_facecolor(COLOR[nm])
        patch.set_alpha(0.85)
        if nm in OURS:
            patch.set_edgecolor("black")
            patch.set_linewidth(1.8)
    for nm, v in zip(names, data):
        med = np.median(v)
        p99 = np.percentile(v, 99)
        ax_c.text(names.index(nm) + 1, med, "%.0f" % med, ha="center",
                  va="bottom", fontsize=7.5)
        ax_c.text(names.index(nm) + 1, p99, "P99=%.0f" % p99, ha="center",
                  va="bottom", fontsize=6.5, color="#7a0000")
    ax_c.set_yscale("log")
    ax_c.set_ylabel("Store latency (ns)")
    ax_c.set_title("(c) Store processing latency (64 B payload, steady state)",
                   fontweight="bold")
    ax_c.set_xticklabels([LABEL[nm] for nm in names], rotation=12, ha="right")
    style_ax(ax_c)
    ax_c.annotate("lowest median &\nnarrowest spread",
                  xy=(5.6, 86), xytext=(5.85, 20), fontsize=8,
                  color=COLOR["psn_tiered"], arrowprops=dict(arrowstyle="->",
                  color=COLOR["psn_tiered"], lw=1))

    fig.suptitle("Figure 1. Time cost: O(1) lookup and store, latency "
                 "independent of cache depth",
                 fontsize=13, fontweight="bold", y=0.99)
    fig.text(0.5, 0.005,
             "Conditions: N=10240, 5 repeats, seed=42, batched rdtsc (B=128); "
             "error bars = std over repeats",
             ha="center", fontsize=8, style="italic")
    fig.tight_layout(rect=[0, 0.02, 1, 0.96])
    save(fig, "fig1_time")


# ============================== Figure 2: space ==============================
def fig_space():
    fig, axes = plt.subplots(2, 2, figsize=(11, 8.5))

    ss = read_space_summary()
    sizes = sorted({int(r["packet_size"]) for r in ss})

    def series(method):
        d = {int(r["packet_size"]): r for r in ss if r["method"] == method}
        return [d[s] for s in sizes]

    # (a) space utilization vs packet size
    ax = axes[0, 0]
    for nm in ORDER_SPACE:
        sr = series(nm)
        ys = [float(r["utilization"]) * 100 for r in sr]
        ls = "--" if nm == "contiguous" else "-"
        lw = 3.0 if nm in OURS else 1.5
        ax.plot(sizes, ys, marker="o", ms=3.5, lw=lw, ls=ls,
                color=SPACE_COLOR[nm], label=SPACE_LABEL[nm])
    ax.set_xlabel("Packet size L (bytes)")
    ax.set_ylabel("Space utilization (%)")
    ax.set_title("(a) Space utilization vs. packet size", fontweight="bold")
    ax.set_ylim(0, 105)
    ax.legend(fontsize=7, loc="lower right")
    style_ax(ax)

    # (b) average bytes per packet vs packet size (log-log)
    ax = axes[0, 1]
    for nm in ORDER_SPACE:
        sr = series(nm)
        ys = [float(r["avg_bytes_per_pkt"]) for r in sr]
        ls = "--" if nm == "contiguous" else "-"
        lw = 3.0 if nm in OURS else 1.5
        ax.plot(sizes, ys, marker="o", ms=3.5, lw=lw, ls=ls,
                color=SPACE_COLOR[nm], label=SPACE_LABEL[nm])
    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_xlabel("Packet size L (bytes, log scale)")
    ax.set_ylabel("Avg. bytes per packet (log scale)")
    ax.set_title("(b) Per-packet allocation vs. packet size (log-log)",
                 fontweight="bold")
    ax.legend(fontsize=7, loc="lower right")
    style_ax(ax)

    # (c) space utilization vs number of connections K
    ax = axes[1, 0]
    mf = read_space_multiflow()
    for key, label, color in [("vary_K", "PSN dynamic", "#6baed6"),
                              ("vary_K_tiered", "PSN tiered (ours)", "#08306b")]:
        pts = sorted((int(r["K"]), float(r["utilization"]) * 100)
                     for r in mf if r["scenario"] == key)
        lw = 3.0 if "tiered" in key else 1.5
        ax.plot([p[0] for p in pts], [p[1] for p in pts], marker="o", ms=4,
                lw=lw, color=color, label=label)
    ax.set_xscale("log")
    ax.set_xlabel("Concurrent connections K")
    ax.set_ylabel("Space utilization (%)")
    ax.set_title("(c) Space utilization vs. connections", fontweight="bold")
    ax.set_ylim(0, 105)
    ax.legend(fontsize=8)
    style_ax(ax)

    # (d) per-packet space overhead vs cache depth npp
    ax = axes[1, 1]
    for key, label, color in [("amortize", "PSN dynamic", "#6baed6"),
                              ("amortize_tiered", "PSN tiered (ours)",
                               "#08306b")]:
        pts = sorted((int(r["npp"]),
                      float(r["allocated_bytes"]) / float(r["total_packets"]))
                     for r in mf if r["scenario"] == key)
        lw = 3.0 if "tiered" in key else 1.5
        ax.plot([p[0] for p in pts], [p[1] for p in pts], marker="o", ms=4,
                lw=lw, color=color, label=label)
    ax.set_xscale("log")
    ax.set_xlabel("Cache depth npp (packets)")
    ax.set_ylabel("Space overhead per packet (bytes)")
    ax.set_title("(d) Per-packet space overhead vs. cache depth",
                 fontweight="bold")
    ax.legend(fontsize=8)
    style_ax(ax)

    fig.suptitle("Figure 2. Space cost: tiered blocks remove the fixed-5 KB "
                 "small-packet waste",
                 fontsize=13, fontweight="bold", y=0.99)
    fig.text(0.5, 0.005,
             "Conditions: N=10240 per connection; tier boundaries "
             "{256,512,1024,1536,2048,4096} B; (b) is log-log",
             ha="center", fontsize=8, style="italic")
    fig.tight_layout(rect=[0, 0.02, 1, 0.96])
    save(fig, "fig2_space")


# ============================== Figure 3: behavior ==========================
def fig_behavior():
    fig, axes = plt.subplots(1, 3, figsize=(15, 4.8))

    # (a) Experiment A: zero-reorder (horizontal bar, log x)
    ax = axes[0]
    zr = read_zero_reorder()
    names = ORDER_6
    avg = [zr[nm][0] for nm in names]
    std = [zr[nm][1] for nm in names]
    disp = [max(c, 1.0) for c in avg]  # log(0) guard
    y = np.arange(len(names))
    bars = ax.barh(y, disp, height=0.6, color=[COLOR[nm] for nm in names],
                   edgecolor="white", linewidth=0.4,
                   xerr=[max(s, 1e-6) for s in std], capsize=3, error_kw=dict(
                   lw=0.8))
    for yi, c, nm in zip(y, avg, names):
        txt = "0 (zero reorder)" if c == 0.0 else ("%.1f" % c)
        bold = c == 0.0
        ax.text(disp[names.index(nm)] * 1.15, yi, txt, va="center",
                ha="left", fontsize=8, fontweight="bold" if bold else "normal",
                color=COLOR[nm])
    ax.set_yticks(y)
    ax.set_yticklabels([LABEL[nm] for nm in names], fontsize=8)
    ax.set_xscale("log")
    ax.set_xlabel("Average PSN comparisons per retrieve (log scale)")
    ax.set_title("(a) Zero reorder: O(1) index, 0 comparisons",
                 fontweight="bold")
    ax.grid(axis="x", alpha=0.3, linewidth=0.5)
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)

    # (b) Experiment B: ordered-retrieve latency CDF
    ax = axes[1]
    retr = read_long(os.path.join(BEHAV, "retrieve_samples.csv"))
    plot_cdf(ax, {nm: retr[nm] for nm in names if nm in retr}, COLOR, LABEL,
             bold=OURS, xlabel="Retrieve latency (ns)",
             ylabel="Cumulative probability")
    # annotate P50/P90/P99 on the tiered curve
    if "psn_tiered" in retr:
        v = retr["psn_tiered"]
        for p, tag in [(50, "P50"), (90, "P90"), (99, "P99")]:
            q = np.percentile(v, p)
            ax.axvline(q, color="#08306b", lw=0.8, ls=":", alpha=0.7)
            ax.text(q, 0.98, tag, rotation=90, va="top", ha="right",
                    fontsize=7, color="#08306b")
    ax.set_title("(b) Ordered-retrieve latency CDF", fontweight="bold")
    ax.legend(fontsize=7.5, loc="lower right")

    # (c) Experiment C: deterministic memory (malloc per overwrite store)
    ax = axes[2]
    dm = read_deterministic_mem()
    psn_names = ["psn_fixed", "psn_dynamic", "psn_tiered"]
    mall = [dm[nm][0] for nm in psn_names]
    msd = [dm[nm][1] for nm in psn_names]
    x = np.arange(len(psn_names))
    bars = ax.bar(x, mall, 0.5, color=[COLOR[nm] for nm in psn_names],
                  edgecolor="white", linewidth=0.4, yerr=msd, capsize=3,
                  error_kw=dict(lw=0.8))
    for xi, v, nm in zip(x, mall, psn_names):
        txt = "0 (deterministic memory)" if v == 0.0 else "%.2f" % v
        ax.text(xi, v + 0.04, txt, ha="center", va="bottom", fontsize=8,
                fontweight="bold" if v == 0.0 else "normal",
                color=COLOR[nm])
    ax.set_ylim(0, 1.3)
    ax.set_xticks(x)
    ax.set_xticklabels([LABEL[nm] for nm in psn_names], fontsize=8)
    ax.set_ylabel("malloc calls per overwrite store")
    ax.set_title("(c) Deterministic memory: 0 malloc in steady state",
                 fontweight="bold")
    ax.grid(axis="y", alpha=0.3, linewidth=0.5)
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)

    fig.suptitle("Figure 3. Behavior: zero reorder, O(1) ordered retrieve, "
                 "deterministic memory",
                 fontsize=13, fontweight="bold", y=1.02)
    fig.text(0.5, 0.01,
             "Conditions: N=10240 out-of-order store, ordered retrieve; "
             "seed=42; 5 repeats; N*10 overwrite stores for (c)",
             ha="center", fontsize=8, style="italic")
    fig.tight_layout(rect=[0, 0.03, 1, 0.96])
    save(fig, "fig3_behavior")


if __name__ == "__main__":
    fig_time()
    fig_space()
    fig_behavior()
    print("\nAll figures written to:", OUT)

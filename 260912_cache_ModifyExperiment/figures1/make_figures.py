#!/usr/bin/env python3
"""
ICASSP 2027 paper figures -- SIGCOMM-style vector PDF.

Reads existing CSV data (no experiments re-run) and produces three combined
figures (each with sub-panels) as vector PDFs.

  fig1_time.pdf      -- Time overhead   (lookup scaling / lookup CDF / store boxplot)
  fig2_space.pdf     -- Space overhead  (util & bytes vs packet size, util vs K, overhead vs depth)
  fig3_behavior.pdf  -- Behavior & determinism (zero-reorder, retrieval CDF, malloc/store)

Data sources (all relative to the experiment root):
  psn_lookup_benchmark/out/scaling.csv          -> fig1a
  psn_lookup_benchmark/out/cdf_<m>_random.csv   -> fig1b
  psn_lookup_benchmark/out/summary.csv          -> fig1b P50/P90/P99 labels
  psn_behavior_bench/out/store_samples.csv      -> fig1c
  psn_space_bench/out/space_summary.csv         -> fig2a / fig2b
  psn_space_bench/out/space_multiflow.csv       -> fig2c / fig2d
  psn_behavior_bench/out/zero_reorder.csv       -> fig3a
  psn_behavior_bench/out/retrieve_samples.csv   -> fig3b
  psn_behavior_bench/out/deterministic_mem.csv  -> fig3c
"""

import os
import csv
from collections import defaultdict

import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.ticker as mticker

# --------------------------------------------------------------------------- #
# Paths
# --------------------------------------------------------------------------- #
HERE = os.path.dirname(os.path.abspath(__file__))
BASE = os.path.dirname(HERE)   # experiment root (260912_cache_ModifyExperiment)
OUT = HERE                     # figures1/

LK = os.path.join(BASE, "psn_lookup_benchmark", "out")
BH = os.path.join(BASE, "psn_behavior_bench", "out")
SP = os.path.join(BASE, "psn_space_bench", "out")

# --------------------------------------------------------------------------- #
# Global style  (SIGCOMM / IEEE: clean, colourblind-safe, thin marks, serif)
# --------------------------------------------------------------------------- #
plt.rcParams.update({
    "font.family": "serif",
    "font.serif": ["Liberation Serif", "Times New Roman", "DejaVu Serif"],
    "mathtext.fontset": "stix",
    "mathtext.rm": "serif",
    "pdf.fonttype": 42,           # embed TrueType -> editable text in PDF
    "ps.fonttype": 42,
    "svg.fonttype": "none",
    "axes.linewidth": 0.6,
    "axes.edgecolor": "#666666",
    "axes.labelcolor": "#111111",
    "text.color": "#111111",
    "xtick.color": "#333333",
    "ytick.color": "#333333",
    "xtick.direction": "out",
    "ytick.direction": "out",
    "legend.frameon": False,
    "legend.fontsize": 7.0,
    "legend.handlelength": 1.4,
    "legend.handletextpad": 0.5,
    "legend.borderpad": 0.3,
    "legend.labelspacing": 0.25,
    "axes.labelsize": 8.5,
    "xtick.labelsize": 7.2,
    "ytick.labelsize": 7.2,
})

GRID_KW = dict(color="#d8d8d8", linewidth=0.5, linestyle="-", alpha=0.8, zorder=0)

# --------------------------------------------------------------------------- #
# Method metadata: names, colourblind-safe colours, markers
# --------------------------------------------------------------------------- #
NAME = {
    "fifo": "FIFO Queue",
    "chained_hash": "Chained Hash",
    "balanced_tree": "Balanced Tree",
    "psn_mapping": "PSN Mapping (Ours)",
    "psn_fixed": "PSN Fixed 5 KB",
    "psn_dynamic": "PSN Simple Dynamic",
    "psn_tiered": "PSN Tiered (Ours)",
    "psn_map_fixed": "PSN Fixed 5 KB",
    "psn_map_dynamic": "PSN Simple Dynamic",
    "psn_map_tiered": "PSN Tiered (Ours)",
    "contiguous": "Contiguous (ideal)",
}

# Colourblind-safe: baselines in warm/gray, PSN family in a blue ramp,
# "ours" in the darkest blue.  Blues are ColorBrewer 'Blues' (CVD-safe by value).
COLOR = {
    "fifo": "#8c8c8c",
    "chained_hash": "#e69f00",
    "balanced_tree": "#009e73",
    "psn_mapping": "#08519c",
    "psn_fixed": "#9ecae1",
    "psn_dynamic": "#4292c6",
    "psn_tiered": "#08519c",
    "psn_map_fixed": "#9ecae1",
    "psn_map_dynamic": "#4292c6",
    "psn_map_tiered": "#08519c",
    "contiguous": "#222222",
}

MARKER = {
    "fifo": "s", "chained_hash": "^", "balanced_tree": "D",
    "psn_mapping": "o",
    "psn_fixed": "s", "psn_dynamic": "^", "psn_tiered": "o",
    "psn_map_fixed": "s", "psn_map_dynamic": "^", "psn_map_tiered": "o",
    "contiguous": "",
}

# 7-line plot order for the space figure (reference first, ours last / on top)
SPACE_ORDER = ["contiguous", "fifo", "chained_hash", "balanced_tree",
               "psn_map_fixed", "psn_map_dynamic", "psn_map_tiered"]


def line_style(m):
    """Return (linestyle, linewidth, markersize, zorder, alpha) for a line."""
    if m in ("psn_tiered", "psn_map_tiered", "psn_mapping"):
        return ("-", 2.2, 5.0, 10, 1.0)          # ours: bold
    if m == "contiguous":
        return ((0, (5, 3)), 1.3, 0.0, 4, 1.0)   # reference: dashed
    if m in ("fifo", "chained_hash", "balanced_tree"):
        return ("-", 1.2, 3.6, 3, 0.85)          # baselines: thin / muted
    return ("-", 1.6, 4.2, 5, 1.0)               # PSN fixed / dynamic


# --------------------------------------------------------------------------- #
# Small helpers
# --------------------------------------------------------------------------- #
def read_csv(path):
    with open(path) as f:
        return list(csv.DictReader(f))


def load_2col_by_method(path, keycol, valcol):
    """{method: np.array(values)} from a two-column csv."""
    d = defaultdict(list)
    for r in read_csv(path):
        d[r[keycol]].append(float(r[valcol]))
    return {k: np.array(v) for k, v in d.items()}


def ecdf(x):
    x = np.sort(np.asarray(x, dtype=float))
    return x, np.arange(1, len(x) + 1) / len(x)


def panel_label(ax, s):
    ax.text(0.0, 1.02, s, transform=ax.transAxes, fontsize=9.5,
            fontweight="bold", va="bottom", ha="left")


def style_axes(ax):
    ax.grid(True, which="major", **GRID_KW)
    ax.set_axisbelow(True)
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)


def save(fig, name):
    fig.savefig(os.path.join(OUT, name + ".pdf"), format="pdf", bbox_inches="tight")
    fig.savefig(os.path.join("/tmp/figprev", name + ".png"), dpi=200,
                bbox_inches="tight", facecolor="white")


os.makedirs("/tmp/figprev", exist_ok=True)


# =========================================================================== #
# FIGURE 1 -- Time overhead
# =========================================================================== #
def fig1():
    fig = plt.figure(figsize=(7.0, 4.7))
    gs = fig.add_gridspec(2, 2, height_ratios=[1.0, 1.05],
                          hspace=0.45, wspace=0.35)
    axa = fig.add_subplot(gs[0, 0])
    axb = fig.add_subplot(gs[0, 1])
    axc = fig.add_subplot(gs[1, :])

    # ---- (a) lookup latency vs cache depth N ----------------------------- #
    rows = read_csv(os.path.join(LK, "scaling.csv"))
    by = defaultdict(list)
    for r in rows:
        by[r["method"]].append((float(r["N"]), float(r["median_ns"]),
                                float(r["median_std"])))
    for m in ["fifo", "chained_hash", "balanced_tree", "psn_mapping"]:
        pts = sorted(by[m])
        x = [p[0] for p in pts]
        y = [p[1] for p in pts]
        e = [p[2] for p in pts]
        ls, lw, ms, zo, al = line_style(m)
        axa.errorbar(x, y, yerr=e, color=COLOR[m], marker=MARKER[m],
                     markersize=ms, linewidth=lw, linestyle=ls, capsize=1.5,
                     elinewidth=0.8, capthick=0.8, zorder=zo, alpha=al,
                     markerfacecolor=COLOR[m], markeredgecolor="white",
                     markeredgewidth=0.6, label=NAME[m])
    axa.set_xscale("log"); axa.set_yscale("log")
    axa.set_xlim(400, 16000); axa.set_ylim(7, 6000)
    axa.xaxis.set_major_locator(mticker.FixedLocator([512, 1024, 2048, 4096, 8192, 10240]))
    axa.xaxis.set_major_formatter(mticker.FixedFormatter(["512", "1K", "2K", "4K", "8K", "10K"]))
    axa.yaxis.set_major_locator(mticker.FixedLocator([10, 100, 1000]))
    axa.yaxis.set_major_formatter(mticker.FixedFormatter(["10", "100", "1K"]))
    axa.set_xlabel("Cache depth N")
    axa.set_ylabel("Median lookup latency (ns)")
    axa.text(15600, 12, "O(1) flat", color="#08519c", fontsize=7.5,
             va="bottom", ha="right", fontstyle="italic")
    axa.text(15600, 50, "O(log n)", color="#009e73", fontsize=7.5,
             va="bottom", ha="right", fontstyle="italic")
    axa.text(15600, 2800, "O(n)", color="#8c8c8c", fontsize=7.5,
             va="bottom", ha="right", fontstyle="italic")
    style_axes(axa)
    panel_label(axa, "(a)")

    # ---- (b) CDF of lookup latency (random access) ------------------------ #
    summary = {(r["method"], r["pattern"]): r
               for r in read_csv(os.path.join(LK, "summary.csv"))}
    for m in ["fifo", "chained_hash", "balanced_tree", "psn_mapping"]:
        x, y = ecdf(np.loadtxt(os.path.join(LK, f"cdf_{m}_random.csv"), skiprows=1))
        ls, lw, ms, zo, al = line_style(m)
        axb.plot(x, y, color=COLOR[m], linewidth=lw, linestyle=ls, zorder=zo,
                 alpha=al, label=NAME[m])
    s = summary[("psn_mapping", "random")]
    psn_x = np.loadtxt(os.path.join(LK, "cdf_psn_mapping_random.csv"), skiprows=1)
    q = {"P50": float(s["p50_ns"]), "P90": float(s["p90_ns"]), "P99": float(s["p99_ns"])}
    for i, (lab, v) in enumerate(q.items()):
        axb.axvline(v, color="#08519c", linestyle=":", linewidth=0.8, alpha=0.7, zorder=5)
        axb.text(v, 0.97 - 0.08 * i, lab, color="#08519c", fontsize=6.4,
                 ha="left", va="top")
        axb.plot(v, np.interp(v, *ecdf(psn_x)), "o", color="#08519c",
                 markersize=2.5, zorder=6)
    axb.set_xscale("log")
    axb.set_xlim(8, 40000)
    axb.xaxis.set_major_locator(mticker.FixedLocator([10, 100, 1000, 10000]))
    axb.xaxis.set_major_formatter(mticker.FixedFormatter(["10", "100", "1K", "10K"]))
    axb.set_ylim(0, 1.0)
    axb.set_yticks([0, 0.5, 1.0])
    axb.set_xlabel("Lookup latency (ns)")
    axb.set_ylabel("Cumulative probability")
    style_axes(axb)
    panel_label(axb, "(b)")

    # ---- (c) store latency distribution (box plot) ------------------------ #
    store = load_2col_by_method(os.path.join(BH, "store_samples.csv"),
                                "method", "latency_ns")
    order = ["fifo", "chained_hash", "balanced_tree",
             "psn_fixed", "psn_dynamic", "psn_tiered"]
    labels = ["FIFO\nQueue", "Chained\nHash", "Balanced\nTree",
              "PSN\nFixed", "PSN\nDynamic", "PSN\nTiered"]
    data = [store[m] for m in order]
    bp = axc.boxplot(data, whis=(5, 95), widths=0.6, patch_artist=True,
                     showfliers=True,
                     flierprops=dict(marker="o", markersize=2.2,
                                     markerfacecolor="none", markeredgewidth=0.6,
                                     alpha=0.5),
                     medianprops=dict(linewidth=1.6, solid_capstyle="butt"),
                     whiskerprops=dict(linewidth=0.9),
                     capprops=dict(linewidth=0.9))
    for i, m in enumerate(order):
        c = COLOR[m]
        patch = bp["boxes"][i]
        patch.set_facecolor(c); patch.set_alpha(0.20)
        patch.set_edgecolor(c); patch.set_linewidth(1.1)
        if m == "psn_tiered":
            patch.set_alpha(0.40); patch.set_linewidth(2.0)
        bp["medians"][i].set_color(c)
        bp["whiskers"][2 * i].set_color(c); bp["whiskers"][2 * i + 1].set_color(c)
        bp["caps"][2 * i].set_color(c); bp["caps"][2 * i + 1].set_color(c)
        bp["fliers"][i].set_markeredgecolor(c)
    axc.set_yscale("log")
    axc.set_ylim(60, 3000)
    axc.yaxis.set_major_locator(mticker.FixedLocator([100, 300, 1000]))
    axc.yaxis.set_major_formatter(mticker.FixedFormatter(["100", "300", "1K"]))
    axc.set_xticks(range(1, 7))
    axc.set_xticklabels(labels, fontsize=7.6)
    for tick in axc.get_xticklabels():
        if "Tiered" in tick.get_text():
            tick.set_color("#08519c"); tick.set_fontweight("bold")
    axc.set_ylabel("Store latency (ns)")
    style_axes(axc)
    panel_label(axc, "(c)")

    # shared legend for panels (a) and (b) (same four methods)
    handles, labels = axa.get_legend_handles_labels()
    fig.legend(handles, labels, loc="upper center", bbox_to_anchor=(0.5, 0.99),
               ncol=4, fontsize=7.2, columnspacing=1.2, handletextpad=0.4,
               handlelength=1.3)
    fig.subplots_adjust(left=0.08, right=0.97, top=0.87, bottom=0.09,
                        wspace=0.32, hspace=0.50)
    return fig


# =========================================================================== #
# FIGURE 2 -- Space overhead
# =========================================================================== #
def fig2():
    fig, axes = plt.subplots(2, 2, figsize=(7.0, 4.9))
    (axa, axb), (axc, axd) = axes

    rows = read_csv(os.path.join(SP, "space_summary.csv"))
    by_util = defaultdict(list)
    by_bytes = defaultdict(list)
    for r in rows:
        ps = float(r["packet_size"])
        by_util[r["method"]].append((ps, float(r["utilization"]) * 100.0))
        by_bytes[r["method"]].append((ps, float(r["avg_bytes_per_pkt"])))

    def plot_lines(ax, data):
        for m in SPACE_ORDER:
            pts = sorted(data[m])
            x = [p[0] for p in pts]; y = [p[1] for p in pts]
            ls, lw, ms, zo, al = line_style(m)
            kw = dict(color=COLOR[m], linewidth=lw, linestyle=ls, zorder=zo,
                      alpha=al, label=NAME[m])
            if MARKER[m]:
                kw.update(marker=MARKER[m], markersize=ms,
                          markerfacecolor=COLOR[m], markeredgecolor="white",
                          markeredgewidth=0.6)
            ax.plot(x, y, **kw)

    # ---- (a) space utilization vs packet size ---------------------------- #
    plot_lines(axa, by_util)
    axa.axvspan(1024, 4096, color="#08519c", alpha=0.06, zorder=0)
    axa.text(1400, 3, "Typical RDMA data packets (1024–4096 B)",
             fontsize=6.4, color="#08519c", ha="center", va="bottom", alpha=0.9)
    axa.set_xlim(0, 4096); axa.set_ylim(0, 100)
    axa.set_xticks([0, 1024, 2048, 3072, 4096])
    axa.set_yticks([0, 20, 40, 60, 80, 100])
    axa.set_xlabel("Packet size (B)")
    axa.set_ylabel("Space utilization (%)")
    axa.legend(loc="lower right", ncol=1, fontsize=6.2)
    style_axes(axa)
    panel_label(axa, "(a)")

    # ---- (b) average bytes per packet vs packet size ---------------------- #
    plot_lines(axb, by_bytes)
    axb.set_xscale("log"); axb.set_yscale("log")
    axb.set_xlim(60, 5000); axb.set_ylim(80, 6000)
    axb.xaxis.set_major_locator(mticker.FixedLocator([64, 128, 256, 512, 1024, 2048, 4096]))
    axb.xaxis.set_major_formatter(mticker.FixedFormatter(["64", "128", "256", "512", "1K", "2K", "4K"]))
    axb.yaxis.set_major_locator(mticker.FixedLocator([100, 300, 1000, 3000]))
    axb.yaxis.set_major_formatter(mticker.FixedFormatter(["100", "300", "1K", "3K"]))
    axb.set_xlabel("Packet size (B)")
    axb.set_ylabel("Avg. allocated bytes / packet (B)")
    axb.legend(loc="upper left", ncol=1, fontsize=6.2, bbox_to_anchor=(0.01, 0.93))
    style_axes(axb)
    panel_label(axb, "(b)")

    # ---- (c) utilization vs number of connections K ----------------------- #
    mf = read_csv(os.path.join(SP, "space_multiflow.csv"))
    for sc in ["vary_K", "vary_K_tiered"]:
        m = "psn_map_dynamic" if sc == "vary_K" else "psn_map_tiered"
        pts = sorted([(float(r["K"]), float(r["utilization"]) * 100.0)
                      for r in mf if r["scenario"] == sc])
        x = [p[0] for p in pts]; y = [p[1] for p in pts]
        ls, lw, ms, zo, al = line_style(m)
        axc.plot(x, y, color=COLOR[m], marker=MARKER[m], markersize=ms,
                 markerfacecolor=COLOR[m], markeredgecolor="white",
                 markeredgewidth=0.6, linewidth=lw, linestyle=ls, zorder=zo,
                 alpha=al, label=NAME[m])
    axc.set_xscale("log")
    axc.set_xlim(0.8, 300); axc.set_ylim(88, 102)
    axc.xaxis.set_major_locator(mticker.FixedLocator([1, 8, 64, 256]))
    axc.xaxis.set_major_formatter(mticker.FixedFormatter(["1", "8", "64", "256"]))
    axc.set_yticks([90, 94, 98])
    axc.text(0.9, 89.2, "flat across K", color="#333333", fontsize=7.2,
             ha="left", va="bottom", fontstyle="italic")
    axc.set_xlabel("Concurrent connections K")
    axc.set_ylabel("Space utilization (%)")
    axc.legend(loc="upper right", fontsize=6.6)
    style_axes(axc)
    panel_label(axc, "(c)")

    # ---- (d) per-packet overhead vs cache occupancy depth ----------------- #
    for sc in ["amortize", "amortize_tiered"]:
        m = "psn_map_dynamic" if sc == "amortize" else "psn_map_tiered"
        pts = sorted([(float(r["npp"]),
                       float(r["allocated_bytes"]) / float(r["total_packets"]))
                      for r in mf if r["scenario"] == sc])
        x = [p[0] for p in pts]; y = [p[1] for p in pts]
        ls, lw, ms, zo, al = line_style(m)
        axd.plot(x, y, color=COLOR[m], marker=MARKER[m], markersize=ms,
                 markerfacecolor=COLOR[m], markeredgecolor="white",
                 markeredgewidth=0.6, linewidth=lw, linestyle=ls, zorder=zo,
                 alpha=al, label=NAME[m])
    axd.set_xscale("log")
    axd.set_xlim(200, 12000); axd.set_ylim(1000, 3200)
    axd.xaxis.set_major_locator(mticker.FixedLocator([256, 512, 1024, 2048, 4096, 10240]))
    axd.xaxis.set_major_formatter(mticker.FixedFormatter(["256", "512", "1K", "2K", "4K", "10K"]))
    axd.yaxis.set_major_locator(mticker.FixedLocator([1000, 1500, 2000, 2500, 3000]))
    axd.yaxis.set_major_formatter(mticker.FixedFormatter(["1.0K", "1.5K", "2.0K", "2.5K", "3.0K"]))
    axd.set_xlabel("Cache occupancy depth (packets)")
    axd.set_ylabel("Allocated bytes / packet (B)")
    axd.legend(loc="upper right", fontsize=6.6)
    style_axes(axd)
    panel_label(axd, "(d)")

    fig.tight_layout(w_pad=1.8, h_pad=2.4)
    return fig


# =========================================================================== #
# FIGURE 3 -- Behavior & determinism
# =========================================================================== #
def fig3():
    fig = plt.figure(figsize=(7.0, 4.4))
    gs = fig.add_gridspec(2, 2, height_ratios=[1.0, 0.8],
                          hspace=0.45, wspace=0.35)
    axa = fig.add_subplot(gs[0, 0])
    axb = fig.add_subplot(gs[0, 1])
    axc = fig.add_subplot(gs[1, :])

    # ---- (a) average PSN comparisons per retrieval (zero reorder) -------- #
    zr = {r["method"]: float(r["avg_cmp"])
          for r in read_csv(os.path.join(BH, "zero_reorder.csv"))}
    order = ["psn_tiered", "psn_dynamic", "psn_fixed", "chained_hash",
             "balanced_tree", "fifo"]
    names = [NAME[m] for m in order]
    vals = [zr[m] for m in order]
    cols = [COLOR[m] for m in order]
    ypos = np.arange(len(order))
    axa.set_xscale("log")
    axa.set_xlim(0.2, 20000)
    for y, v, c, m in zip(ypos, vals, cols, order):
        if v > 0:
            axa.barh(y, v, height=0.62, color=c, alpha=0.85, edgecolor="none",
                     zorder=3)
            lab = f"{v:,.1f}" if v > 100 else f"{v:g}"
            axa.text(v * 1.35, y, lab, va="center", ha="left", fontsize=7.0,
                     color="#333333")
        else:
            axa.barh(y, 0.24, height=0.62, color=c, alpha=0.9, edgecolor="none",
                     zorder=3)
            txt = "0  (zero reorder)" if m == "psn_tiered" else "0"
            axa.text(0.30, y, txt, va="center", ha="left", fontsize=7.4,
                     fontweight="bold", color=c)
    axa.set_yticks(ypos)
    axa.set_yticklabels(names, fontsize=7.4)
    for lab in axa.get_yticklabels():
        if "Tiered" in lab.get_text():
            lab.set_color("#08519c"); lab.set_fontweight("bold")
    axa.xaxis.set_major_locator(mticker.FixedLocator([1, 10, 100, 1000, 10000]))
    axa.xaxis.set_major_formatter(mticker.FixedFormatter(["1", "10", "100", "1K", "10K"]))
    axa.set_xlabel("Avg. PSN comparisons / retrieval")
    axa.set_ylim(-0.7, len(order) - 0.3)
    axa.grid(True, which="major", axis="x", **GRID_KW)
    axa.set_axisbelow(True)
    for s in ("top", "right"):
        axa.spines[s].set_visible(False)
    axa.spines["left"].set_visible(False)
    panel_label(axa, "(a)")

    # ---- (b) CDF of retrieval latency ------------------------------------- #
    ret = load_2col_by_method(os.path.join(BH, "retrieve_samples.csv"),
                              "method", "latency_ns")
    for m in ["fifo", "chained_hash", "balanced_tree",
              "psn_fixed", "psn_dynamic", "psn_tiered"]:
        x, y = ecdf(ret[m])
        ls, lw, ms, zo, al = line_style(m)
        axb.plot(x, y, color=COLOR[m], linewidth=lw, linestyle=ls, zorder=zo,
                 alpha=al, label=NAME[m])
    t = ret["psn_tiered"]
    qs = {"P50": np.percentile(t, 50), "P90": np.percentile(t, 90),
          "P99": np.percentile(t, 99)}
    for i, (lab, v) in enumerate(qs.items()):
        axb.axvline(v, color="#08519c", linestyle=":", linewidth=0.8,
                    alpha=0.7, zorder=5)
        axb.text(v, 0.97 - 0.09 * i, lab, color="#08519c", fontsize=6.4,
                 ha="left", va="top")
    axb.set_xscale("log")
    axb.set_xlim(30, 50000)
    axb.xaxis.set_major_locator(mticker.FixedLocator([100, 1000, 10000]))
    axb.xaxis.set_major_formatter(mticker.FixedFormatter(["100", "1K", "10K"]))
    axb.set_ylim(0, 1.0); axb.set_yticks([0, 0.5, 1.0])
    axb.set_xlabel("Retrieval latency (ns)")
    axb.set_ylabel("Cumulative probability")
    style_axes(axb)
    panel_label(axb, "(b)")

    # ---- (c) malloc calls per store (deterministic memory) ---------------- #
    dm = {r["method"]: float(r["malloc_per_store"])
          for r in read_csv(os.path.join(BH, "deterministic_mem.csv"))}
    order3 = ["psn_fixed", "psn_dynamic", "psn_tiered"]
    names3 = ["PSN\nFixed", "PSN\nDynamic", "PSN\nTiered"]
    vals3 = [dm[m] for m in order3]
    cols3 = [COLOR[m] for m in order3]
    xp = np.arange(3)
    for x, v, c in zip(xp, vals3, cols3):
        if v > 0:
            axc.bar(x, v, width=0.5, color=c, alpha=0.85, edgecolor="none", zorder=3)
            axc.text(x, v + 0.05, "1.0", ha="center", va="bottom", fontsize=7.6)
        else:
            axc.bar(x, 0.05, width=0.5, color=c, alpha=0.9, edgecolor="none", zorder=3)
            axc.text(x, 0.13, "0", ha="center", va="bottom", fontsize=8.2,
                     fontweight="bold", color="#08519c")
            axc.text(x, 0.28, "(deterministic memory)", ha="center", va="bottom",
                     fontsize=6.4, fontstyle="italic", color="#555555")
    axc.set_xticks(xp); axc.set_xticklabels(names3, fontsize=7.4)
    for lab in axc.get_xticklabels():
        if "Tiered" in lab.get_text():
            lab.set_color("#08519c"); lab.set_fontweight("bold")
    axc.set_xlim(-0.7, 2.7)
    axc.set_ylim(0, 1.25)
    axc.set_yticks([0, 0.5, 1.0])
    axc.set_ylabel("Malloc calls / store")
    style_axes(axc)
    panel_label(axc, "(c)")

    # shared top legend (covers all 6 methods used across panels)
    handles, labels = axb.get_legend_handles_labels()
    fig.legend(handles, labels, loc="upper center", bbox_to_anchor=(0.5, 0.99),
               ncol=6, fontsize=7.2, columnspacing=0.9, handletextpad=0.4,
               handlelength=1.2)
    fig.subplots_adjust(left=0.08, right=0.97, top=0.87, bottom=0.11,
                        wspace=0.32, hspace=0.55)
    return fig


# --------------------------------------------------------------------------- #
def main():
    for fn, name in [(fig1, "fig1_time"), (fig2, "fig2_space"), (fig3, "fig3_behavior")]:
        f = fn()
        save(f, name)
        plt.close(f)
    print("done ->", OUT)


if __name__ == "__main__":
    main()

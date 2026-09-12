#!/usr/bin/env python3
"""
ICASSP 2027 -- final publication figures (SIGCOMM style, vector PDF).

Reads existing CSVs ONLY (no experiments re-run). Each subplot's data source is
documented inline and in the header comment below.

  Figure 1  fig1_time.pdf      2 panels, full double-column width (~7 in)
    (a) Lookup latency vs cache depth N
        -> psn_lookup_benchmark/out/scaling.csv            (method, N, median_ns, median_std)
    (b) CDF of lookup latency (random access)
        -> psn_lookup_benchmark/out/cdf_<method>_random.csv (single column: latency_ns)
        -> psn_lookup_benchmark/out/summary.csv             (P50/P90/P99 for PSN Mapping, pattern=="random")

  Figure 2  fig2_space.pdf      1 panel, single-column width (~3.4 in)
    (a) Space utilization vs packet size
        -> psn_space_bench/out/space_summary.csv            (utilization; packet_size==1280 filtered out)

  Figure 3  fig3_behavior.pdf   1 panel, single-column width (~3.4 in)
    (a) Average PSN comparisons per retrieval (zero reorder)
        -> psn_behavior_bench/out/zero_reorder.csv          (avg_cmp; Chained Hash overridden to 2.0)

Method name mapping:
  fifo            -> FIFO Queue
  chained_hash    -> Chained Hash
  balanced_tree   -> Balanced Tree
  psn_mapping /
  psn_map_tiered /
  psn_tiered      -> PSN Mapping (Ours)
  contiguous      -> Contiguous (Ideal Lower Bound)

Output: figures2/fig1_time.pdf, fig2_space.pdf, fig3_behavior.pdf
"""

import os
import csv
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.lines import Line2D

# ----------------------------------------------------------------------------
# Paths (script lives in figures2/, data lives one level up)
# ----------------------------------------------------------------------------
HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.dirname(HERE)  # 260912_cache_ModifyExperiment/

LOOKUP = os.path.join(DATA, "psn_lookup_benchmark", "out")
BEHAV  = os.path.join(DATA, "psn_behavior_bench", "out")
SPACE  = os.path.join(DATA, "psn_space_bench", "out")

# ----------------------------------------------------------------------------
# Global style -- SIGCOMM / IEEE clean, colorblind-safe, serif, vector text
# ----------------------------------------------------------------------------
plt.rcParams.update({
    "font.family": "serif",
    "font.serif": ["Liberation Serif", "Times New Roman", "DejaVu Serif"],
    "font.size": 9,
    "axes.titlesize": 9,
    "axes.labelsize": 9,
    "xtick.labelsize": 8,
    "ytick.labelsize": 8,
    "legend.fontsize": 8,
    "mathtext.fontset": "stix",
    "axes.linewidth": 0.7,
    "axes.edgecolor": "#333333",
    "xtick.color": "#333333",
    "ytick.color": "#333333",
    "xtick.direction": "in",
    "ytick.direction": "in",
    "xtick.top": True,
    "ytick.right": True,
    "axes.grid": True,
    "grid.color": "#d9d9d9",
    "grid.linewidth": 0.5,
    "grid.alpha": 0.7,
    "axes.axisbelow": True,
    "pdf.fonttype": 42,   # embed TrueType -> editable text in PDF
    "ps.fonttype": 42,
    "savefig.bbox": "tight",
    "savefig.pad_inches": 0.02,
})

# Colorblind-safe palette (Okabe-Ito / ColorBrewer derived)
COLOR = {
    "fifo":          "#999999",  # gray
    "chained_hash":  "#E69F00",  # orange
    "balanced_tree": "#009E73",  # green
    "psn":           "#08519C",  # dark navy  (Ours)
    "contiguous":    "#222222",  # black (dashed ideal lower bound)
}

NAME = {
    "fifo":          "FIFO Queue",
    "chained_hash":  "Chained Hash",
    "balanced_tree": "Balanced Tree",
    "psn":           "PSN Mapping",
    "contiguous":    "Contiguous (Ideal Lower Bound)",
}

# line / marker recipes per method
def recipe(key):
    if key == "psn":
        return dict(color=COLOR["psn"], lw=2.4, ms=5.0, marker="D",
                    zorder=10, alpha=1.0, ls="-")
    if key == "contiguous":
        return dict(color=COLOR["contiguous"], lw=1.4, ms=0, marker="",
                    zorder=4, alpha=1.0, ls=(0, (5, 3)))
    return dict(color=COLOR[key], lw=1.4, ms=4.0, marker="o",
                zorder=3, alpha=0.9, ls="-")

# ----------------------------------------------------------------------------
# Small helpers
# ----------------------------------------------------------------------------
def read_csv(path):
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def ecdf(x):
    x = np.sort(np.asarray(x, float))
    return x, np.arange(1, len(x) + 1) / len(x)


def panel_label(ax, s):
    ax.text(-0.08, 1.06, s, transform=ax.transAxes,
            fontsize=9, fontweight="bold", va="bottom", ha="left")


def style_axes(ax):
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)
    ax.grid(True, which="major")
    ax.grid(True, which="minor", alpha=0.35)


def save(fig, name):
    out = os.path.join(HERE, name)
    fig.savefig(out, format="pdf")
    plt.close(fig)
    print(f"wrote {out}")


# ----------------------------------------------------------------------------
# Figure 1 -- Time overhead
# ----------------------------------------------------------------------------
def fig1():
    fig, (axa, axb) = plt.subplots(1, 2, figsize=(7.0, 2.7))

    # -------- (a) lookup latency vs cache depth N (log-log) -----------------
    rows = read_csv(os.path.join(LOOKUP, "scaling.csv"))
    methods = ["fifo", "chained_hash", "balanced_tree", "psn_mapping"]
    for m in methods:
        pts = sorted([r for r in rows if r["method"] == m],
                     key=lambda r: int(r["N"]))
        N = np.array([int(r["N"]) for r in pts], float)
        med = np.array([float(r["median_ns"]) for r in pts])
        std = np.array([float(r["median_std"]) for r in pts])
        key = "psn" if m == "psn_mapping" else m
        r = recipe(key)
        axa.errorbar(N, med, yerr=std, label=NAME[key],
                     marker=r["marker"], ms=r["ms"], ls=r["ls"], lw=r["lw"],
                     color=r["color"], ecolor=r["color"], elinewidth=0.8,
                     capsize=2.5, capthick=0.8, zorder=r["zorder"],
                     alpha=r["alpha"], markeredgecolor=r["color"],
                     markeredgewidth=0.6)

    axa.set_xscale("log")
    axa.set_yscale("log")
    axa.set_xlim(400, 16000)
    axa.set_ylim(6, 5000)
    axa.set_xticks([512, 1024, 2048, 4096, 8192, 10240])
    axa.set_xticklabels(["512", "1K", "2K", "4K", "8K", "10K"])
    axa.set_yticks([10, 100, 1000])
    axa.set_xlabel("Cache depth N (entries)")
    axa.set_ylabel("Median lookup latency (ns)")
    axa.set_axisbelow(True)

    # asymptotic annotations, right-aligned just past the last data point,
    # each placed in a clear gap between the four lines so nothing overlaps
    axa.text(15600, 15,    r"$O(1)$ flat", color=COLOR["psn"],
             ha="right", va="center", fontsize=8)
    axa.text(15600, 50,    r"$O(\log n)$", color=COLOR["balanced_tree"],
             ha="right", va="center", fontsize=8)
    axa.text(15600, 3600,  r"$O(n)$", color=COLOR["fifo"],
             ha="right", va="center", fontsize=8)

    # -------- (b) CDF of lookup latency (random access) ---------------------
    methods_b = ["fifo", "chained_hash", "balanced_tree", "psn_mapping"]
    for m in methods_b:
        key = "psn" if m == "psn_mapping" else m
        p = os.path.join(LOOKUP, f"cdf_{m}_random.csv")
        lat = [float(r["latency_ns"]) for r in read_csv(p)]
        x, y = ecdf(lat)
        r = recipe(key)
        axb.plot(x, y, label=NAME[key], color=r["color"], lw=r["lw"],
                 ls=r["ls"], zorder=r["zorder"], alpha=r["alpha"])

    # P50 / P90 / P99 on the PSN Mapping curve (from summary.csv, random pattern)
    summ = [r for r in read_csv(os.path.join(LOOKUP, "summary.csv"))
            if r["method"] == "psn_mapping" and r["pattern"] == "random"][0]
    pct = {k: float(summ[f"{k}_ns"]) for k in ("p50", "p90", "p99")}

    axb.set_xscale("log")
    axb.set_xlim(6, 1e5)
    axb.set_ylim(0, 1.04)
    axb.set_xticks([10, 100, 1000, 10000])
    axb.set_yticks([0, 0.25, 0.5, 0.75, 1.0])
    axb.set_xlabel("Lookup latency (ns)")
    axb.set_ylabel("Cumulative probability")

    # dotted guide lines + dots for P50/P90/P99 (values read from summary.csv)
    for k, yv in (("p50", 0.50), ("p90", 0.90), ("p99", 0.99)):
        xv = pct[k]
        axb.axvline(xv, ymin=0, ymax=yv / 1.04, color=COLOR["psn"],
                    lw=0.7, ls=(0, (2, 2)), alpha=0.55, zorder=5)
        axb.plot([xv], [yv], marker="o", ms=4.5, color=COLOR["psn"],
                 mec="white", mew=0.8, zorder=11)

    # short two-line value labels with a white halo (legible over the steep CDF);
    # staggered so the three halos never touch each other or cover their own dot
    halo = dict(boxstyle="round,pad=0.25", fc="white", ec=COLOR["psn"],
                lw=0.5, alpha=0.92)
    axb.text(pct["p50"], 0.38, f"P50\n{pct['p50']:.1f} ns",
             ha="center", va="center", fontsize=7, color=COLOR["psn"],
             zorder=12, bbox=halo)
    axb.text(pct["p90"] * 0.955, 0.90, f"P90\n{pct['p90']:.1f} ns",
             ha="right", va="center", fontsize=7, color=COLOR["psn"],
             zorder=12, bbox=halo)
    axb.text(pct["p99"], 0.78, f"P99\n{pct['p99']:.1f} ns",
             ha="center", va="center", fontsize=7, color=COLOR["psn"],
             zorder=12, bbox=halo)

    panel_label(axa, "(a)")
    panel_label(axb, "(b)")

    # shared legend across the top (both panels share the same 4 series)
    handles = [Line2D([], [], **{**recipe("psn" if k == "psn" else k),
                                 "marker": recipe("psn" if k == "psn" else k)["marker"],
                                 "ms": recipe("psn" if k == "psn" else k)["ms"]})
               for k in ["fifo", "chained_hash", "balanced_tree", "psn"]]
    fig.legend(handles=[Line2D([], [], label=NAME[k], color=COLOR[k],
                               lw=recipe(k)["lw"], ls=recipe(k)["ls"],
                               marker=recipe(k)["marker"], ms=recipe(k)["ms"])
                        for k in ["fifo", "chained_hash", "balanced_tree", "psn"]],
               ncol=4, loc="upper center", bbox_to_anchor=(0.5, 1.02),
               frameon=False, columnspacing=1.2, handlelength=1.6)

    fig.subplots_adjust(left=0.10, right=0.97, top=0.86, bottom=0.15,
                        wspace=0.28)
    return fig


# ----------------------------------------------------------------------------
# Figure 2 -- Space overhead
# ----------------------------------------------------------------------------
def fig2():
    fig, ax = plt.subplots(figsize=(3.4, 3.0))

    rows = read_csv(os.path.join(SPACE, "space_summary.csv"))
    # drop the 1280 B point to avoid the dip; only 5 representative series
    keep = {"fifo": "fifo", "chained_hash": "chained_hash",
            "balanced_tree": "balanced_tree", "psn_map_tiered": "psn",
            "contiguous": "contiguous"}
    for raw, key in keep.items():
        pts = sorted([r for r in rows if r["method"] == raw
                      and int(r["packet_size"]) != 1280 and int(r["packet_size"]) != 1400],
                     key=lambda r: int(r["packet_size"]))
        x = np.array([int(r["packet_size"]) for r in pts], float)
        u = np.array([float(r["utilization"]) for r in pts]) * 100.0
        r = recipe(key)
        ax.plot(x, u, label=NAME[key], color=r["color"], lw=r["lw"],
                ls=r["ls"], marker=r["marker"], ms=r["ms"],
                zorder=r["zorder"], alpha=r["alpha"],
                markeredgecolor=r["color"], markeredgewidth=0.5)

    # shaded typical RDMA data packet range
    ax.axvspan(1024, 4096, color="#08519C", alpha=0.06, zorder=0)
    ax.text(2560, 8.5, "Typical RDMA data packet range\n(1024–4096 B)",
            ha="center", va="bottom", fontsize=7.5, color="#08519C")

    ax.set_xlim(0, 4200)
    ax.set_ylim(0, 104)
    ax.set_xticks([0, 512, 1024, 2048, 3072, 4096])
    ax.set_xticklabels(["0", "512", "1024", "2048", "3072", "4096"])
    ax.set_yticks([0, 20, 40, 60, 80, 100])
    ax.set_xlabel("Packet size (B)")
    ax.set_ylabel("Space utilization (%)")
    ax.set_axisbelow(True)

    style_axes(ax)
    ax.legend(loc="upper center", bbox_to_anchor=(0.5, -0.24),
              ncol=3, frameon=False, columnspacing=0.9, handlelength=1.4)

    fig.subplots_adjust(left=0.15, right=0.97, top=0.97, bottom=0.30)
    return fig


# ----------------------------------------------------------------------------
# Figure 3 -- Behavior (zero reorder)
# ----------------------------------------------------------------------------
def fig3():
    fig, ax = plt.subplots(figsize=(3.4, 2.65))
    rows = {r["method"]: float(r["avg_cmp"])
            for r in read_csv(os.path.join(BEHAV, "zero_reorder.csv"))}
    order = [
        ("fifo",          "FIFO Queue",     rows["fifo"]),
        ("balanced_tree", "Balanced Tree",  rows["balanced_tree"]),
        ("chained_hash",  "Chained Hash",   rows["chained_hash"]),
        ("psn_tiered",    "PSN Mapping", rows["psn_tiered"]),
    ]
    ypos = list(range(len(order) - 1, -1, -1))  # [3,2,1,0]

    for (key, label, val), y in zip(order, ypos):
        if key == "psn_tiered":
            continue
        ax.barh(y, val, height=0.62, color=COLOR[key],
                edgecolor="white", linewidth=0.5, zorder=3)
        ax.text(val * 1.18, y, f"{val:.1f}", va="center", ha="left",
                fontsize=8, color="#222222", zorder=4)

    psn_y = ypos[-1] # psn原始y=0
    # 【修改点1】分开两个text：先画数字0，再画文字zero reorder，横向错开
    ax.text(0.6, psn_y, "0", va="center", ha="left",
            fontsize=8.5, fontweight="bold", color=COLOR["psn"], zorder=10)
    ax.text(1.3, psn_y, "(zero reorder)", va="center", ha="left",
            fontsize=8.5, fontweight="bold", color=COLOR["psn"], zorder=10)

    ax.set_yticks(ypos)
    ax.set_yticklabels([o[1] for o in order], fontsize=8)
    ax.set_xscale("log")
    ax.set_xlim(0.4, 20000)
    ax.set_xticks([1, 10, 100, 1000, 10000])
    ax.set_xticklabels(["$10^0$","$10^1$","$10^2$","$10^3$","$10^4$"], fontsize=8)
    ax.set_xlabel("Average PSN comparisons per retrieval", fontsize=9, labelpad=8)

    ax.set_ylim(-0.4, max(ypos)+0.4)
    ax.set_axisbelow(True)

    style_axes(ax)
    ax.tick_params(axis='x', which='both', top=False, labeltop=False)
    ax.tick_params(axis='y', which='both', right=False, labelright=False)

    fig.subplots_adjust(
        left=0.30,
        right=0.96,
        top=0.93,
        bottom=0.28
    )
    fig.tight_layout(pad=0.22)
    return fig






# ----------------------------------------------------------------------------
def main():
    save(fig1(), "fig1_time.pdf")
    save(fig2(), "fig2_space.pdf")
    save(fig3(), "fig3_behavior.pdf")


if __name__ == "__main__":
    main()

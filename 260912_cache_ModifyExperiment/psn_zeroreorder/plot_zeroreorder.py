#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ICASSP 2027 — Experiment 3(a) scaling: average PSN comparisons vs cache depth N.

Reads psn_zeroreorder/out/zeroreorder_scaling.csv (method, N, avg_cmp, std_cmp)
and draws a SIGCOMM-style log-log line chart with std error bars.

Four methods (CSV name -> style key):
  fifo           -> FIFO Queue        (gray,  O(N))
  chained_hash   -> Chained Hash      (orange)
  balanced_tree  -> Balanced Tree     (green, O(log n))
  psn_tiered     -> PSN Mapping (Ours)(navy,  O(1), 0 comparisons)

Reuses the COLOR / NAME / recipe() / read_csv() style of figures2/make_figures.py
so the figure stays visually consistent with the rest of the paper.

Output: psn_zeroreorder/fig_zeroreorder_scaling.pdf  (vector PDF only).
"""

import os
import csv
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.lines import Line2D

HERE = os.path.dirname(os.path.abspath(__file__))
CSV_PATH = os.path.join(HERE, "out", "zeroreorder_scaling.csv")
OUT = os.path.join(HERE, "fig_zeroreorder_scaling.pdf")

# --- global style (mirrors figures2/make_figures.py) ---
plt.rcParams.update({
    "font.family": "serif",
    "font.serif": ["Liberation Serif", "Times New Roman", "DejaVu Serif"],
    "font.size": 9,
    "axes.titlesize": 9,
    "axes.labelsize": 9,
    "xtick.labelsize": 8,
    "ytick.labelsize": 8,
    "legend.fontsize": 7.5,
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
    "pdf.fonttype": 42,   # embed TrueType -> editable text in the PDF
    "ps.fonttype": 42,
    "savefig.bbox": "tight",
    "savefig.pad_inches": 0.02,
})

COLOR = {
    "fifo":          "#999999",  # gray
    "chained_hash":  "#E69F00",  # orange
    "balanced_tree": "#009E73",  # green
    "psn":           "#08519C",  # dark navy (Ours)
}

NAME = {
    "fifo":          "FIFO Queue",
    "chained_hash":  "Chained Hash",
    "balanced_tree": "Balanced Tree",
    "psn":           "PSN Mapping",
}

# line / marker recipe per method (reused from figures2/make_figures.py)
def recipe(key):
    if key == "psn":
        return dict(color=COLOR["psn"], lw=2.4, ms=5.0, marker="D",
                    zorder=10, alpha=1.0, ls="-")
    return dict(color=COLOR[key], lw=1.4, ms=4.0, marker="o",
                zorder=3, alpha=0.9, ls="-")

# reuse the project's csv reader
def read_csv(path):
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def style_axes(ax):
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)
    ax.grid(True, which="major")
    ax.grid(True, which="minor", alpha=0.35)


# CSV method name -> recipe/color key
KEY = {"fifo": "fifo", "chained_hash": "chained_hash",
       "balanced_tree": "balanced_tree", "psn_tiered": "psn"}

# log-scale display floor: psn_tiered has avg_cmp == 0, which cannot sit on a
# log axis; it is drawn at this floor and annotated with the true value "0".
FLOOR = 0.1


def main():
    rows = read_csv(CSV_PATH)
    data = {}
    for r in rows:
        data.setdefault(r["method"], []).append(
            (int(r["N"]), float(r["avg_cmp"]), float(r["std_cmp"])))
    for m in data:
        data[m].sort()

    fig, ax = plt.subplots(figsize=(3.4, 2.7))  # ICASSP single-column width

    for m in ("fifo", "chained_hash", "balanced_tree", "psn_tiered"):
        pts = data[m]
        N = np.array([p[0] for p in pts], float)
        avg = np.array([p[1] for p in pts], float)
        std = np.array([p[2] for p in pts], float)
        y = np.maximum(avg, FLOOR)  # keep 0 off the log axis
        r = recipe(KEY[m])
        ax.errorbar(N, y, yerr=std, label=NAME[KEY[m]],
                    marker=r["marker"], ms=r["ms"], ls=r["ls"], lw=r["lw"],
                    color=r["color"], ecolor=r["color"], elinewidth=0.8,
                    capsize=2.5, capthick=0.8, zorder=r["zorder"],
                    alpha=r["alpha"], markeredgecolor=r["color"],
                    markeredgewidth=0.6)

    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_xlim(400, 22000)
    ax.set_ylim(0.05, 10000)
    ax.set_xticks([512, 1024, 2048, 4096, 8192, 10240])
    ax.set_xticklabels(["512", "1K", "2K", "4K", "8K", "10K"])
    ax.set_yticks([0.1, 1, 10, 100, 1000, 10000])
    ax.set_yticklabels(["0.1", "1", "10", "100", "1000", "10000"])
    ax.set_xlabel("Cache depth N (entries)")
    ax.set_ylabel("Average PSN comparisons per retrieval")
    ax.set_axisbelow(True)

    # asymptotic complexity annotations (upper-right, per spec)
    ax.text(19000, 5120, r"$O(N)$", color=COLOR["fifo"],
            ha="right", va="center", fontsize=8)
    ax.text(19000, 45, r"$O(\log n)$", color=COLOR["balanced_tree"],
            ha="right", va="center", fontsize=8)
    ax.text(19000, 0.55, r"$O(1)$", color=COLOR["psn"],
            ha="right", va="center", fontsize=8)
    # psn_tiered: 0 comparisons (drawn at the FLOOR for the log axis)
    ax.text(19000, 0.2, "0 (zero reorder)", color=COLOR["psn"],
            ha="right", va="center", fontsize=7, fontweight="bold")

    style_axes(ax)

    # legend on top, 4 columns, no frame
    handles = [Line2D([], [], label=NAME[k], color=COLOR[k],
                      lw=recipe(k)["lw"], ls=recipe(k)["ls"],
                      marker=recipe(k)["marker"], ms=recipe(k)["ms"])
               for k in ("fifo", "chained_hash", "balanced_tree", "psn")]
    ax.legend(handles=handles, ncol=4, loc="upper center",
              bbox_to_anchor=(0.5, 1.20), frameon=False, columnspacing=1.0,
              handlelength=1.4, fontsize=7.5)

    fig.subplots_adjust(left=0.18, right=0.96, top=0.80, bottom=0.17)
    fig.savefig(OUT, format="pdf", bbox_inches="tight")
    plt.close(fig)
    print("wrote", OUT)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Fig — exp1b lookup 面板: lookup/retrieve time cost vs N（带 O(·) 复杂度注解）。

按用户要求，每种变体单独一张图、单独一个 PDF/PNG（绝不多图合一）：
  布局 × index_only 是否入图：
    1) fig_exp1b_lookup_gbn64     — 单图 gbn_long64，4 方法
    2) fig_exp1b_lookup_gbn64_idx — 单图 gbn_long64，5 方法（含 index-only Φ）
    3) fig_exp1b_lookup_sr64      — 单图 sr_64，4 方法
    4) fig_exp1b_lookup_sr64_idx  — 单图 sr_64，5 方法
    5) fig_exp1b_lookup_2x2       — 2×2 四模式面板，4 方法
    6) fig_exp1b_lookup_2x2_idx   — 2×2 四模式面板，5 方法

视觉规格（沿用 fig_exp1a_store 重画版逐项）：
  白底 + 完整框线 + 四边向内刻度(含次级) + 只留水平浅灰主网格；
  serif/Times；轴标签加粗 9pt、刻度 8pt；图例左上浅灰细边框单列；
  无图内标题（caption 交论文）；O(·) 注解 = 曲线复杂度（O(n)/O(log n)/O(1)）。

坐标：横轴 Cache depth N 对数(base2)、纵轴 Median lookup time cost (ns) 对数（禁 latency）。
复杂度注解：FIFO→O(n)、Balanced Tree→O(log n)、PSN Mapping→O(1)（hash/index 同为 O(1)，
  只注代表曲线，避免三条 O(1) 叠字）。

数字提取：从 lookup_summary.csv + n_cmp.csv 写 out/exp1b_lookup/exp1b_numbers.md（脚本提取，禁手抄）。
"""
import os
import collections

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import FixedLocator

plt.rcParams["font.family"] = "serif"
plt.rcParams["font.serif"] = ["Times New Roman", "Liberation Serif", "DejaVu Serif"]
plt.rcParams["axes.unicode_minus"] = False
plt.rcParams["font.size"] = 8

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CSV = os.path.join(ROOT, "out", "exp1b_lookup", "lookup_summary.csv")
NCMP = os.path.join(ROOT, "out", "exp1b_lookup", "n_cmp.csv")
OUT_MD = os.path.join(ROOT, "out", "exp1b_lookup", "exp1b_numbers.md")

N_LIST = [512, 1024, 2048, 4096, 8192, 10240]
MODES = [("gbn_long64", "GBN (long, 64)"), ("gbn_short8", "GBN (short, 8)"),
         ("sr_16", "SR (16)"), ("sr_64", "SR (64)")]

# 图内 4 主方法（与 exp1a 一致）：csv 名 / 显示名 / 颜色 / 标记 / 实心 / 线宽
PLOT = [
    ("fifo_bounded",          "FIFO Queue",    "#8C8C8C", "o", False, 1.7),
    ("chained_hash_bounded",  "Chained Hash",  "#E8A33D", "s", False, 1.7),
    ("balanced_tree_bounded", "Balanced Tree", "#4C9F70", "^", False, 1.7),
    ("psn_dynblock",          "PSN Mapping",   "#1F4E9C", "D", True,  2.3),
]
# 对照 E（可选入图）：Φ 纯算术下界，虚线区别于实线主方法
IDX = ("index_only", "index-only ($\\Phi$)", "#A93226", "x", False, 1.3)

# 复杂度注解：方法 -> (标注文本, 指向曲线末点)
COMPLEXITY = {
    "fifo_bounded":          "O(n)",
    "balanced_tree_bounded": "O(log n)",
    "psn_dynblock":          "O(1)",
}


def read_rows():
    rows = []
    with open(CSV, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            rows.append(line.split(","))
    assert rows and rows[0][0] == "method", "CSV header missing"
    hdr = rows[0]
    return [dict(zip(hdr, r)) for r in rows[1:]]


def read_ncmp():
    rows = []
    with open(NCMP, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            rows.append(line.split(","))
    assert rows and rows[0][0] == "method", "n_cmp header missing"
    hdr = rows[0]
    return [dict(zip(hdr, r)) for r in rows[1:]]


def collect(rows):
    """(method, mode) -> {N -> (p50, p90)}"""
    out = collections.defaultdict(dict)
    for r in rows:
        out[(r["method"], r["mode"])][int(r["N"])] = (float(r["p50_ns"]), float(r["p90_ns"]))
    return out


def style_ax(ax, labelsize=8):
    ax.set_facecolor("white")
    for s in ax.spines.values():
        s.set_visible(True)
        s.set_color("black")
        s.set_linewidth(0.8)
    ax.tick_params(axis="both", which="major", direction="in", top=True,
                   right=True, bottom=True, left=True, length=3.5, labelsize=labelsize)
    ax.tick_params(axis="both", which="minor", direction="in", top=True,
                   right=True, bottom=True, left=True, length=2.0)
    ax.minorticks_on()
    ax.grid(False)
    ax.grid(True, which="major", axis="y", color="#dcdcdc", linewidth=0.6, zorder=0)
    ax.set_axisbelow(True)
    ax.xaxis.label.set_fontweight("bold")
    ax.yaxis.label.set_fontweight("bold")
    ax.xaxis.label.set_fontsize(9)
    ax.yaxis.label.set_fontsize(9)


def draw_mode(ax, data, mode, include_idx, annotate=True, fontsize=7.5):
    """在一根轴上画给定 mode 的 lookup-vs-N 曲线（+ 可选 O(·) 注解）。"""
    series = list(PLOT) + ([IDX] if include_idx else [])
    last_pts = {}  # method -> (x_last, y_last)
    for name, label, color, marker, filled, lw in series:
        d = data.get((name, mode))
        if not d:
            continue
        xs = [n for n in N_LIST if n in d]
        ys = [d[n][0] for n in xs]
        last_pts[name] = (xs[-1], ys[-1])
        ax.plot(xs, ys, color=color, marker=marker, ms=6, ls=("--" if name == "index_only" else "-"),
                lw=lw, mfc=(color if filled else "none"), mec=color,
                mew=(0.0 if filled else 1.1), label=label, zorder=3)

    ax.set_xscale("log", base=2)
    ax.set_yscale("log")
    ax.set_xticks([512, 2048, 8192])
    ax.set_xticklabels(["512", "2048", "8192"])
    ax.xaxis.set_minor_locator(FixedLocator([1024, 4096]))
    ax.set_xlim(300, 21000)

    # 纵轴范围：给 O(n) 注解留出 FIFO 上方的空间
    fifo_last = last_pts.get("fifo_bounded", (10240, 1e5))[1]
    ax.set_ylim(20, fifo_last * 2.2)

    ax.set_xlabel("Cache depth N")
    ax.set_ylabel("Median lookup time cost (ns)")
    style_ax(ax)

    if annotate:
        # 三处复杂度注解放数据右侧开放区，箭头指向对应曲线末点（避免与下界曲线叠字）
        annot = [
            ("fifo_bounded",          17000, 1.12),
            ("balanced_tree_bounded", 13500, 1.55),
            ("psn_dynblock",          17000, 1.25),
        ]
        for name, tx, ymul in annot:
            if name not in last_pts:
                continue
            color = dict((n, c) for n, _, c, _, _, _ in PLOT)[name]
            x0, y0 = last_pts[name]
            ax.annotate(COMPLEXITY[name], xy=(x0, y0), xytext=(tx, y0 * ymul),
                        fontsize=fontsize, fontstyle="italic", color=color,
                        arrowprops=dict(arrowstyle="->", color=color, lw=0.7,
                                        shrinkA=0, shrinkB=2),
                        ha="center", va="center")

    leg = ax.legend(loc="upper left", fontsize=8, ncol=1, frameon=True,
                    framealpha=1.0, edgecolor="#c9c9c9", borderpad=0.4,
                    borderaxespad=0.6, handlelength=1.6, handletextpad=0.5,
                    labelspacing=0.45)
    leg.get_frame().set_linewidth(0.8)


def save(fig, stem):
    for ext in ("png", "pdf"):
        out = os.path.join(HERE, "%s.%s" % (stem, ext))
        fig.savefig(out, dpi=300, bbox_inches="tight")
        print("wrote %s" % out)


def fig_single(data, mode, include_idx, stem):
    fig, ax = plt.subplots(figsize=(3.45, 2.3))
    draw_mode(ax, data, mode, include_idx, annotate=True)
    fig.tight_layout(pad=0.4)
    save(fig, stem)
    plt.close(fig)


def fig_2x2(data, include_idx, stem):
    fig, axes = plt.subplots(2, 2, figsize=(7.0, 5.2))
    for ax, (mode, title) in zip(axes.flat, MODES):
        draw_mode(ax, data, mode, include_idx, annotate=True, fontsize=6.5)
        ax.set_title(title, fontsize=9, fontweight="bold")
    fig.tight_layout(pad=0.6, h_pad=1.0, w_pad=1.0)
    save(fig, stem)
    plt.close(fig)


def ncmp_table(ncmp, mode):
    by = collections.defaultdict(dict)
    for r in ncmp:
        if r["mode"] != mode:
            continue
        by[r["method"]][int(r["N"])] = float(r["mean_cmp_per_pkt"])
    order = [m[0] for m in PLOT] + ["index_only"]
    labels = {
        "fifo_bounded": "FIFO Queue", "chained_hash_bounded": "Chained Hash",
        "balanced_tree_bounded": "Balanced Tree", "psn_dynblock": "PSN Mapping",
        "index_only": "index-only ($\\Phi$)",
    }
    lines = ["| method | " + " | ".join(str(n) for n in N_LIST) + " | 复杂度 |",
             "|---|" + "---|" * (len(N_LIST) + 1)]
    for m in order:
        if m not in by:
            continue
        cells = ["%.1f" % by[m].get(n, float("nan")) if n in by[m] else "—" for n in N_LIST]
        cx = {"fifo_bounded": "O(N)≈N/2", "chained_hash_bounded": "O(1)",
              "balanced_tree_bounded": "O(log N)", "psn_dynblock": "O(1) Φ",
              "index_only": "O(1) Φ"}[m]
        lines.append("| %s | %s | %s |" % (labels[m], " | ".join(cells), cx))
    return "\n".join(lines)


def p50_table(data, mode):
    by = collections.defaultdict(dict)
    for (m, md), d in data.items():
        if md != mode:
            continue
        for n, (p50, _) in d.items():
            by[m][n] = p50
    order = [m[0] for m in PLOT] + ["index_only"]
    labels = {
        "fifo_bounded": "FIFO Queue", "chained_hash_bounded": "Chained Hash",
        "balanced_tree_bounded": "Balanced Tree", "psn_dynblock": "PSN Mapping",
        "index_only": "index-only ($\\Phi$)",
    }
    lines = ["| method | " + " | ".join(str(n) for n in N_LIST) + " |",
             "|---|" + "---|" * len(N_LIST)]
    for m in order:
        if m not in by:
            continue
        cells = []
        for n in N_LIST:
            cells.append("%.1f" % by[m][n] if n in by[m] else "—")
        lines.append("| %s | %s |" % (labels[m], " | ".join(cells)))
    return "\n".join(lines)


def main():
    rows = read_rows()
    ncmp = read_ncmp()
    data = collect(rows)

    # ---- 6 张图，每张独立 PDF+PNG ----
    fig_single(data, "gbn_long64", False, "fig_exp1b_lookup_gbn64")
    fig_single(data, "gbn_long64", True,  "fig_exp1b_lookup_gbn64_idx")
    fig_single(data, "sr_64",      False, "fig_exp1b_lookup_sr64")
    fig_single(data, "sr_64",      True,  "fig_exp1b_lookup_sr64_idx")
    fig_2x2(data, False, "fig_exp1b_lookup_2x2")
    fig_2x2(data, True,  "fig_exp1b_lookup_2x2_idx")

    # ---- 数字提取（脚本产出，禁手抄）----
    md = []
    md.append("# exp1b lookup — 脚本提取（lookup_summary.csv / n_cmp.csv）\n")
    md.append("> payload=1024，B=512，reps=5，n_batches=32；主指标 p50，次指标 p90。\n")
    md.append("> floor(读钟/B)：B=512 → 7.619 ns/op；B=32 → 112.505 ns/op（floor ∝ 1/B 闭环）。\n")
    md.append("> 图：gbn64 / sr64 单图 + 2×2 四模式；含/不含 index-only 各一版（单图独立 PDF）。\n")
    for mode, title in MODES:
        md.append("\n## p50 (ns) — %s（%s）\n" % (mode, title))
        md.append(p50_table(data, mode))
    md.append("\n## n_cmp（mean_cmp_per_pkt，纯取包比较）— gbn_long64 代表\n")
    md.append(ncmp_table(ncmp, "gbn_long64"))
    text = "\n".join(md) + "\n"

    with open(OUT_MD, "w", encoding="utf-8") as f:
        f.write(text)
    print("wrote %s" % OUT_MD)
    print("\n" + text)


if __name__ == "__main__":
    main()

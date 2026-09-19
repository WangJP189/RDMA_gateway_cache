#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Fig + 数字 — exp1a 分配策略敏感性（第 5 条正交矩阵的分配维度）: pooled vs perstore。

背景（第 5 条）：分配策略与索引结构正交。同一索引结构两种分配口径：
  pooled   = 一次性预分配空闲链表 + 复用（n_malloc=0，恒）
  perstore = 逐 store malloc / 淘汰 free（n_malloc=n_free=实际 store 次数）
唯一变量 = 分配策略；索引结构 / 淘汰 / 计时口径完全相同（alloc_equiv_test 验证字节一致）。

图 fig_exp1a_alloc_sensitivity：
  单栏 3.45×2.3 in；横轴 packet size（log base2，5 档 MTU）、纵轴 store time cost（ns，log）。
  4 结构各两条线：pooled（实线、实心标记）vs perstore（虚线、空心标记）。
  线型图例：实线=pooled / 虚线=perstore。

数字 out/exp1a_store/alloc_sensitivity.md：
  p50 pooled vs perstore 表 + Δ(ns)/Δ% + 裸 malloc/free 参考（alloc_probe）+ 关键结论。
数字全部脚本从 CSV 提取，禁手抄。
"""
import os
import collections

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import FixedLocator
from matplotlib.lines import Line2D

plt.rcParams["font.family"] = "serif"
plt.rcParams["font.serif"] = ["Times New Roman", "Liberation Serif", "DejaVu Serif"]
plt.rcParams["axes.unicode_minus"] = False
plt.rcParams["font.size"] = 8

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
POOLED_CSV = os.path.join(ROOT, "out", "exp1a_store", "store_summary.csv")
PERSTORE_CSV = os.path.join(ROOT, "out", "exp1a_store_perstore", "store_summary.csv")
PROBE_CSV = os.path.join(ROOT, "out", "alloc_probe", "alloc_probe.csv")
OUT_MD = os.path.join(ROOT, "out", "exp1a_store", "alloc_sensitivity.md")

PAYLOADS = [256, 512, 1024, 2048, 4096]

# (pooled 名, perstore 名, 显示名, 颜色, 标记, 实心)
PAIRS = [
    ("fifo_bounded",          "fifo_perstore",          "FIFO Queue",    "#8C8C8C", "o", False),
    ("chained_hash_bounded",  "chained_hash_perstore",  "Chained Hash",  "#E8A33D", "s", False),
    ("balanced_tree_bounded", "balanced_tree_perstore", "Balanced Tree", "#4C9F70", "^", False),
    ("psn_dynblock",          "psn_dynblock_perstore",  "PSN Mapping",   "#1F4E9C", "D", True),
]

PROBE_LABEL = {
    "32": "32 B（fifo/hash 节点）",
    "56": "56 B（avl 节点）",
    "64": "64 B（align16(56)）",
    "4128": "4128 B（32+4096，perstore 最大 malloc）",
}


def read_csv(path, first_col="method"):
    rows = []
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            rows.append(line.split(","))
    assert rows and rows[0][0] == first_col, "CSV header missing (%s): %s" % (first_col, path)
    hdr = rows[0]
    return [dict(zip(hdr, r)) for r in rows[1:]]


def collect(rows, B=2048):
    """method -> {payload -> p50}"""
    out = {}
    for r in rows:
        if int(r["B"]) != B:
            continue
        out.setdefault(r["method"], {})[int(r["payload"])] = float(r["p50_ns"])
    return out


def style_ax(ax):
    ax.set_facecolor("white")
    for s in ax.spines.values():
        s.set_visible(True)
        s.set_color("black")
        s.set_linewidth(0.8)
    ax.tick_params(axis="both", which="major", direction="in", top=True,
                   right=True, bottom=True, left=True, length=3.5, labelsize=8)
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


def main():
    pooled = collect(read_csv(POOLED_CSV))
    perstore = collect(read_csv(PERSTORE_CSV))

    # ---- 图：pooled（实线）vs perstore（虚线）----
    fig, ax = plt.subplots(figsize=(3.45, 2.3))
    for pname, sname, label, color, marker, filled in PAIRS:
        if pname in pooled:
            xs = [p for p in PAYLOADS if p in pooled[pname]]
            ys = [pooled[pname][p] for p in xs]
            ax.plot(xs, ys, color=color, marker=marker, ms=6, ls="-", lw=1.7,
                    mfc=(color if filled else "none"), mec=color,
                    mew=(0.0 if filled else 1.1), label=label, zorder=3)
        if sname in perstore:
            xs = [p for p in PAYLOADS if p in perstore[sname]]
            ys = [perstore[sname][p] for p in xs]
            ax.plot(xs, ys, color=color, marker=marker, ms=6, ls="--", lw=1.3,
                    mfc="none", mec=color, mew=1.0, label="_nolegend_", zorder=3)

    ax.set_xscale("log", base=2)
    ax.set_yscale("log")
    ax.set_xticks([256, 512, 1024, 2048, 4096])
    ax.set_xticklabels(["256", "512", "1024", "2048", "4096"])
    ax.xaxis.set_minor_locator(FixedLocator([384, 768, 1536, 3072]))
    ax.set_xlim(200, 5500)
    ax.set_xlabel("Packet size (B)")
    ax.set_ylabel("Store time cost (ns)")
    style_ax(ax)

    # 方法图例（左上）
    leg = ax.legend(loc="upper left", fontsize=7, ncol=1, frameon=True,
                    framealpha=1.0, edgecolor="#c9c9c9", borderpad=0.4,
                    borderaxespad=0.6, handlelength=1.5, handletextpad=0.5,
                    labelspacing=0.4)
    leg.get_frame().set_linewidth(0.8)
    # 线型图例：实线=pooled / 虚线=perstore（右下）
    style_leg = ax.legend(
        handles=[Line2D([], [], color="#333333", ls="-", lw=1.6, label="pooled"),
                 Line2D([], [], color="#333333", ls="--", lw=1.6, label="perstore")],
        loc="lower right", fontsize=7, frameon=True, framealpha=1.0,
        edgecolor="#c9c9c9", borderpad=0.3, borderaxespad=0.5,
        handlelength=1.5, handletextpad=0.5, labelspacing=0.4)
    style_leg.get_frame().set_linewidth(0.8)
    ax.add_artist(leg)

    fig.tight_layout(pad=0.4)
    for ext in ("png", "pdf"):
        out = os.path.join(HERE, "fig_exp1a_alloc_sensitivity.%s" % ext)
        fig.savefig(out, dpi=300, bbox_inches="tight")
        print("wrote %s" % out)
    plt.close(fig)

    # ---- 数字表（脚本提取）----
    md = []
    md.append("# exp1a 分配策略敏感性 — 脚本提取（pooled vs perstore）\n")
    md.append("> 唯一变量 = 分配策略（pooled 复用 / perstore 逐 store malloc+free）；"
              "索引结构 / 淘汰 / 计时口径完全相同（alloc_equiv_test 验证字节一致）。\n")
    md.append("> 主指标 p50，B=2048；pooled n_malloc=0，perstore n_malloc=n_free=实际 store 次数。\n")

    md.append("\n## p50 (ns) — pooled vs perstore（4 结构 × 5 payload）\n")
    for pname, sname, label, color, marker, filled in PAIRS:
        md.append("\n### %s\n" % label)
        md.append("| payload | pooled p50 | perstore p50 | Δ (ns) | Δ% |")
        md.append("|---|---|---|---|---|")
        deltas = []
        for p in PAYLOADS:
            a = pooled.get(pname, {}).get(p)
            b = perstore.get(sname, {}).get(p)
            if a is None or b is None:
                md.append("| %d B | — | — | — | — |" % p)
                continue
            d = b - a
            dp = d / a * 100.0
            deltas.append(dp)
            md.append("| %d B | %.3f | %.3f | %+.3f | %+.1f%% |" % (p, a, b, d, dp))
        if deltas:
            md.append("| **均值** | | | | **%+.1f%%** |" % (sum(deltas) / len(deltas)))

    md.append("\n## 裸 malloc/free 参考（alloc_probe，B=2048，p50 ns/op）\n")
    md.append("| n_bytes | 语义 | p50 (ns) |")
    md.append("|---|---|---|")
    probe = {}
    for r in read_csv(PROBE_CSV, "n_bytes"):
        probe[r["n_bytes"]] = float(r["p50_ns"])
    for k in ("32", "56", "64", "4128"):
        if k in probe:
            md.append("| %s | %s | %.3f |" % (k, PROBE_LABEL[k], probe[k]))

    md.append("\n## 关键结论（脚本计算，禁手抄）\n")
    # 每结构平均 Δ%（含 payload 项）
    for pname, sname, label, color, marker, filled in PAIRS:
        deltas = []
        for p in PAYLOADS:
            a = pooled.get(pname, {}).get(p)
            b = perstore.get(sname, {}).get(p)
            if a is not None and b is not None:
                deltas.append((b - a) / a * 100.0)
        if not deltas:
            continue
        md.append("- **%s**：perstore 相对 pooled 平均 **%+.1f%%**（%d 档 payload）——"
                  "逐 store malloc/free 的固定开销，随 payload 增大被 memcpy 摊薄。"
                  % (label, sum(deltas) / len(deltas), len(deltas)))

    text = "\n".join(md) + "\n"
    with open(OUT_MD, "w", encoding="utf-8") as f:
        f.write(text)
    print("wrote %s" % OUT_MD)
    print("\n" + text)


if __name__ == "__main__":
    main()

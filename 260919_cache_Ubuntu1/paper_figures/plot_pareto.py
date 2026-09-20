#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Fig — Pareto（综合权衡）: store p50 vs SR-64 lookup p50（2026-09-20）。

坐标：
  x = store p50（ns）@ payload=1024、pooled、B=2048（来自 exp1a store_summary.csv）。
  y = SR-64 retrieve_set p50（ns）@ N=4096、sort_impl=0（来自 exp1b lookup_summary.csv）。
  双对数；左下角 = 更好（store 快且 lookup 快）。

序列（5 点）：
  FIFO Queue / Chained Hash / Balanced Tree / PSN Mapping / index-only(Φ 纯算术对照)。

想证的一件事（正文数字全部脚本从 CSV 提取，禁手抄）：
  PSN Mapping 在 (store, lookup) 两个维度上**同时**严格优于三个基线（store 更低且 lookup 更低），
  是唯一真实方法的 Pareto 最优点；另一个前沿点是 index-only（Φ 地板：无 payload 拷贝的不可约 store 成本，
  但 SR 交付仍走排序路径，故 lookup 略高于 PSN 的位图快路径）。

视觉规格（沿用 fig_exp1a_store）：白底 + 完整框线 + 四边向内刻度；serif/Times；轴标签加粗；
  只留水平浅灰主网格；无图内标题。标注用文字标签（点数少，不用图例）。

数字提取：out/pareto/pareto_numbers.md（脚本从两个 CSV 提取，禁手抄）。
"""
import os

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

plt.rcParams["font.family"] = "serif"
plt.rcParams["font.serif"] = ["Times New Roman", "Liberation Serif", "DejaVu Serif"]
plt.rcParams["axes.unicode_minus"] = False
plt.rcParams["font.size"] = 8

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CSV_STORE = os.path.join(ROOT, "out", "exp1a_store", "store_summary.csv")
CSV_LOOKUP = os.path.join(ROOT, "out", "exp1b_lookup", "lookup_summary.csv")
OUT_DIR = os.path.join(ROOT, "out", "pareto")
OUT_MD = os.path.join(OUT_DIR, "pareto_numbers.md")

# (store 方法, lookup 方法, 显示名, 颜色, 标记)
#   PSN：store 用 psn_dynblock（自适应收敛后冻结）、lookup 用 psn_dynblock_adaptive（同义）。
POINTS = [
    ("fifo_bounded",          "fifo_bounded",          "FIFO Queue",    "#8C8C8C", "o"),
    ("chained_hash_bounded",  "chained_hash_bounded",  "Chained Hash",  "#E8A33D", "s"),
    ("balanced_tree_bounded", "balanced_tree_bounded", "Balanced Tree", "#4C9F70", "^"),
    ("psn_dynblock",          "psn_dynblock_adaptive", "PSN Mapping",   "#1F4E9C", "D"),
    ("index_only",            "index_only",            "index-only ($\\Phi$)", "#999999", "*"),
]


def read_csv(path):
    rows = []
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            rows.append(line.split(","))
    assert rows and rows[0][0] == "method", "CSV header missing: %s" % path
    hdr = rows[0]
    return [dict(zip(hdr, r)) for r in rows[1:]]


def store_p50(rows):
    """method -> store p50 @ payload=1024, pooled, B=2048"""
    out = {}
    for r in rows:
        if r["alloc_mode"] != "pooled":
            continue
        if int(r["payload"]) != 1024 or int(r["B"]) != 2048:
            continue
        out[r["method"]] = float(r["p50_ns"])
    return out


def lookup_p50(rows):
    """method -> SR-64 p50 @ N=4096, sort_impl=0"""
    out = {}
    for r in rows:
        if r["mode"] != "sr_64" or int(r["N"]) != 4096:
            continue
        if r.get("sort_impl", "0") != "0":
            continue
        out[r["method"]] = float(r["p50_ns"])
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
    ax.grid(True, which="major", color="#dcdcdc", linewidth=0.6, zorder=0)
    ax.set_axisbelow(True)
    ax.xaxis.label.set_fontweight("bold")
    ax.yaxis.label.set_fontweight("bold")
    ax.xaxis.label.set_fontsize(9)
    ax.yaxis.label.set_fontsize(9)


def main():
    sp = store_p50(read_csv(CSV_STORE))
    lp = lookup_p50(read_csv(CSV_LOOKUP))

    pts = []
    for sm, lm, label, color, marker in POINTS:
        if sm not in sp or lm not in lp:
            print("WARN missing: store=%s(%s) lookup=%s(%s)" %
                  (sm, sm in sp, lm, lm in lp))
            continue
        pts.append((sm, lm, label, color, marker, sp[sm], lp[lm]))

    # ---- 画图（单栏 3.45×2.6 in）----
    fig, ax = plt.subplots(figsize=(3.45, 2.6))
    for sm, lm, label, color, marker, x, y in pts:
        filled = (marker == "D" and label.startswith("PSN"))
        ax.plot([x], [y], color=color, marker=marker, ms=8, ls="none",
                mfc=(color if filled else "none"), mec=color,
                mew=(0.0 if filled else 1.3), zorder=3)

    # 文字标签（偏移避免重叠）
    offs = {
        "FIFO Queue": (6, 0.06),
        "Chained Hash": (6, 0.10),
        "Balanced Tree": (6, 0.05),
        "PSN Mapping": (-6, -12),
        "index-only ($\\Phi$)": (-6, 8),
    }
    for sm, lm, label, color, marker, x, y in pts:
        dx, dy = offs.get(label, (0, 0))
        ax.annotate(label, xy=(x, y), xytext=(dx, dy),
                    textcoords="offset points", fontsize=7.5,
                    color=color, ha=("left" if dx >= 0 else "right"),
                    zorder=4)

    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_xlim(0.8, 120)
    ax.set_ylim(800, 300000)
    ax.set_xlabel("Store p50 (ns) @ 1024 B")
    ax.set_ylabel("SR-64 lookup p50 (ns) @ N=4096")
    style_ax(ax)

    # 主导关系结论（脚本计算，禁手抄）
    psn = next(p for p in pts if p[2] == "PSN Mapping")
    dom = []
    for sm, lm, label, color, marker, x, y in pts:
        if label == "PSN Mapping" or label.startswith("index-only"):
            continue
        if psn[5] < x and psn[6] < y:
            dom.append(label)
    if dom:
        ax.text(0.97, 0.03,
                "PSN Mapping dominates: %s" % ", ".join(dom),
                transform=ax.transAxes, fontsize=6.5, color="#1F4E9C",
                ha="right", va="bottom")

    fig.tight_layout(pad=0.4)
    for ext in ("png", "pdf"):
        out = os.path.join(HERE, "fig_pareto.%s" % ext)
        fig.savefig(out, dpi=300, bbox_inches="tight")
        print("wrote %s" % out)
    plt.close(fig)

    # ---- 数字提取（脚本产出，禁手抄）----
    os.makedirs(OUT_DIR, exist_ok=True)
    md = []
    md.append("# Pareto — 脚本提取（store_summary.csv × lookup_summary.csv）\n")
    md.append("> x=store p50 @ payload=1024 pooled B=2048（exp1a）；y=SR-64 retrieve_set p50 @ N=4096 sort_impl=0（exp1b）。\n")
    md.append("\n| method | store p50 (ns) | SR-64 lookup p50 (ns) |\n|---|---|---|")
    for sm, lm, label, color, marker, x, y in pts:
        md.append("| %s | %.3f | %.1f |" % (label, x, y))
    md.append("")
    md.append("## 主导关系（脚本计算）")
    if dom:
        for d in dom:
            dp = next(p for p in pts if p[2] == d)
            md.append("- PSN Mapping 同时优于 %s：store %.3f < %.3f ns 且 lookup %.1f < %.1f ns"
                      % (d, psn[5], dp[5], psn[6], dp[6]))
    else:
        md.append("- （无）")
    idx = next(p for p in pts if p[2].startswith("index-only"))
    md.append("- index-only（Φ 地板）：store %.3f ns（无 payload 拷贝的不可约成本），但 SR 交付仍走排序路径，"
              "lookup %.1f ns 略高于 PSN 位图快路径 %.1f ns" % (idx[5], idx[6], psn[6]))
    text = "\n".join(md) + "\n"
    with open(OUT_MD, "w", encoding="utf-8") as f:
        f.write(text)
    print("wrote %s" % OUT_MD)
    print("\n" + text)


if __name__ == "__main__":
    main()

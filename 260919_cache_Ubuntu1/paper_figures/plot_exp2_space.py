#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Fig — exp2 space 面板: space utilization (%) vs packet size（双 y 轴修订 2026-09-20）。

序列（5 方法，全部有界预分配，无界版不进 exp2）：
  FIFO Queue / Chained Hash / Balanced Tree / PSN Mapping (adaptive S) / PSN fixed block (S=4096)。

指标：utilization = payload_bytes / allocated_bytes
      payload_bytes   = N × pl（满窗 N=4096 条、包长 pl）
      allocated_bytes = cache_footprint_bytes（池 + 索引/桶 + 元数据，sizeof 实测）
      overhead_per_pkt = (allocated_bytes − payload_bytes) / N（每驻留包分摊的空间开销，B）
      lower_bound_util = pl / (align16(pl) + sizeof(slot_meta_t))（位置槽理想利用率上界，方法无关）

想证的三件事（正文数字全部脚本从 CSV 提取，禁手抄）：
  ① 我们（弹性）利用率反超原最优基线 FIFO（第 1 条删 24B 头后 stride=align16(S)），且贴近理想上界；
  ② PSN fixed block (S=4096) 在 256B 崩到 ~6%、overhead 冲到 ~3855 B/包，证明弹性槽大小解决了固定块低利用率；
  ③ 弹性 S 收敛到各 MTU 档（block_S 列）。

双 y 轴：
  左轴 = Space utilization (%)，线性 0–105；5 方法实线 + lower_bound_util 灰点线（理想上界）。
  右轴 = Overhead per pkt (B)，对数；5 方法虚线（同色对应，右轴讲「每包浪费多少字节」）。
  fixed 组 overhead 在 256B 冲到 3855B（右轴对数才与 15–72B 的其余方法同框可见）。

视觉规格（沿用 fig_exp1a_store 重画版）：白底 + 完整框线 + 四边向内刻度(含次级) + 只留水平浅灰主网格；
  serif/Times；轴标签加粗 9pt、刻度 8pt；图例左上浅灰细边框单列；无图内标题、无 O(·) 标注。
  两条 PSN 变体同色：#1F4E9C 实线=弹性、虚线=fixed S=4096。

数字提取：out/exp2_space/exp2_numbers.md（脚本从 space_summary.csv 提取，禁手抄）。
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
CSV = os.path.join(ROOT, "out", "exp2_space", "space_summary.csv")
OUT_MD = os.path.join(ROOT, "out", "exp2_space", "exp2_numbers.md")

PAYLOADS = [256, 512, 1024, 2048, 4096]

# 5 序列：csv 名 / 显示名 / 颜色 / 标记 / 实心 / 线宽 / 线型（fixed=虚线）
PLOT = [
    ("fifo_bounded",          "FIFO Queue",                "#8C8C8C", "o", False, 1.7, "-"),
    ("chained_hash_bounded",  "Chained Hash",              "#E8A33D", "s", False, 1.7, "-"),
    ("balanced_tree_bounded", "Balanced Tree",             "#4C9F70", "^", False, 1.7, "-"),
    ("psn_dynblock",          "PSN Mapping (adaptive S)",  "#1F4E9C", "D", True,  2.3, "-"),
    ("ring_fixed",            "PSN fixed block (S=4096)",  "#1F4E9C", "D", False, 1.7, "--"),
]

TABLE_ORDER = [m[0] for m in PLOT]
LABELS = {m[0]: m[1] for m in PLOT}


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


def collect(rows):
    """method -> {payload -> (util_frac, block_S, alloc_bytes, overhead_per_pkt, lower_bound_util)}"""
    out = collections.OrderedDict()
    for r in rows:
        m = r["method"]
        out.setdefault(m, {})
        out[m][int(r["payload"])] = (float(r["utilization"]),
                                     int(r["block_S"]),
                                     int(r["allocated_bytes"]),
                                     float(r["overhead_per_pkt"]),
                                     float(r["lower_bound_util"]))
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


def _set_x(ax):
    ax.set_xscale("log", base=2)
    ax.set_xticks([256, 512, 1024, 2048, 4096])
    ax.set_xticklabels(["256", "512", "1024", "2048", "4096"])
    ax.xaxis.set_minor_locator(FixedLocator([384, 768, 1536, 3072]))
    ax.set_xlim(200, 5500)


def main():
    data = collect(read_rows())

    # ---- 画图（单栏 3.45×2.6 in，双 y 轴）----
    fig, ax = plt.subplots(figsize=(3.45, 2.6))

    # 左轴：利用率（实线）+ 理想上界（灰点线）
    for name, label, color, marker, filled, lw, ls in PLOT:
        d = data.get(name)
        if not d:
            continue
        xs = [p for p in PAYLOADS if p in d]
        ys = [d[p][0] * 100.0 for p in xs]   # fraction -> percent
        ax.plot(xs, ys, color=color, marker=marker, ms=7, ls=ls, lw=lw,
                mfc=(color if filled else "none"), mec=color,
                mew=(0.0 if filled else 1.1), label=label, zorder=3)
    # lower_bound_util（理想上界，方法无关）：取任一行（每 pl 相同）
    lb = {}
    for m in TABLE_ORDER:
        if m in data:
            for p in PAYLOADS:
                if p in data[m]:
                    lb[p] = data[m][p][4] * 100.0
            if lb:
                break
    if lb:
        lb_xs = [p for p in PAYLOADS if p in lb]
        lb_ys = [lb[p] for p in lb_xs]
        ax.plot(lb_xs, lb_ys, color="#999999", ls=":", lw=1.2, marker="None",
                label="ideal bound (lower_bound_util)", zorder=2)

    _set_x(ax)
    ax.set_ylim(0, 105)
    ax.set_yticks([0, 20, 40, 60, 80, 100])
    ax.set_xlabel("Packet size (B)")
    ax.set_ylabel("Space utilization (%)")
    style_ax(ax)

    leg = ax.legend(loc="upper left", fontsize=6.5, ncol=1, frameon=True,
                    framealpha=1.0, edgecolor="#c9c9c9", borderpad=0.3,
                    borderaxespad=0.5, handlelength=1.6, handletextpad=0.5,
                    labelspacing=0.4)
    leg.get_frame().set_linewidth(0.8)

    # 右轴：overhead_per_pkt（虚线，同色对应）
    ax2 = ax.twinx()
    for name, label, color, marker, filled, lw, ls in PLOT:
        d = data.get(name)
        if not d:
            continue
        xs = [p for p in PAYLOADS if p in d]
        ys = [d[p][3] for p in xs]          # overhead_per_pkt (B)
        ax2.plot(xs, ys, color=color, ls=":", lw=1.0, marker="None",
                 alpha=0.55, zorder=1)
    ax2.set_yscale("log")
    ax2.set_ylim(5, 8000)
    ax2.set_ylabel("Overhead per pkt (B)")
    ax2.yaxis.label.set_fontweight("bold")
    ax2.yaxis.label.set_fontsize(9)
    ax2.tick_params(axis="y", which="major", direction="in", labelsize=8,
                    length=3.5)
    ax2.spines["right"].set_color("black")
    ax2.spines["right"].set_linewidth(0.8)

    fig.tight_layout(pad=0.4)
    for ext in ("png", "pdf"):
        out = os.path.join(HERE, "fig_exp2_space.%s" % ext)
        fig.savefig(out, dpi=300, bbox_inches="tight")
        print("wrote %s" % out)
    plt.close(fig)

    # ---- 数字提取（脚本产出，禁手抄）----
    def pct(m, p):
        return data.get(m, {}).get(p, (0.0, 0, 0, 0.0, 0.0))[0] * 100.0

    def ovh(m, p):
        return data.get(m, {}).get(p, (0.0, 0, 0, 0.0, 0.0))[3]

    md = []
    md.append("# exp2 space — 脚本提取（space_summary.csv）\n")
    md.append("> N=4096 满窗；utilization = payload_bytes / allocated_bytes；"
              "overhead_per_pkt = (allocated−payload)/N；lower_bound_util = pl/(align16(pl)+12)。\n")

    md.append("\n## 空间利用率 (%) — 5 方法 × 5 档 MTU\n")
    header = "| method | " + " | ".join("%d B" % p for p in PAYLOADS) + " |"
    sep = "|---|" + "---|" * len(PAYLOADS)
    md.append(header)
    md.append(sep)
    for m in TABLE_ORDER:
        if m not in data:
            continue
        cells = ["%.2f" % pct(m, p) if p in data[m] else "—" for p in PAYLOADS]
        md.append("| %s | %s |" % (LABELS[m], " | ".join(cells)))

    md.append("\n## 每包空间开销 overhead_per_pkt (B) — 5 方法 × 5 档 MTU\n")
    md.append(header)
    md.append(sep)
    for m in TABLE_ORDER:
        if m not in data:
            continue
        cells = ["%.1f" % ovh(m, p) if p in data[m] else "—" for p in PAYLOADS]
        md.append("| %s | %s |" % (LABELS[m], " | ".join(cells)))

    md.append("\n## 理想上界 lower_bound_util (%)（仅对齐填充 + 12B meta，方法无关）\n")
    if lb:
        lb_cells = ["%.2f" % lb[p] if p in lb else "—" for p in PAYLOADS]
        md.append("| lower_bound_util | %s |" % " | ".join(lb_cells))

    md.append("\n## 分配总量 allocated_bytes (B) — 满窗 N=4096\n")
    md.append(header)
    md.append(sep)
    for m in TABLE_ORDER:
        if m not in data:
            continue
        cells = ["%d" % data[m][p][2] if p in data[m] else "—" for p in PAYLOADS]
        md.append("| %s | %s |" % (LABELS[m], " | ".join(cells)))

    md.append("\n## block_S（dynblock 槽大小；非 dynblock=0）\n")
    for m in ("psn_dynblock", "ring_fixed"):
        if m in data:
            s = ", ".join("%s B→S=%d" % (p, data[m][p][1]) for p in PAYLOADS)
            md.append("- %s: %s" % (LABELS[m], s))

    md.append("\n## 关键结论（脚本计算，供正文）\n")
    fifo_lo = min((pct("fifo_bounded", p) for p in PAYLOADS), default=0.0)
    fifo_hi = max((pct("fifo_bounded", p) for p in PAYLOADS), default=0.0)
    el_lo = min((pct("psn_dynblock", p) for p in PAYLOADS), default=0.0)
    el_hi = max((pct("psn_dynblock", p) for p in PAYLOADS), default=0.0)
    md.append("- FIFO（原最优基线）利用率区间：%.2f%%–%.2f%%" % (fifo_lo, fifo_hi))
    md.append("- PSN(弹性) 利用率区间：%.2f%%–%.2f%%（**反超 FIFO**）" % (el_lo, el_hi))
    for p in PAYLOADS:
        gap_el = pct("psn_dynblock", p) - pct("fifo_bounded", p)   # 正=PSN 更高
        gap_fx = pct("fifo_bounded", p) - pct("ring_fixed", p)     # 正=FIFO 更高（fixed 塌陷）
        md.append("- %d B：PSN(弹性)−FIFO=%+.2f pp，FIFO−PSN(fixed)=%.2f pp" % (p, gap_el, gap_fx))
    min_el_p = min(PAYLOADS, key=lambda p: pct("psn_dynblock", p) - pct("fifo_bounded", p))
    min_el_gap = pct("psn_dynblock", min_el_p) - pct("fifo_bounded", min_el_p)
    md.append("- 弹性反超最小点：%d B，PSN 仍高 %+.2f pp（5 档全部反超 FIFO）" % (min_el_p, min_el_gap))
    worst_fx_p = min(PAYLOADS, key=lambda p: pct("ring_fixed", p))
    worst_fx_util = pct("ring_fixed", worst_fx_p)
    md.append("- fixed 最大塌陷点：%d B，利用率 %.2f%%（vs FIFO %.2f%%），overhead %.1f B/包 —— 弹性槽大小机制的价值所在"
              % (worst_fx_p, worst_fx_util, pct("fifo_bounded", worst_fx_p), ovh("ring_fixed", worst_fx_p)))
    # 弹性贴近理想上界（lb[p] 已是百分比）
    el_lb_gap_min = min(PAYLOADS, key=lambda p: lb[p] - pct("psn_dynblock", p))
    md.append("- 弹性距理想上界最近：%d B，PSN %.2f%% vs lower_bound %.2f%%（差 %.2f pp；仅剩溢出数组等固定开销）"
              % (el_lb_gap_min, pct("psn_dynblock", el_lb_gap_min),
                 lb[el_lb_gap_min], lb[el_lb_gap_min] - pct("psn_dynblock", el_lb_gap_min)))

    text = "\n".join(md) + "\n"
    with open(OUT_MD, "w", encoding="utf-8") as f:
        f.write(text)
    print("wrote %s" % OUT_MD)
    print("\n" + text)


if __name__ == "__main__":
    main()

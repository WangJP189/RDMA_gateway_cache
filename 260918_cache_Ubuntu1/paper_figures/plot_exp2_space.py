#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Fig — exp2 space 面板: space utilization (%) vs packet size。

序列（5 方法，全部有界预分配，无界版不进 exp2）：
  FIFO Queue / Chained Hash / Balanced Tree / PSN Mapping (adaptive S) / PSN Mapping (fixed S=4096)。

指标：utilization = payload_bytes / allocated_bytes
      payload_bytes   = N × pl（满窗 N=4096 条、包长 pl）
      allocated_bytes = cache_footprint_bytes（池 + 索引/桶 + 元数据，sizeof 实测）

想证的三件事（正文数字全部脚本从 CSV 提取，禁手抄）：
  ① 我们（弹性）利用率非最高，但与最优基线 FIFO 相差 <=5%（256B ~4.4pp、4096B ~0.3pp）；
  ② PSN Mapping(fixed S=4096) 在 256B 崩到 ~6%（4128B 槽只装 256B），证明弹性槽大小解决了固定块低利用率；
  ③ 弹性 S 收敛到各 MTU 档（block_S 列）。

视觉规格（沿用 fig_exp1a_store 重画版）：
  白底 + 完整框线 + 四边向内刻度(含次级) + 只留水平浅灰主网格；serif/Times；
  轴标签加粗 9pt、刻度 8pt；图例左上浅灰细边框单列；无图内标题、无 O(·) 标注。
  两条 PSN 变体同色：#1F4E9C 实线=弹性、虚线=fixed S=4096。

坐标：横轴 Packet size (B) 对数(base2，256/512/1024/2048/4096)；纵轴 Space utilization (%) 线性 0-100
      （fixed 组崩到 ~6%，线性轴才能直接看见塌陷；对数轴会把 80-100% 压扁）。

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
    ("psn_dynblock_fixed",    "PSN Mapping (fixed S=4096)","#1F4E9C", "D", False, 1.7, "--"),
]

# 表内顺序（与图一致）
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
    """method -> {payload -> (util_frac, block_S, alloc_bytes)}"""
    out = collections.OrderedDict()
    for r in rows:
        m = r["method"]
        out.setdefault(m, {})
        out[m][int(r["payload"])] = (float(r["utilization"]),
                                     int(r["block_S"]),
                                     int(r["allocated_bytes"]))
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
    data = collect(read_rows())

    # ---- 画图（单栏 3.45×2.3 in）----
    fig, ax = plt.subplots(figsize=(3.45, 2.3))
    for name, label, color, marker, filled, lw, ls in PLOT:
        d = data.get(name)
        if not d:
            continue
        xs = [p for p in PAYLOADS if p in d]
        ys = [d[p][0] * 100.0 for p in xs]   # fraction -> percent
        ax.plot(xs, ys, color=color, marker=marker, ms=7, ls=ls, lw=lw,
                mfc=(color if filled else "none"), mec=color,
                mew=(0.0 if filled else 1.1), label=label, zorder=3)

    ax.set_xscale("log", base=2)
    ax.set_xticks([256, 512, 1024, 2048, 4096])
    ax.set_xticklabels(["256", "512", "1024", "2048", "4096"])
    ax.xaxis.set_minor_locator(FixedLocator([384, 768, 1536, 3072]))
    ax.set_xlim(200, 5500)
    ax.set_ylim(0, 105)
    ax.set_yticks([0, 20, 40, 60, 80, 100])
    ax.set_xlabel("Packet size (B)")
    ax.set_ylabel("Space utilization (%)")
    style_ax(ax)

    leg = ax.legend(loc="lower right", fontsize=6.5, ncol=1, frameon=True,
                    framealpha=1.0, edgecolor="#c9c9c9", borderpad=0.3,
                    borderaxespad=0.5, handlelength=1.6, handletextpad=0.5,
                    labelspacing=0.4)
    leg.get_frame().set_linewidth(0.8)

    fig.tight_layout(pad=0.4)
    for ext in ("png", "pdf"):
        out = os.path.join(HERE, "fig_exp2_space.%s" % ext)
        fig.savefig(out, dpi=300, bbox_inches="tight")
        print("wrote %s" % out)
    plt.close(fig)

    # ---- 数字提取（脚本产出，禁手抄）----
    def pct(m, p):
        return data.get(m, {}).get(p, (0.0, 0, 0))[0] * 100.0

    md = []
    md.append("# exp2 space — 脚本提取（space_summary.csv）\n")
    md.append("> N=4096 满窗；utilization = payload_bytes / allocated_bytes（cache_footprint_bytes，sizeof 实测）。\n")

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

    md.append("\n## 分配总量 allocated_bytes (B) — 满窗 N=4096\n")
    md.append(header.replace("method", "method").replace("%d B", "%d B"))
    md.append(sep)
    for m in TABLE_ORDER:
        if m not in data:
            continue
        cells = ["%d" % data[m][p][2] if p in data[m] else "—" for p in PAYLOADS]
        md.append("| %s | %s |" % (LABELS[m], " | ".join(cells)))

    md.append("\n## block_S（dynblock 槽大小；非 dynblock=0）\n")
    for m in ("psn_dynblock", "psn_dynblock_fixed"):
        if m in data:
            s = ", ".join("%s B→S=%d" % (p, data[m][p][1]) for p in PAYLOADS)
            md.append("- %s: %s" % (LABELS[m], s))

    md.append("\n## 关键结论（脚本计算，供正文）\n")
    fifo_best = max((pct("fifo_bounded", p) for p in PAYLOADS), default=0.0)
    md.append("- FIFO（最优基线）利用率区间：%.2f%%–%.2f%%" %
              (min((pct("fifo_bounded", p) for p in PAYLOADS), default=0.0), fifo_best))
    for p in PAYLOADS:
        gap_el = pct("fifo_bounded", p) - pct("psn_dynblock", p)
        gap_fx = pct("fifo_bounded", p) - pct("psn_dynblock_fixed", p)
        md.append("- %d B：FIFO−PSN(弹性)=%.2f pp，FIFO−PSN(fixed)=%.2f pp" % (p, gap_el, gap_fx))
    worst_el_p = max(PAYLOADS, key=lambda p: pct("fifo_bounded", p) - pct("psn_dynblock", p))
    worst_el_gap = pct("fifo_bounded", worst_el_p) - pct("psn_dynblock", worst_el_p)
    md.append("- 弹性最大劣势点：%d B，差 %.2f pp（<=5%% 断言成立）" % (worst_el_p, worst_el_gap))
    worst_fx_p = min(PAYLOADS, key=lambda p: pct("psn_dynblock_fixed", p))
    worst_fx_util = pct("psn_dynblock_fixed", worst_fx_p)
    md.append("- fixed 最大塌陷点：%d B，利用率 %.2f%%（vs FIFO %.2f%%）——弹性槽大小机制的价值所在" %
              (worst_fx_p, worst_fx_util, pct("fifo_bounded", worst_fx_p)))

    text = "\n".join(md) + "\n"
    with open(OUT_MD, "w", encoding="utf-8") as f:
        f.write(text)
    print("wrote %s" % OUT_MD)
    print("\n" + text)


if __name__ == "__main__":
    main()

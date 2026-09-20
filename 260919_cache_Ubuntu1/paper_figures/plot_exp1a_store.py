#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Fig — exp1a store 面板（重画版，2026-09-16 定稿；第 5 条双面板修订 2026-09-20）: store time cost vs packet size。

双面板（左：4 方法全景；右：PSN 弹性 vs ring_fixed 对照 + 百分比差标签）：
  左图 —— 与论文 3.1「三种常用结构 + 我们」一一对应，只画 4 条：
    FIFO Queue / Chained Hash / Balanced Tree / PSN Mapping（= dynblock adaptive S）。
  右图 —— 把「不做弹性的固定块」从表中拉出来单独对照：PSN Mapping（adaptive S）vs
    ring_fixed（S=4096 固定），每个 payload 标注 fixed 相对 elastic 的 p50 百分比差（+%）。
    说明：S=4096 固定槽在小包下槽 stride 4096B（2048 槽跨 8MB 超 L2），store 缓存局部性差；
    弹性 S=payload 槽 stride 随包收缩，故小包更快。至 4096B 两者 S 相同、差收敛到 0。

视觉规格（照参考图 fig1_time 风格，逐项落实）：
  - serif/Times；轴标签加粗；单栏字号：轴标签 9 / 刻度 8 / 图例 8；
  - 白底 + 完整框线（四边 spine 全留）；四边向内刻度 + 次级 minor ticks；
  - 只留水平浅灰主网格线（无竖网格/竖线）；
  - 线宽 1.7（基线）/ 2.3（PSN Mapping）；标记 ○/□/△/◆ ~7pt；
  - 颜色：FIFO=#8C8C8C｜Chained Hash=#E8A33D｜Balanced Tree=#4C9F70｜PSN Mapping=#1F4E9C（实心）；
  - 图例放坐标区内左上：浅灰细边框、单列；
  - 无图内标题栏（caption 交论文 \\caption{}）；无 O(·) 标注（那是 exp1b 的活）。

坐标：横轴 Packet size (B) 对数（刻度 256/512/1024/2048/4096）；纵轴 Store time cost (ns) 对数（禁 latency）。
尺寸：双面板 7.0×2.6 in（跨栏）；单栏按 3.45×2.3 in 同比例缩放、字号×2。

数字提取：把 B=2048 p50/p90 + B=1024 p50（6 方法）+ 右图 % diff 表写
  out/exp1a_store/exp1a_numbers.md（数字全部脚本从 CSV 提取，禁手抄）。
RESULTS.md 由 assemble_results.py 统一组装（本脚本不再直写 RESULTS.md）。
"""
import os
import collections

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import FixedLocator

# ---- serif（Times 度量兼容替代）----
plt.rcParams["font.family"] = "serif"
plt.rcParams["font.serif"] = ["Times New Roman", "Liberation Serif", "DejaVu Serif"]
plt.rcParams["axes.unicode_minus"] = False
plt.rcParams["font.size"] = 8

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CSV_PATH = os.path.join(ROOT, "out", "exp1a_store", "store_summary.csv")
OUT_MD = os.path.join(ROOT, "out", "exp1a_store", "exp1a_numbers.md")

# ---- 图内 4 序列：csv 名 / 显示名 / 颜色 / 标记 / 实心 / 线宽 ----
PLOT = [
    ("fifo_bounded",          "FIFO Queue",    "#8C8C8C", "o", False, 1.7),
    ("chained_hash_bounded",  "Chained Hash",  "#E8A33D", "s", False, 1.7),
    ("balanced_tree_bounded", "Balanced Tree", "#4C9F70", "^", False, 1.7),
    ("psn_dynblock",          "PSN Mapping",   "#1F4E9C", "D", True,  2.3),
]

# ---- 右图：PSN 弹性 vs ring_fixed（固定块对照）----
RIGHT_PLOT = [
    ("psn_dynblock", "PSN Mapping (adaptive S)", "#1F4E9C", "D", True,  2.3),
    ("ring_fixed",   "PSN fixed block (S=4096)", "#C0504D", "s", False, 1.7),
]

# ---- 表内 6 方法（含被移出图的两个）----
TABLE_ORDER = ["fifo_bounded", "chained_hash_bounded", "balanced_tree_bounded",
               "psn_dynblock", "ring_fixed", "index_only"]
TABLE_LABEL = {
    "fifo_bounded":          "FIFO Queue",
    "chained_hash_bounded":  "Chained Hash",
    "balanced_tree_bounded": "Balanced Tree",
    "psn_dynblock":          "PSN Mapping (adaptive S)",
    "ring_fixed":            "PSN fixed block (S=4096)",
    "index_only":            "index-only ($\\Phi$)",
}

# RDMA 5 档 MTU（横坐标/变量统一用这 5 档，见 memory rdma-mtu-tiers）
PAYLOADS = [256, 512, 1024, 2048, 4096]


def read_rows():
    rows = []
    with open(CSV_PATH, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            rows.append(line.split(","))
    assert rows and rows[0][0] == "method", "CSV header missing"
    hdr = rows[0]
    return [dict(zip(hdr, r)) for r in rows[1:]]


def collect(rows, B):
    """method -> {payload -> (p50, p90, block_S)}，只取给定 B 的行。"""
    out = collections.OrderedDict()
    for r in rows:
        if int(r["B"]) != B:
            continue
        m = r["method"]
        out.setdefault(m, {})
        out[m][int(r["payload"])] = (float(r["p50_ns"]), float(r["p90_ns"]),
                                     int(r["block_S"]))
    return out


def md_table(data, key):
    """p50 主表（key='p50'/'p90'）：行=6 方法、列=payload。"""
    payloads = PAYLOADS
    idx = 0 if key == "p50" else 1
    header = "| method | " + " | ".join("%d B" % p for p in payloads) + " |"
    sep = "|---|" + "---|" * len(payloads)
    lines = [header, sep]
    for m in TABLE_ORDER:
        if m not in data:
            continue
        cells = []
        for p in payloads:
            if p in data[m]:
                cells.append("%.3f" % data[m][p][idx])
            else:
                cells.append("—")
        lines.append("| %s | %s |" % (TABLE_LABEL[m], " | ".join(cells)))
    return "\n".join(lines)


def style_ax(ax):
    """白底 + 完整框线 + 四边向内刻度(含次级) + 只留水平浅灰主网格。"""
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
    ax.grid(True, which="major", axis="y", color="#dcdcdc", linewidth=0.6,
            zorder=0)
    ax.set_axisbelow(True)
    ax.xaxis.label.set_fontweight("bold")
    ax.yaxis.label.set_fontweight("bold")
    ax.xaxis.label.set_fontsize(9)
    ax.yaxis.label.set_fontsize(9)


def _set_x_log(ax):
    ax.set_xscale("log", base=2)
    ax.set_xticks([256, 512, 1024, 2048, 4096])
    ax.set_xticklabels(["256", "512", "1024", "2048", "4096"])
    ax.xaxis.set_minor_locator(FixedLocator([384, 768, 1536, 3072]))
    ax.set_xlim(200, 5500)


def _legend(ax):
    leg = ax.legend(loc="upper left", fontsize=8, ncol=1, frameon=True,
                    framealpha=1.0, edgecolor="#c9c9c9", borderpad=0.4,
                    borderaxespad=0.6, handlelength=1.6, handletextpad=0.5,
                    labelspacing=0.45)
    leg.get_frame().set_linewidth(0.8)


def draw_left(ax, data):
    payloads = PAYLOADS
    for name, label, color, marker, filled, lw in PLOT:
        if name not in data:
            continue
        xs = [p for p in payloads if p in data[name]]
        ys = [data[name][p][0] for p in xs]
        ax.plot(xs, ys, color=color, marker=marker, ms=7, ls="-", lw=lw,
                mfc=(color if filled else "none"), mec=color,
                mew=(0.0 if filled else 1.1), label=label, zorder=3)
    _set_x_log(ax)
    ax.set_yscale("log")
    ax.set_ylim(1, 200)
    ax.set_xlabel("Packet size (B)")
    ax.set_ylabel("Store time cost (ns)")
    style_ax(ax)
    _legend(ax)


def draw_right(ax, data):
    payloads = PAYLOADS
    elastic = data.get("psn_dynblock", {})
    fixed = data.get("ring_fixed", {})
    for name, label, color, marker, filled, lw in RIGHT_PLOT:
        if name not in data:
            continue
        xs = [p for p in payloads if p in data[name]]
        ys = [data[name][p][0] for p in xs]
        ax.plot(xs, ys, color=color, marker=marker, ms=7, ls="-", lw=lw,
                mfc=(color if filled else "none"), mec=color,
                mew=(0.0 if filled else 1.1), label=label, zorder=3)
    # 每个 payload 标注 fixed 相对 elastic 的 p50 百分比差（fixed 快为负）
    for p in payloads:
        if p not in elastic or p not in fixed:
            continue
        e = elastic[p][0]
        fx = fixed[p][0]
        diff = (fx - e) / e * 100.0 if e > 0 else 0.0
        ax.annotate("%+.0f%%" % diff, xy=(p, fx),
                    xytext=(0, 4), textcoords="offset points",
                    fontsize=6.5, color="#C0504D", ha="center",
                    zorder=5)
    _set_x_log(ax)
    ax.set_yscale("log")
    ax.set_ylim(1, 200)
    ax.set_xlabel("Packet size (B)")
    ax.set_ylabel("Store time cost (ns)")
    style_ax(ax)
    _legend(ax)


def diff_table(data):
    """fixed 相对 elastic 的 p50 百分比差（右图标签数据源）。"""
    elastic = data.get("psn_dynblock", {})
    fixed = data.get("ring_fixed", {})
    payloads = PAYLOADS
    header = "| payload | " + " | ".join("%d B" % p for p in payloads) + " |"
    sep = "|---|" + "---|" * len(payloads)
    rows = ["elastic p50 (ns)", "fixed p50 (ns)", "fixed vs elastic (%)"]
    vals = [[], [], []]
    for p in payloads:
        e = elastic.get(p, (0.0,))[0]
        fx = fixed.get(p, (0.0,))[0]
        diff = (fx - e) / e * 100.0 if e > 0 else 0.0
        vals[0].append("%.3f" % e)
        vals[1].append("%.3f" % fx)
        vals[2].append("%+.1f" % diff)
    lines = [header, sep]
    for label, v in zip(rows, vals):
        lines.append("| %s | %s |" % (label, " | ".join(v)))
    return "\n".join(lines)


def main():
    rows = read_rows()
    B_main = 2048
    data = collect(rows, B_main)

    # ---- 双面板图（跨栏 7.0×2.6 in）----
    fig, (axL, axR) = plt.subplots(1, 2, figsize=(7.0, 2.6))
    draw_left(axL, data)
    draw_right(axR, data)
    fig.tight_layout(pad=0.4)
    for ext in ("png", "pdf"):
        out = os.path.join(HERE, "fig_exp1a_store.%s" % ext)
        fig.savefig(out, dpi=300, bbox_inches="tight")
        print("wrote %s" % out)
    plt.close(fig)

    # ---- 数字提取（脚本产出，禁手抄）----
    xval = collect(rows, 1024)
    md = []
    md.append("# exp1a store — 脚本提取（store_summary.csv）\n")
    md.append("## B=2048 主矩阵 · p50 (ns) — 主指标\n")
    md.append(md_table(data, "p50"))
    md.append("\n## B=2048 主矩阵 · p90 (ns) — 次指标\n")
    md.append(md_table(data, "p90"))
    md.append("\n## B=1024 交叉验证 · p50 (ns) — 排序/差距一致性\n")
    md.append(md_table(xval, "p50"))
    md.append("\n## 右图：PSN 弹性 vs ring_fixed（B=2048 p50，fixed 相对 elastic 的百分比差）\n")
    md.append(diff_table(data))
    md.append("\n## 收敛后 S（dynblock）\n")
    for m in ("psn_dynblock", "ring_fixed"):
        if m in data:
            s = ", ".join("%s B→S=%d" % (p, data[m][p][2]) for p in PAYLOADS)
            md.append("- %s: %s" % (TABLE_LABEL[m], s))
    text = "\n".join(md) + "\n"

    with open(OUT_MD, "w", encoding="utf-8") as f:
        f.write(text)
    print("wrote %s" % OUT_MD)
    print("\n" + text)


if __name__ == "__main__":
    main()

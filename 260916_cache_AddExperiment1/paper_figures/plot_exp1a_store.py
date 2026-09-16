#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Fig — exp1a store 面板（重画版，2026-09-16 定稿）: store time cost vs packet size。

序列（与论文 3.1「三种常用结构 + 我们」一一对应，图文一致，只画 4 条）：
  FIFO Queue / Chained Hash / Balanced Tree / PSN Mapping（= dynblock adaptive S）。
  index_only 与 dynblock(fixed S=4096) 从图中移出 → 只进 RESULTS.md 表 + 正文数字。

视觉规格（照参考图 fig1_time 风格，逐项落实）：
  - serif/Times；轴标签加粗；单栏字号：轴标签 9 / 刻度 8 / 图例 8；
  - 白底 + 完整框线（四边 spine 全留）；四边向内刻度 + 次级 minor ticks；
  - 只留水平浅灰主网格线（无竖网格/竖线）；
  - 线宽 1.7（基线）/ 2.3（PSN Mapping）；标记 ○/□/△/◆ ~7pt；
  - 颜色：FIFO=#8C8C8C｜Chained Hash=#E8A33D｜Balanced Tree=#4C9F70｜PSN Mapping=#1F4E9C（实心）；
  - 图例放坐标区内左上：浅灰细边框、单列；
  - 无图内标题栏（caption 交论文 \\caption{}）；无 O(·) 标注（那是 exp1b 的活）。

坐标：横轴 Packet size (B) 对数（刻度仅 64/1024/4096）；纵轴 Store time cost (ns) 对数（禁 latency）。
尺寸：单栏 3.45×2.3 in（默认）；跨栏按 7.0×3.6 in 同比例放大、字号×2。

数字提取：把 B=8192 p50/p90 + B=1024 p50（6 方法）写 out/exp1a_store/exp1a_numbers.md
与 RESULTS.md（数字全部脚本从 CSV 提取，禁手抄）。
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
RESULTS_MD = os.path.join(ROOT, "RESULTS.md")

# ---- 图内 4 序列：csv 名 / 显示名 / 颜色 / 标记 / 实心 / 线宽 ----
PLOT = [
    ("fifo_bounded",          "FIFO Queue",    "#8C8C8C", "o", False, 1.7),
    ("chained_hash_bounded",  "Chained Hash",  "#E8A33D", "s", False, 1.7),
    ("balanced_tree_bounded", "Balanced Tree", "#4C9F70", "^", False, 1.7),
    ("psn_dynblock",          "PSN Mapping",   "#1F4E9C", "D", True,  2.3),
]

# ---- 表内 6 方法（含被移出图的两个）----
TABLE_ORDER = ["fifo_bounded", "chained_hash_bounded", "balanced_tree_bounded",
               "psn_dynblock", "psn_dynblock_fixed", "index_only"]
TABLE_LABEL = {
    "fifo_bounded":          "FIFO Queue",
    "chained_hash_bounded":  "Chained Hash",
    "balanced_tree_bounded": "Balanced Tree",
    "psn_dynblock":          "PSN Mapping (adaptive S)",
    "psn_dynblock_fixed":    "PSN Mapping (fixed S=4096)",
    "index_only":            "index-only ($\\Phi$)",
}


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
    payloads = [64, 1024, 4096]
    idx = 0 if key == "p50" else 1
    lines = ["| method | 64 B | 1024 B | 4096 B |", "|---|---|---|---|"]
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
    # 四边向内刻度（主刻度带标签，次级无标签）
    ax.tick_params(axis="both", which="major", direction="in", top=True,
                   right=True, bottom=True, left=True, length=3.5, labelsize=8)
    ax.tick_params(axis="both", which="minor", direction="in", top=True,
                   right=True, bottom=True, left=True, length=2.0)
    ax.minorticks_on()
    # 只留水平浅灰主网格线（无竖网格/竖线）
    ax.grid(False)
    ax.grid(True, which="major", axis="y", color="#dcdcdc", linewidth=0.6,
            zorder=0)
    ax.set_axisbelow(True)
    # 轴标签加粗
    ax.xaxis.label.set_fontweight("bold")
    ax.yaxis.label.set_fontweight("bold")
    ax.xaxis.label.set_fontsize(9)
    ax.yaxis.label.set_fontsize(9)


def main():
    rows = read_rows()
    B_main = 8192
    data = collect(rows, B_main)
    payloads = [64, 1024, 4096]

    # ---- 画图（单栏 3.45×2.3 in）----
    fig, ax = plt.subplots(figsize=(3.45, 2.3))
    for name, label, color, marker, filled, lw in PLOT:
        if name not in data:
            continue
        xs = [p for p in payloads if p in data[name]]
        ys = [data[name][p][0] for p in xs]
        ax.plot(xs, ys, color=color, marker=marker, ms=7, ls="-", lw=lw,
                mfc=(color if filled else "none"), mec=color,
                mew=(0.0 if filled else 1.1), label=label, zorder=3)

    ax.set_xscale("log", base=2)
    ax.set_yscale("log")
    ax.set_xticks([64, 1024, 4096])
    ax.set_xticklabels(["64", "1024", "4096"])
    ax.xaxis.set_minor_locator(FixedLocator([128, 256, 512, 2048]))
    ax.set_xlim(40, 6500)
    ax.set_ylim(1, 1500)
    ax.set_xlabel("Packet size (B)")
    ax.set_ylabel("Store time cost (ns)")
    style_ax(ax)

    # 图例放进坐标区内左上：浅灰细边框、单列
    leg = ax.legend(loc="upper left", fontsize=8, ncol=1, frameon=True,
                    framealpha=1.0, edgecolor="#c9c9c9", borderpad=0.4,
                    borderaxespad=0.6, handlelength=1.6, handletextpad=0.5,
                    labelspacing=0.45)
    leg.get_frame().set_linewidth(0.8)

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
    md.append("## B=8192 主矩阵 · p50 (ns) — 主指标\n")
    md.append(md_table(data, "p50"))
    md.append("\n## B=8192 主矩阵 · p90 (ns) — 次指标\n")
    md.append(md_table(data, "p90"))
    md.append("\n## B=1024 交叉验证 · p50 (ns) — 排序/差距一致性\n")
    md.append(md_table(xval, "p50"))
    md.append("\n## 收敛后 S（dynblock）\n")
    for m in ("psn_dynblock", "psn_dynblock_fixed"):
        if m in data:
            s = ", ".join("%s B→S=%d" % (p, data[m][p][2]) for p in payloads)
            md.append("- %s: %s" % (TABLE_LABEL[m], s))
    text = "\n".join(md) + "\n"

    with open(OUT_MD, "w", encoding="utf-8") as f:
        f.write(text)
    print("wrote %s" % OUT_MD)

    # ---- RESULTS.md（数字段脚本写入）----
    r = []
    r.append("# RESULTS — 实验结果（数字由脚本从 store_summary.csv 提取）\n")
    r.append("> 生成：`python3 paper_figures/plot_exp1a_store.py`；表内数字禁止手抄。\n")
    r.append("\n## exp1a — store time cost（存时间开销）\n")
    r.append("\n**图：** `paper_figures/fig_exp1a_store.pdf`（同目录 `fig_exp1a_store.png` @300dpi）。\n")
    r.append("\n**图注（名词性短语，交论文 `\\caption{}`）：** Store time cost versus packet size.\n")
    r.append("\n条件（论文 3.1 正文）：N=10240，batch B=8192 主矩阵，R=5 runs，单位 ns；"
             "B=1024 为交叉验证。横轴 packet size（64/1024/4096 B）、纵轴 store time cost（ns），双对数。"
             "主指标 p50，次指标 p90（p99 仅 CSV 留痕，~1% 批次遭 ~2ms VM 调度停顿，不可用）。"
             "图内仅 4 方法；index_only 与 dynblock(fixed S=4096) 仅入下表（正文用数字给自适应收益）。\n")
    r.append("\n### 主矩阵 B=8192 · p50 (ns)\n")
    r.append(md_table(data, "p50"))
    r.append("\n\n### 主矩阵 B=8192 · p90 (ns)\n")
    r.append(md_table(data, "p90"))
    r.append("\n\n### B=1024 交叉验证 · p50 (ns) — 排序/差距一致性\n")
    r.append(md_table(xval, "p50"))
    r.append("\n\n### 收敛后 S（dynblock）\n")
    for m in ("psn_dynblock", "psn_dynblock_fixed"):
        if m in data:
            s = ", ".join("%s B→S=%d" % (p, data[m][p][2]) for p in payloads)
            r.append("- %s: %s" % (TABLE_LABEL[m], s))
    r.append("\n\n---\n")
    rtext = "\n".join(r) + "\n"

    with open(RESULTS_MD, "w", encoding="utf-8") as f:
        f.write(rtext)
    print("wrote %s" % RESULTS_MD)
    print("\n" + text)


if __name__ == "__main__":
    main()

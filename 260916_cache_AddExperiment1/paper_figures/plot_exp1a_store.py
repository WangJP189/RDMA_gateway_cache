#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Fig — 时间开销图 · store 面板（exp1a）：store time cost vs packet size。

读取 out/exp1a_store/store_summary.csv（B=8192 主矩阵），画 6 条序列：
  fifo_bounded / chained_hash_bounded / balanced_tree_bounded /
  psn_dynblock（自适应收敛后冻结）/ psn_dynblock_fixed（S=4096 固定，虚线对照）/
  index_only（Φ 不可约成本）。

规格（2026-09-16 定稿）：
  - 横轴 packet size（64/1024/4096 B），对数；纵轴 store time cost（ns），对数；
  - 矢量 PDF + 300 dpi PNG；黑白可读：靠线型 + 标记区分（不只靠颜色）；
  - 字体 serif（Times 度量兼容替代），缩放后字号 ≥7pt；
  - 主指标 p50；次指标 p90（本图只画 p50，p90/p99 见 RESULTS.md 脚本提取表）；
  - 图注（名词性短语）="Store time cost versus packet size."；
  - 图内注：N=10240, batch B=8192, R=5 runs；单位 ns。

数字提取：同时把 B=8192 的 p50/p90（及 B=1024 交叉验证）以 markdown 表打印到
stdout + 写入 out/exp1a_store/exp1a_numbers.md，供 RESULTS.md 直接引用（禁手抄）。
"""
import os
import csv
import collections

import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

# ---- serif 字体（Times 度量兼容替代）----
plt.rcParams["font.family"] = "serif"
plt.rcParams["font.serif"] = ["Times New Roman", "Liberation Serif", "DejaVu Serif"]
plt.rcParams["axes.unicode_minus"] = False
plt.rcParams["font.size"] = 9

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CSV_PATH = os.path.join(ROOT, "out", "exp1a_store", "store_summary.csv")
OUT_MD = os.path.join(ROOT, "out", "exp1a_store", "exp1a_numbers.md")
RESULTS_MD = os.path.join(ROOT, "RESULTS.md")

# 序列定义（顺序 = 图例顺序；黑白可读：线型 + 标记 + 填充 区分）
SERIES = [
    ("fifo_bounded",          "FIFO (bounded)",
     dict(color="black", ls="-",  marker="o", mfc="none",  lw=1.2, ms=5.5)),
    ("chained_hash_bounded",  "chained hash (bounded)",
     dict(color="black", ls="--", marker="s", mfc="none",  lw=1.2, ms=5.0)),
    ("balanced_tree_bounded", "balanced tree (bounded)",
     dict(color="black", ls="-.", marker="^", mfc="none",  lw=1.2, ms=6.0)),
    ("psn_dynblock",          "dynblock (adaptive S)",
     dict(color="black", ls="-",  marker="D", mfc="black", lw=2.2, ms=5.5)),
    ("psn_dynblock_fixed",    "dynblock (fixed S=4096)",
     dict(color="black", ls=":",  marker="D", mfc="none",  lw=1.4, ms=5.5)),
    ("index_only",            "index-only ($\\Phi$)",
     dict(color="0.45", ls=":",  marker="x", mfc="none",  lw=1.0, ms=6.0)),
]
ORDER = [s[0] for s in SERIES]
LABEL = dict((s[0], s[1]) for s in SERIES)
STYLE = dict((s[0], s[2]) for s in SERIES)


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
    """p50 主表（key='p50'/'p90'）：行=方法、列=payload。"""
    payloads = [64, 1024, 4096]
    idx = 0 if key == "p50" else 1
    head = "| method | 64 B | 1024 B | 4096 B |"
    sep = "|---|---|---|---|"
    lines = [head, sep]
    for m in ORDER:
        if m not in data:
            continue
        cells = []
        for p in payloads:
            if p in data[m]:
                cells.append("%.3f" % data[m][p][idx])
            else:
                cells.append("—")
        lines.append("| %s | %s |" % (LABEL[m], " | ".join(cells)))
    return "\n".join(lines)


def main():
    rows = read_rows()
    B_main = 8192
    data = collect(rows, B_main)
    payloads = [64, 1024, 4096]

    # ---- 画图 ----
    fig, ax = plt.subplots(figsize=(5.4, 4.2))
    for m in ORDER:
        if m not in data:
            continue
        xs = [p for p in payloads if p in data[m]]
        ys = [data[m][p][0] for p in xs]
        ax.plot(xs, ys, label=LABEL[m], **STYLE[m])

    ax.set_xscale("log", base=2)
    ax.set_yscale("log")
    ax.set_xticks(payloads)
    ax.set_xticklabels(["64", "1024", "4096"])
    ax.set_xlabel("Packet size (B)")
    ax.set_ylabel("Store time cost (ns)")
    ax.grid(True, which="both", ls=":", color="#cccccc", zorder=0)
    ax.set_axisbelow(True)

    # 图内注：N / batch B / runs + 单位
    ax.text(0.02, 0.02, "N=10240, batch B=8192, R=5 runs; unit: ns",
            transform=ax.transAxes, fontsize=7.5, color="#333333",
            va="bottom", ha="left")

    # 图注（名词性短语，放在图上方作标题）
    ax.set_title("Store time cost versus packet size", fontsize=10, loc="left",
                 pad=8)

    leg = ax.legend(loc="upper left", frameon=False, fontsize=7.5,
                    handlelength=2.4, borderaxespad=0.4, ncol=1)
    fig.tight_layout(rect=(0, 0, 1, 0.98))
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
            md.append("- %s: %s" % (LABEL[m], s))
    text = "\n".join(md) + "\n"

    with open(OUT_MD, "w", encoding="utf-8") as f:
        f.write(text)
    print("wrote %s" % OUT_MD)

    # ---- RESULTS.md（数字段由脚本写入，禁手抄）----
    r = []
    r.append("# RESULTS — 实验结果（数字由脚本从 store_summary.csv 提取）\n")
    r.append("> 生成：`python3 paper_figures/plot_exp1a_store.py`；表内数字禁止手抄。\n")
    r.append("\n## exp1a — store time cost（存时间开销）\n")
    r.append("\n**图：** `paper_figures/fig_exp1a_store.pdf`（同目录 `fig_exp1a_store.png` @300dpi）。\n")
    r.append("\n**图注（名词性短语）：** Store time cost versus packet size.\n")
    r.append("\n图内注：N=10240, batch B=8192, R=5 runs；单位 ns。横轴 packet size（64/1024/4096 B）、"
             "纵轴 store time cost（ns），双对数。主指标 p50，次指标 p90（p99 仅 CSV 留痕，"
             "~1% 批次遭 ~2ms VM 调度停顿，不可用）。序列：FIFO/hash/tree 三个 bounded 基线、"
             "dynblock（自适应收敛后冻结）、dynblock（fixed S=4096，虚线对照）、index-only（Φ 不可约成本）。\n")
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
            r.append("- %s: %s" % (LABEL[m], s))
    r.append("\n\n---\n")
    rtext = "\n".join(r) + "\n"

    with open(RESULTS_MD, "w", encoding="utf-8") as f:
        f.write(rtext)
    print("wrote %s" % RESULTS_MD)
    print("\n" + text)


if __name__ == "__main__":
    main()

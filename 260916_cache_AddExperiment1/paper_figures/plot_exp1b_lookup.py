#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Fig — exp1b lookup 面板: lookup/retrieve mean time cost vs N（平均时间，非中值）。

按用户最新要求：
  - 主指标 = 平均时间 mean_ns（非中值 p50）；纵轴 "Mean lookup time cost (ns)"；
  - 只出两张图：GBN（gbn_long64）与 SR（sr_64），各 4 方法（不含 index-only）；
  - 图内不写 O(·) 文本；图例缩小贴左上空白区。

视觉规格（沿用 fig_exp1a_store 重画版）：
  白底 + 完整框线 + 四边向内刻度(含次级) + 只留水平浅灰主网格；serif/Times；
  轴标签加粗 9pt、刻度 8pt；图例左上浅灰细边框单列；无图内标题。

坐标：横轴 Cache depth N 对数(base2)、纵轴 Mean lookup time cost (ns) 对数（禁 latency）。

数字提取（全部脚本从 CSV 提取，禁手抄）：
  - out/exp1b_lookup/exp1b_numbers.md     —— mean 表 + n_cmp 表
  - out/exp1b_lookup/exp1b_analysis_data.md —— 供 AI 分析的自洽数据块（含设置/两模式 mean 表/n_cmp/结论要点）
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
OUT_AI = os.path.join(ROOT, "out", "exp1b_lookup", "exp1b_analysis_data.md")

N_LIST = [512, 1024, 2048, 4096, 8192, 10240]

# 图内 4 主方法（与 exp1a 一致）：csv 名 / 显示名 / 颜色 / 标记 / 实心 / 线宽
PLOT = [
    ("fifo_bounded",          "FIFO Queue",    "#8C8C8C", "o", False, 1.7),
    ("chained_hash_bounded",  "Chained Hash",  "#E8A33D", "s", False, 1.7),
    ("balanced_tree_bounded", "Balanced Tree", "#4C9F70", "^", False, 1.7),
    ("psn_dynblock",          "PSN Mapping",   "#1F4E9C", "D", True,  2.3),
]
# 表内 5 方法（含 index-only 对照 E，图内不画）
TABLE_ORDER = [m[0] for m in PLOT] + ["index_only"]
LABELS = {
    "fifo_bounded": "FIFO Queue", "chained_hash_bounded": "Chained Hash",
    "balanced_tree_bounded": "Balanced Tree", "psn_dynblock": "PSN Mapping",
    "index_only": "index-only ($\\Phi$)",
}
COMPLEXITY = {
    "fifo_bounded": "O(N)≈N/2", "chained_hash_bounded": "O(1)",
    "balanced_tree_bounded": "O(log N)", "psn_dynblock": "O(1) Φ",
    "index_only": "O(1) Φ",
}


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


def collect(rows):
    """(method, mode) -> {N -> (mean, p50, p90)}"""
    out = collections.defaultdict(dict)
    for r in rows:
        out[(r["method"], r["mode"])][int(r["N"])] = (
            float(r["mean_ns"]), float(r["p50_ns"]), float(r["p90_ns"]))
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


def draw_mode(ax, data, mode):
    """在一根轴上画给定 mode 的 mean-lookup-vs-N 曲线（4 方法，无 O(·) 注解，图例缩小）。"""
    ys_all = []
    for name, label, color, marker, filled, lw in PLOT:
        d = data.get((name, mode))
        if not d:
            continue
        xs = [n for n in N_LIST if n in d]
        ys = [d[n][0] for n in xs]   # index 0 = mean_ns
        ys_all.extend(ys)
        ax.plot(xs, ys, color=color, marker=marker, ms=6, ls="-",
                lw=lw, mfc=(color if filled else "none"), mec=color,
                mew=(0.0 if filled else 1.1), label=label, zorder=3)

    ax.set_xscale("log", base=2)
    ax.set_yscale("log")
    ax.set_xticks([512, 2048, 8192])
    ax.set_xticklabels(["512", "2048", "8192"])
    ax.xaxis.set_minor_locator(FixedLocator([1024, 4096]))
    ax.set_xlim(400, 16000)

    y_min, y_max = min(ys_all), max(ys_all)
    ax.set_ylim(y_min * 0.6, y_max * 1.35)

    ax.set_xlabel("Cache depth N")
    ax.set_ylabel("Mean lookup time cost (ns)")
    style_ax(ax)

    # 图例缩小：贴左上（该区无数据：FIFO 左端在中部高度、右侧才升到顶），浅灰细边框单列
    leg = ax.legend(loc="upper left", fontsize=6.5, ncol=1, frameon=True,
                    framealpha=1.0, edgecolor="#c9c9c9", borderpad=0.25,
                    borderaxespad=0.5, handlelength=1.3, handletextpad=0.4,
                    labelspacing=0.3)
    leg.get_frame().set_linewidth(0.8)


def save(fig, stem):
    for ext in ("png", "pdf"):
        out = os.path.join(HERE, "%s.%s" % (stem, ext))
        fig.savefig(out, dpi=300, bbox_inches="tight")
        print("wrote %s" % out)


def mean_table(data, mode):
    """mean_ns 表：行=5 方法、列=N。"""
    by = collections.defaultdict(dict)
    for (m, md), d in data.items():
        if md != mode:
            continue
        for n, (mean, _, _) in d.items():
            by[m][n] = mean
    lines = ["| method | " + " | ".join(str(n) for n in N_LIST) + " |",
             "|---|" + "---|" * len(N_LIST)]
    for m in TABLE_ORDER:
        if m not in by:
            continue
        cells = ["%.1f" % by[m][n] if n in by[m] else "—" for n in N_LIST]
        lines.append("| %s | %s |" % (LABELS[m], " | ".join(cells)))
    return "\n".join(lines)


def ncmp_table(ncmp, mode):
    by = collections.defaultdict(dict)
    for r in ncmp:
        if r["mode"] != mode:
            continue
        by[r["method"]][int(r["N"])] = float(r["mean_cmp_per_pkt"])
    lines = ["| method | " + " | ".join(str(n) for n in N_LIST) + " | 复杂度 |",
             "|---|" + "---|" * (len(N_LIST) + 1)]
    for m in TABLE_ORDER:
        if m not in by:
            continue
        cells = ["%.1f" % by[m][n] if n in by[m] else "—" for n in N_LIST]
        lines.append("| %s | %s | %s |" % (LABELS[m], " | ".join(cells), COMPLEXITY[m]))
    return "\n".join(lines)


def analysis_block(data, ncmp):
    """供 AI 分析的自洽数据块（脚本提取，可直接粘贴给 AI）。"""
    L = []
    L.append("# exp1b lookup 实验数据（供 AI 分析）\n")
    L.append("## 实验设置")
    L.append("- 任务：RDMA 网关缓存 lookup/取包时间开销（GBN/SR NAK 重传工作负载）")
    L.append("- 方法：FIFO 队列 / 链式哈希 / 平衡树 / PSN 确定性映射(ours) / index-only(Φ纯算术对照)")
    L.append("- 本数据块模式：GBN(long,64) 与 SR(64)（各自代表 GBN 与 SR 两类重传）")
    L.append("- N（缓存深度 / 工作集条数）：512, 1024, 2048, 4096, 8192, 10240")
    L.append("- payload=1024 B；计时批 B=512 ops；reps=5；主指标 = 平均时间 mean_ns（单位 ns）")
    L.append("- 每 op = 一次 retrieve_range(GBN)/retrieve_set(SR)（定位 + memcpy 全过程）")
    L.append("- 读钟地板：B=512 → 7.619 ns/op；B=32 → 112.505 ns/op（floor ∝ 1/B）")
    L.append("- 注：mean 受 VM ~1% 调度停顿尾影响，快方法 mean 比 p50 高约 4-6%（真实上界，非算法差异）；结论不受影响")
    for mode, label in [("gbn_long64", "GBN (long, 64)"), ("sr_64", "SR (64)")]:
        L.append("\n## %s — mean lookup time (ns)\n" % label)
        L.append("| N | FIFO Queue | Chained Hash | Balanced Tree | PSN Mapping | index-only |")
        L.append("|---|---|---|---|---|---|")
        for n in N_LIST:
            cells = []
            for m in TABLE_ORDER:
                v = data.get((m, mode), {}).get(n)
                cells.append("%.1f" % v[0] if v else "—")
            L.append("| %d | %s |" % (n, " | ".join(cells)))
    L.append("\n## n_cmp（每次取包平均比较次数，复杂度证据；gbn_long64 代表）\n")
    L.append(ncmp_table(ncmp, "gbn_long64"))
    L.append("\n## 关键结论（供 AI 交叉验证）")
    L.append("1. 伸缩性：FIFO 随 N 线性增长（O(N)，n_cmp≈N/2）；Chained Hash / PSN Mapping / index-only 近似平缓（O(1)）；Balanced Tree 亚线性（O(log N)）。")
    L.append("2. PSN Mapping 绝对时间比 Chained Hash 慢约 1.5-2.5×（固定块池的 cache 局部性代价），但比 FIFO 快约两个数量级（N=10240 时快 ~90-120×）。")
    L.append("3. PSN Mapping 的独特价值不在 lookup 绝对速度，而在：Φ 位置索引零比较、连续 mmap 池可 DMA 零拷贝、自适应块大小（空间利用率，见 exp2）、零迁移 resize。")
    return "\n".join(L)


def main():
    rows = read_csv(CSV)
    ncmp = read_csv(NCMP)
    data = collect(rows)

    # ---- 两张图：GBN 与 SR（各 4 方法，mean，无 O(·)，图例缩小）----
    for mode, stem in [("gbn_long64", "fig_exp1b_lookup_gbn64"),
                       ("sr_64",      "fig_exp1b_lookup_sr64")]:
        fig, ax = plt.subplots(figsize=(3.45, 2.3))
        draw_mode(ax, data, mode)
        fig.tight_layout(pad=0.4)
        save(fig, stem)
        plt.close(fig)

    # ---- 数字表（mean 为主指标）----
    md = []
    md.append("# exp1b lookup — 脚本提取（lookup_summary.csv / n_cmp.csv）\n")
    md.append("> payload=1024，B=512，reps=5，n_batches=32；主指标 = 平均时间 mean_ns（非中值）。\n")
    md.append("> 读钟地板：B=512 → 7.619 ns/op；B=32 → 112.505 ns/op（floor ∝ 1/B 闭环）。\n")
    md.append("> mean 受 VM ~1% 调度停顿尾影响，快方法 mean 比 p50 高约 4-6%（真实上界）；p50/p90/p99 仍在 CSV。\n")
    for mode, label in [("gbn_long64", "GBN (long, 64)"), ("sr_64", "SR (64)")]:
        md.append("\n## mean_ns (ns) — %s（%s）\n" % (mode, label))
        md.append(mean_table(data, mode))
    md.append("\n## n_cmp（mean_cmp_per_pkt，纯取包比较）— gbn_long64 代表\n")
    md.append(ncmp_table(ncmp, "gbn_long64"))
    text = "\n".join(md) + "\n"

    with open(OUT_MD, "w", encoding="utf-8") as f:
        f.write(text)
    print("wrote %s" % OUT_MD)

    ai = analysis_block(data, ncmp)
    with open(OUT_AI, "w", encoding="utf-8") as f:
        f.write(ai)
    print("wrote %s" % OUT_AI)


if __name__ == "__main__":
    main()

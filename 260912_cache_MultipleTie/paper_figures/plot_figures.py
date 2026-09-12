#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
论文组合图生成脚本：把三大基准（时间/空间/行为）的数据整合成 3 组图，
每组图论证一个方面（图 1 时间、图 2 空间、图 3 行为与确定性）。

用法（在 260912_cache_MultipleTie/ 下）：
    python3 paper_figures/plot_figures.py

输入（已跑好的基准产出）：
    psn_lookup_benchmark/out/   summary.csv, scaling.csv, cdf_*_random.csv
    psn_space_bench/out/        space_summary.csv, space_multiflow.csv
    psn_behavior_bench/out/     behavior_summary.csv, behavior_alloc.csv,
                                behavior_sweep.csv

输出（paper_figures/）：
    fig1_time.png/pdf        时间开销（查找/存/取 延迟）
    fig2_space.png/pdf       空间开销（内存利用率 / 每包字节 / 多流 / 摊销）
    fig3_behavior.png/pdf    行为与确定性（零整理 / 处理开销 / malloc / 丢包）
"""

import os
import csv
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

# ---- 中文字体 ----
plt.rcParams["font.sans-serif"] = ["Noto Sans CJK JP", "Noto Sans CJK SC",
                                   "AR PL UMing CN", "AR PL UKai CN",
                                   "WenQuanYi Micro Hei", "DejaVu Sans"]
plt.rcParams["axes.unicode_minus"] = False

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
LOOKUP = os.path.join(ROOT, "psn_lookup_benchmark", "out")
SPACE = os.path.join(ROOT, "psn_space_bench", "out")
BEHAV = os.path.join(ROOT, "psn_behavior_bench", "out")
OUT = HERE

# ---- 统一配色（本文方法 psn_tiered / psn_mapping 用蓝色高亮） ----
COLOR = {
    "fifo": "#9e9e9e",
    "chained_hash": "#f0a400",
    "balanced_tree": "#2ca02c",
    "psn_fixed": "#d62728",
    "psn_dynamic": "#9467bd",
    "psn_tiered": "#1f77b4",
    "psn_mapping": "#1f77b4",
    "contiguous": "#000000",
}
LABEL = {
    "fifo": "FIFO 队列",
    "chained_hash": "链式哈希",
    "balanced_tree": "平衡树",
    "psn_fixed": "PSN 固定 5KB",
    "psn_dynamic": "PSN 简单动态",
    "psn_tiered": "PSN 分档动态（本文）",
    "psn_mapping": "PSN 映射（本文）",
    "contiguous": "Contiguous 下界",
}
ORDER_LOOKUP = ["fifo", "chained_hash", "balanced_tree", "psn_mapping"]
ORDER_6 = ["fifo", "chained_hash", "balanced_tree", "psn_fixed",
           "psn_dynamic", "psn_tiered"]
ORDER_SPACE = ["fifo", "chained_hash", "balanced_tree", "psn_map_fixed",
               "psn_map_dynamic", "psn_map_tiered", "contiguous"]
SPACE_LABEL = {
    "fifo": "FIFO 队列",
    "chained_hash": "链式哈希",
    "balanced_tree": "平衡树",
    "psn_map_fixed": "PSN 固定 5KB",
    "psn_map_dynamic": "PSN 简单动态",
    "psn_map_tiered": "PSN 分档动态（本文）",
    "contiguous": "Contiguous 下界",
}
SPACE_COLOR = {
    "fifo": "#9e9e9e",
    "chained_hash": "#f0a400",
    "balanced_tree": "#2ca02c",
    "psn_map_fixed": "#d62728",
    "psn_map_dynamic": "#9467bd",
    "psn_map_tiered": "#1f77b4",
    "contiguous": "#000000",
}


def read_csv(path):
    rows = []
    with open(path, "r", encoding="utf-8") as f:
        rd = csv.DictReader(f)
        for r in rd:
            rows.append(r)
    return rows


def read_summary():
    return read_csv(os.path.join(LOOKUP, "summary.csv"))


def read_scaling():
    return read_csv(os.path.join(LOOKUP, "scaling.csv"))


def read_cdf(method):
    p = os.path.join(LOOKUP, f"cdf_{method}_random.csv")
    vals = []
    with open(p, "r", encoding="utf-8") as f:
        next(f)  # header
        for line in f:
            line = line.strip()
            if line:
                vals.append(float(line))
    return np.array(sorted(vals))


def read_space_summary():
    return read_csv(os.path.join(SPACE, "space_summary.csv"))


def read_space_multiflow():
    return read_csv(os.path.join(SPACE, "space_multiflow.csv"))


def read_behavior():
    return read_csv(os.path.join(BEHAV, "behavior_summary.csv"))


def read_alloc():
    return read_csv(os.path.join(BEHAV, "behavior_alloc.csv"))


def read_sweep():
    return read_csv(os.path.join(BEHAV, "behavior_sweep.csv"))


def bar_group(ax, names, groups, colors, title, ylabel, log=False, legend=True):
    """grouped bar: groups = [(label, [vals per name]), ...]"""
    x = np.arange(len(names))
    n = len(groups)
    w = 0.8 / n
    for i, (glabel, vals) in enumerate(groups):
        ax.bar(x + (i - (n - 1) / 2) * w, vals, w, label=glabel,
               color=[colors[nm] for nm in names], edgecolor="white",
               linewidth=0.4)
    ax.set_xticks(x)
    ax.set_xticklabels([LABEL.get(nm, nm) for nm in names], rotation=15,
                       ha="right", fontsize=8)
    ax.set_title(title, fontsize=11, fontweight="bold")
    ax.set_ylabel(ylabel, fontsize=9)
    if log:
        ax.set_yscale("log")
    if legend:
        ax.legend(fontsize=8, loc="best")
    ax.grid(axis="y", alpha=0.3, linewidth=0.5)
    for s in ["top", "right"]:
        ax.spines[s].set_visible(False)


# ============================== 图 1：时间 ==============================
def fig_time():
    fig, axes = plt.subplots(2, 2, figsize=(11, 8.5))
    fig.suptitle("图 1  时间开销：PSN 映射 O(1) 查找/存储/取回，且延迟与规模无关",
                 fontsize=13, fontweight="bold", y=0.98)

    # (a) 查找延迟 vs 缓存占用 N
    ax = axes[0, 0]
    sc = read_scaling()
    for nm in ORDER_LOOKUP:
        pts = [(int(r["N"]), float(r["median_ns"])) for r in sc
               if r["method"] == nm]
        pts.sort()
        xs = [p[0] for p in pts]
        ys = [p[1] for p in pts]
        ax.plot(xs, ys, marker="o", ms=4, lw=2, color=COLOR[nm],
                label=LABEL[nm])
    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_xlabel("缓存占用包数 N", fontsize=9)
    ax.set_ylabel("查找延迟 P50 (ns)", fontsize=9)
    ax.set_title("(a) 查找延迟随 N 增长（复杂度曲线）", fontsize=10,
                 fontweight="bold")
    ax.annotate("FIFO O(n)", xy=(10240, 2158), xytext=(2000, 3000),
                fontsize=8, color=COLOR["fifo"])
    ax.annotate("平衡树 O(log n)", xy=(10240, 33.7), xytext=(2000, 60),
                fontsize=8, color=COLOR["balanced_tree"])
    ax.annotate("PSN O(1) 平线", xy=(10240, 9.0), xytext=(3000, 6),
                fontsize=8, color=COLOR["psn_mapping"])
    ax.legend(fontsize=8, loc="upper left")
    ax.grid(alpha=0.3, linewidth=0.5, which="both")

    # (b) 查找延迟分布 CDF（随机访问）
    ax = axes[0, 1]
    for nm in ORDER_LOOKUP:
        v = read_cdf(nm)
        y = np.arange(1, len(v) + 1) / len(v) * 100
        ax.plot(v, y, lw=2, color=COLOR[nm], label=LABEL[nm])
    ax.set_xscale("log")
    ax.set_xlabel("查找延迟 (ns, 对数)", fontsize=9)
    ax.set_ylabel("累积概率 (%)", fontsize=9)
    ax.set_title("(b) 随机访问查找延迟 CDF", fontsize=10, fontweight="bold")
    ax.legend(fontsize=8, loc="lower right")
    ax.grid(alpha=0.3, linewidth=0.5, which="both")

    # (c) 存储延迟（顺序 vs 乱序，L=1024）
    ax = axes[1, 0]
    bh = {r["method"]: r for r in read_behavior()}
    names = ORDER_6
    g_seq = [float(bh[nm]["store_seq_p50"]) for nm in names]
    g_ooo = [float(bh[nm]["store_ooo_p50"]) for nm in names]
    bar_group(ax, names, [("顺序到达", g_seq), ("乱序到达", g_ooo)], COLOR,
              "(c) 存储延迟（L=1024，固定 5KB 慢 5×）", "存储延迟 P50 (ns)",
              log=True)

    # (d) 取回延迟（按序取回 / 命中 / 丢包判定）
    ax = axes[1, 1]
    g_ord = [float(bh[nm]["retr_ord_p50"]) for nm in names]
    g_found = [float(bh[nm]["found_p50"]) for nm in names]
    g_miss = [float(bh[nm]["miss_p50"]) for nm in names]
    bar_group(ax, names, [("按序取回", g_ord), ("命中取回", g_found),
                          ("丢包判定", g_miss)], COLOR,
              "(d) 取回/丢包判定延迟（FIFO O(n) 重排 8.8µs）",
              "取回延迟 P50 (ns)", log=True)

    fig.tight_layout(rect=[0, 0, 1, 0.96])
    for ext in ("png", "pdf"):
        fig.savefig(os.path.join(OUT, f"fig1_time.{ext}"), dpi=300,
                    bbox_inches="tight")
    plt.close(fig)
    print("[OK] fig1_time.png/pdf")


# ============================== 图 2：空间 ==============================
def fig_space():
    fig, axes = plt.subplots(2, 2, figsize=(11, 8.5))
    fig.suptitle("图 2  空间开销：分档动态块消除固定 5KB 的小包浪费",
                 fontsize=13, fontweight="bold", y=0.98)

    ss = read_space_summary()
    sizes = sorted({int(r["packet_size"]) for r in ss})

    def series(method):
        d = {int(r["packet_size"]): r for r in ss if r["method"] == method}
        return [d[s] for s in sizes]

    # (a) 空间利用率 vs 包大小
    ax = axes[0, 0]
    for nm in ORDER_SPACE:
        sr = series(nm)
        ys = [float(r["utilization"]) * 100 for r in sr]
        ls = "--" if nm == "contiguous" else "-"
        ax.plot(sizes, ys, marker="o", ms=4, lw=2, ls=ls,
                color=SPACE_COLOR[nm], label=SPACE_LABEL[nm])
    ax.set_xlabel("包大小 L (B)", fontsize=9)
    ax.set_ylabel("空间利用率 (%)", fontsize=9)
    ax.set_title("(a) 空间利用率 vs 包大小（固定 5KB 贴地）", fontsize=10,
                 fontweight="bold")
    ax.set_ylim(0, 105)
    ax.legend(fontsize=7.5, loc="lower right")
    ax.grid(alpha=0.3, linewidth=0.5)

    # (b) 每包平均分配字节 vs 包大小（对数）
    ax = axes[0, 1]
    for nm in ORDER_SPACE:
        sr = series(nm)
        ys = [float(r["avg_bytes_per_pkt"]) for r in sr]
        ls = "--" if nm == "contiguous" else "-"
        ax.plot(sizes, ys, marker="o", ms=4, lw=2, ls=ls,
                color=SPACE_COLOR[nm], label=SPACE_LABEL[nm])
    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_xlabel("包大小 L (B, 对数)", fontsize=9)
    ax.set_ylabel("每包平均字节 (B, 对数)", fontsize=9)
    ax.set_title("(b) 每包平均分配字节（固定 5KB 绝对浪费）", fontsize=10,
                 fontweight="bold")
    ax.legend(fontsize=7.5, loc="lower right")
    ax.grid(alpha=0.3, linewidth=0.5, which="both")

    # (c) 多流：利用率 vs 连接数 K
    ax = axes[1, 0]
    mf = read_space_multiflow()
    for key, label, color, ls in [
        ("vary_K", "简单动态", "#9467bd", "-"),
        ("vary_K_tiered", "分档动态（本文）", "#1f77b4", "-"),
    ]:
        pts = [(int(r["K"]), float(r["utilization"]) * 100) for r in mf
               if r["scenario"] == key]
        pts.sort()
        ax.plot([p[0] for p in pts], [p[1] for p in pts], marker="o", ms=4,
                lw=2, ls=ls, color=color, label=label)
    ax.set_xscale("log")
    ax.set_xlabel("并发连接数 K", fontsize=9)
    ax.set_ylabel("空间利用率 (%)", fontsize=9)
    ax.set_title("(c) 多流：利用率与连接数无关", fontsize=10, fontweight="bold")
    ax.set_ylim(0, 105)
    ax.legend(fontsize=8)
    ax.grid(alpha=0.3, linewidth=0.5)

    # (d) 摊销：利用率 vs 占用深度 npp
    ax = axes[1, 1]
    for key, label, color, ls in [
        ("amortize", "简单动态", "#9467bd", "-"),
        ("amortize_tiered", "分档动态（本文）", "#1f77b4", "-"),
    ]:
        pts = [(int(r["npp"]), float(r["utilization"]) * 100) for r in mf
               if r["scenario"] == key]
        pts.sort()
        ax.plot([p[0] for p in pts], [p[1] for p in pts], marker="o", ms=4,
                lw=2, ls=ls, color=color, label=label)
    ax.set_xscale("log")
    ax.set_xlabel("每连接占用深度 npp (包)", fontsize=9)
    ax.set_ylabel("空间利用率 (%)", fontsize=9)
    ax.set_title("(d) 固定数组开销随占用深度摊销", fontsize=10,
                 fontweight="bold")
    ax.set_ylim(0, 105)
    ax.legend(fontsize=8)
    ax.grid(alpha=0.3, linewidth=0.5)

    fig.tight_layout(rect=[0, 0, 1, 0.96])
    for ext in ("png", "pdf"):
        fig.savefig(os.path.join(OUT, f"fig2_space.{ext}"), dpi=300,
                    bbox_inches="tight")
    plt.close(fig)
    print("[OK] fig2_space.png/pdf")


# ============================== 图 3：行为与确定性 ==============================
def fig_behavior():
    fig, axes = plt.subplots(2, 2, figsize=(11, 8.5))
    fig.suptitle("图 3  行为与确定性：零整理、确定性内存、丢包 O(1) 判定",
                 fontsize=13, fontweight="bold", y=0.98)

    bh = {r["method"]: r for r in read_behavior()}
    names = ORDER_6

    # (a) PSN 比较次数 / 取回（零整理证据）
    ax = axes[0, 0]
    cmps = [float(bh[nm]["cmp_per_retr"]) for nm in names]
    disp = [max(c, 0.1) for c in cmps]  # log(0) 保护
    x = np.arange(len(names))
    bars = ax.bar(x, disp, 0.6, color=[COLOR[nm] for nm in names],
                  edgecolor="white", linewidth=0.4)
    for xi, c, b in zip(x, cmps, bars):
        ax.text(b.get_x() + b.get_width() / 2, b.get_height() * 1.05,
                f"{c:g}", ha="center", va="bottom", fontsize=8)
    ax.set_yscale("log")
    ax.set_xticks(x)
    ax.set_xticklabels([LABEL[nm] for nm in names], rotation=15, ha="right",
                       fontsize=8)
    ax.set_ylabel("PSN 比较次数 / 取回（对数）", fontsize=9)
    ax.set_title("(a) 取回平均 PSN 比较次数：本文为 0（零整理）", fontsize=10,
                 fontweight="bold")
    ax.grid(axis="y", alpha=0.3, linewidth=0.5)
    for s in ["top", "right"]:
        ax.spines[s].set_visible(False)

    # (b) 每包处理开销（store 控制面 64B）
    ax = axes[0, 1]
    small = [float(bh[nm]["store_small_p50"]) for nm in names]
    x = np.arange(len(names))
    bars = ax.bar(x, small, 0.6, color=[COLOR[nm] for nm in names],
                  edgecolor="white", linewidth=0.4)
    for xi, v, b in zip(x, small, bars):
        ax.text(b.get_x() + b.get_width() / 2, b.get_height() * 1.02,
                f"{v:.0f}", ha="center", va="bottom", fontsize=8)
    ax.set_xticks(x)
    ax.set_xticklabels([LABEL[nm] for nm in names], rotation=15, ha="right",
                       fontsize=8)
    ax.set_ylabel("store 控制面开销 P50 (ns)", fontsize=9)
    ax.set_title("(b) 每包 store 处理开销（64B 负载，隔离分配器）", fontsize=10,
                 fontweight="bold")
    ax.grid(axis="y", alpha=0.3, linewidth=0.5)
    for s in ["top", "right"]:
        ax.spines[s].set_visible(False)

    # (c) malloc / 覆盖写 store
    ax = axes[1, 0]
    al = {r["method"]: r for r in read_alloc()}
    psn_names = ["psn_fixed", "psn_dynamic", "psn_tiered"]
    mall = [float(al[nm]["mallocs_per_store"]) for nm in psn_names]
    x = np.arange(len(psn_names))
    bars = ax.bar(x, mall, 0.5, color=[COLOR[nm] for nm in psn_names],
                  edgecolor="white", linewidth=0.4)
    for xi, v, b in zip(x, mall, bars):
        ax.text(b.get_x() + b.get_width() / 2, b.get_height() + 0.03,
                f"{v:.2f}", ha="center", va="bottom", fontsize=9)
    ax.set_ylim(0, 1.25)
    ax.set_xticks(x)
    ax.set_xticklabels([LABEL[nm] for nm in psn_names], rotation=10,
                       ha="right", fontsize=8)
    ax.set_ylabel("malloc 次数 / 覆盖写 store", fontsize=9)
    ax.set_title("(c) 稳态 malloc：分档池 0 次（确定性内存）", fontsize=10,
                 fontweight="bold")
    ax.grid(axis="y", alpha=0.3, linewidth=0.5)
    for s in ["top", "right"]:
        ax.spines[s].set_visible(False)

    # (d) 丢包判定延迟 vs 丢包率
    ax = axes[1, 1]
    sw = read_sweep()
    for nm in names:
        pts = [(float(r["rate"]), float(r["miss_p50"])) for r in sw
               if r["method"] == nm]
        pts.sort()
        xs = [p[0] * 100 for p in pts]
        ys = [p[1] for p in pts]
        ax.plot(xs, ys, marker="o", ms=4, lw=2, color=COLOR[nm],
                label=LABEL[nm])
    ax.set_yscale("log")
    ax.set_xlabel("丢包率 (%)", fontsize=9)
    ax.set_ylabel("丢包判定延迟 P50 (ns, 对数)", fontsize=9)
    ax.set_title("(d) 丢包判定延迟与丢包率无关（准确率恒 100%）", fontsize=10,
                 fontweight="bold")
    ax.legend(fontsize=7.5, loc="upper right")
    ax.grid(alpha=0.3, linewidth=0.5, which="both")

    fig.tight_layout(rect=[0, 0, 1, 0.96])
    for ext in ("png", "pdf"):
        fig.savefig(os.path.join(OUT, f"fig3_behavior.{ext}"), dpi=300,
                    bbox_inches="tight")
    plt.close(fig)
    print("[OK] fig3_behavior.png/pdf")


if __name__ == "__main__":
    fig_time()
    fig_space()
    fig_behavior()
    print("\n全部组合图已生成到:", OUT)

#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
plot_cdf.py — 绘制 PSN 查找延迟 CDF 与占用扫描曲线
-------------------------------------------------
读取 psn_bench 输出的 csv，生成论文可用的图（PNG + PDF）。

依赖: numpy, matplotlib
安装: pip install numpy matplotlib   （或 apt install python3-matplotlib）

用法:
    python3 plot_cdf.py --dir . --out .        # 画 CDF（random+sequential）
    python3 plot_cdf.py --dir . --sweep        # 画占用扫描 scaling.csv
    python3 plot_cdf.py --dir . --all          # 两个都画

输出:
    cdf_lookup.png / cdf_lookup.pdf   （随机 vs 顺序 两子图）
    scaling.png / scaling.pdf         （中位延迟 vs 缓存占用 N）
"""

import argparse
import os
import sys

import numpy as np

try:
    import matplotlib
    matplotlib.use("Agg")  # 无显示环境也能出图
    import matplotlib.pyplot as plt
except ImportError:
    sys.exit("缺少 matplotlib，请先: pip install numpy matplotlib")

# 方法与显示名、颜色（论文里常用的区分度高的配色）
METHODS = [
    ("fifo",         "FIFO Queue",        "#d62728"),  # 红
    ("chained_hash", "Chained Hash",      "#ff7f0e"),  # 橙
    ("balanced_tree","Balanced Tree",     "#2ca02c"),  # 绿
    ("psn_mapping",  "PSN Mapping (ours)", "#1f77b4"), # 蓝（强调，加粗）
]
LINE_WIDTH = {"psn_mapping": 2.8}
MARKERS = {"fifo": "s", "chained_hash": "^", "balanced_tree": "o", "psn_mapping": "D"}


def load_sorted(dirpath, method, pattern):
    """读取 cdf_<method>_<pattern>.csv 的 latency_ns 列（已排序）。"""
    path = os.path.join(dirpath, f"cdf_{method}_{pattern}.csv")
    if not os.path.exists(path):
        return None
    vals = np.loadtxt(path, delimiter=",", skiprows=1, usecols=0)
    if vals.ndim == 0:
        vals = vals[None]
    return np.sort(vals)


def find_data_dir(explicit=None):
    """自动定位 csv 所在目录。

    优先级：显式 --dir > 当前目录 > ./out > ./out 的兄弟目录。
    返回 (dirpath, 是否找到至少一个 cdf_*.csv)。
    """
    candidates = []
    if explicit is not None:
        candidates.append(explicit)
    candidates += [".", "out", "./out"]
    seen = set()
    for d in candidates:
        if d in seen or not os.path.isdir(d):
            continue
        seen.add(d)
        if any(f.startswith("cdf_") and f.endswith(".csv") for f in os.listdir(d)):
            return d, True
    return (candidates[0] if candidates else "."), False


def plot_cdf(dirpath, outdir):
    # 先检查有没有数据，没有就直接报错，避免画出空图
    found_any = False
    for key, _label, _color in METHODS:
        for pattern in ("random", "sequential"):
            if load_sorted(dirpath, key, pattern) is not None:
                found_any = True
    if not found_any:
        sys.exit(
            f"[错误] 在目录 {dirpath!r} 下没有找到任何 cdf_*.csv。\n"
            "请先运行基准测试生成数据，例如:\n"
            "  ./psn_bench -o ./out\n"
            "然后指定数据目录:\n"
            "  python3 plot_cdf.py --dir ./out --out ./out\n"
        )

    fig, axes = plt.subplots(1, 2, figsize=(12, 5), sharey=True)
    for ax, pattern in zip(axes, ["random", "sequential"]):
        for key, label, color in METHODS:
            x = load_sorted(dirpath, key, pattern)
            if x is None or len(x) == 0:
                continue
            y = np.arange(1, len(x) + 1) / len(x)
            ax.plot(x, y, label=label, color=color,
                    lw=LINE_WIDTH.get(key, 1.8),
                    marker=MARKERS[key], markevery=max(1, len(x) // 20),
                    markersize=4)
        ax.set_xscale("log")
        ax.set_xlabel("Lookup latency (ns, log scale)")
        ax.set_ylabel("CDF  P(X ≤ latency)")
        ax.set_title(f"Access pattern: {pattern}")
        ax.grid(True, which="both", ls="--", alpha=0.4)
        ax.legend(loc="lower right", fontsize=9)

    fig.suptitle("PSN Lookup Latency CDF — RDMA Gateway Cache Structures",
                 fontsize=13)
    fig.tight_layout(rect=[0, 0, 1, 0.96])
    for ext in ("png", "pdf"):
        fig.savefig(os.path.join(outdir, f"cdf_lookup.{ext}"), dpi=300,
                    bbox_inches="tight")
    print(f"[OK] 已保存 cdf_lookup.png / cdf_lookup.pdf")


def plot_scaling(dirpath, outdir):
    path = os.path.join(dirpath, "scaling.csv")
    if not os.path.exists(path):
        print("[跳过] 未找到 scaling.csv（先用 --sweep 生成）")
        return
    data = {}
    with open(path) as f:
        next(f)  # 跳过表头
        for line in f:
            line = line.strip()
            if not line:
                continue
            method, n, med = line.split(",")
            data.setdefault(method, ([], []))
            data[method][0].append(int(n))
            data[method][1].append(float(med))

    fig, ax = plt.subplots(figsize=(7.5, 5))
    for key, label, color in METHODS:
        if key not in data:
            continue
        xs, ys = data[key]
        order = np.argsort(xs)
        xs = np.array(xs)[order]
        ys = np.array(ys)[order]
        ax.plot(xs, ys, label=label, color=color, marker=MARKERS[key],
                lw=LINE_WIDTH.get(key, 1.8), markersize=5)

    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_xlabel("Cache occupancy N (packets)")
    ax.set_ylabel("Median lookup latency (ns)")
    ax.set_title("Median Lookup Latency vs. Cache Occupancy")
    ax.grid(True, which="both", ls="--", alpha=0.4)
    ax.legend(fontsize=9)
    fig.tight_layout()
    for ext in ("png", "pdf"):
        fig.savefig(os.path.join(outdir, f"scaling.{ext}"), dpi=300,
                    bbox_inches="tight")
    print("[OK] 已保存 scaling.png / scaling.pdf")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default=None,
                    help="csv 所在目录（默认自动在 . / out / ./out 中查找）")
    ap.add_argument("--out", default=".", help="图片输出目录")
    ap.add_argument("--sweep", action="store_true", help="画 scaling 图")
    ap.add_argument("--all", action="store_true", help="CDF + scaling 都画")
    args = ap.parse_args()

    dirpath, found = find_data_dir(args.dir)
    if not found:
        sys.exit(
            f"[错误] 在目录 {dirpath!r} 下没有找到任何 cdf_*.csv。\n"
            "请先运行基准测试生成数据，例如:\n"
            "  ./psn_bench -o ./out\n"
            "然后指定数据目录:\n"
            "  python3 plot_cdf.py --dir ./out --out ./out\n"
        )
    print(f"[数据目录] 使用 {os.path.abspath(dirpath)}")

    os.makedirs(args.out, exist_ok=True)
    if args.all:
        plot_cdf(dirpath, args.out)
        plot_scaling(dirpath, args.out)
    elif args.sweep:
        plot_scaling(dirpath, args.out)
    else:
        plot_cdf(dirpath, args.out)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
plot_space.py — 绘制 RDMA 网关「缓存空间开销」对比图
-------------------------------------------------
读取 space_bench 输出的 csv，生成论文可用的图（PNG + PDF）。

依赖: numpy, matplotlib
安装: pip install numpy matplotlib   （或 apt install python3-matplotlib）

用法:
    python3 plot_space.py --dir . --out .              # 空间利用率 + 绝对空间
    python3 plot_space.py --dir . --out . --multiflow  # 额外画多流摊销

输出:
    space_utilization.png / space_utilization.pdf  （主图：利用率 vs 包大小）
    space_absolute.png    / space_absolute.pdf     （辅助图：每包平均分配字节）
    space_multiflow.png   / space_multiflow.pdf    （可选：多流摊销）
"""

import argparse
import os
import sys

import numpy as np

try:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
except ImportError:
    sys.exit("缺少 matplotlib，请先: pip install numpy matplotlib")

# 方法与显示名、颜色、线型（与时间实验配色保持一致，新增两个方法）
METHODS = [
    ("fifo",            "FIFO Queue",          "#d62728", "-"),
    ("chained_hash",    "Chained Hash",        "#ff7f0e", "-"),
    ("balanced_tree",   "Balanced Tree",       "#2ca02c", "-"),
    ("psn_map_fixed",   "PSN Map (fixed 5KB)", "#9467bd", "-"),
    ("psn_map_dynamic", "PSN Map (dynamic)",   "#1f77b4", "-"),  # 强调：我们的方法
    ("contiguous",      "Contiguous (ideal)",  "#7f7f7f", "--"),  # 理想下界
]
LINE_WIDTH = {"psn_map_dynamic": 2.8, "contiguous": 2.2}
MARKERS = {"fifo": "s", "chained_hash": "^", "balanced_tree": "o",
           "psn_map_fixed": "v", "psn_map_dynamic": "D", "contiguous": "*"}


def load_summary(dirpath):
    """读取 space_summary.csv，返回 {method: (L_list, util_list, avg_list)}。"""
    path = os.path.join(dirpath, "space_summary.csv")
    if not os.path.exists(path):
        sys.exit(f"[错误] 未找到 {path}，请先运行 ./space_bench -o {dirpath}")
    data = {}
    with open(path) as f:
        header = next(f).strip().split(",")
        # method,packet_size,N,payload_bytes,allocated_bytes,utilization,avg_bytes_per_pkt
        for line in f:
            line = line.strip()
            if not line:
                continue
            cols = line.split(",")
            method = cols[0]
            L = int(cols[1])
            util = float(cols[5])
            avg = float(cols[6])
            data.setdefault(method, ([], [], []))
            data[method][0].append(L)
            data[method][1].append(util * 100.0)  # 转成百分比
            data[method][2].append(avg)
    # 按 L 排序
    for m in data:
        order = np.argsort(data[m][0])
        data[m] = (np.array(data[m][0])[order],
                   np.array(data[m][1])[order],
                   np.array(data[m][2])[order])
    return data


def plot_utilization(dirpath, outdir, data):
    fig, ax = plt.subplots(figsize=(8, 5.5))
    for key, label, color, ls in METHODS:
        if key not in data:
            continue
        L, util, _ = data[key]
        ax.plot(L, util, label=label, color=color, ls=ls,
                lw=LINE_WIDTH.get(key, 1.8), marker=MARKERS[key], markersize=5)

    ax.set_xscale("log")
    ax.set_xlabel("Packet size L (bytes, log scale)")
    ax.set_ylabel("Space utilization  payload / allocated (%)")
    ax.set_title("Cache Space Utilization vs. Packet Size")
    ax.set_ylim(0, 105)
    ax.grid(True, which="both", ls="--", alpha=0.4)
    ax.legend(fontsize=9, loc="lower right")
    fig.tight_layout()
    for ext in ("png", "pdf"):
        fig.savefig(os.path.join(outdir, f"space_utilization.{ext}"), dpi=300,
                    bbox_inches="tight")
    print("[OK] 已保存 space_utilization.png / space_utilization.pdf")


def plot_absolute(dirpath, outdir, data):
    fig, ax = plt.subplots(figsize=(8, 5.5))
    for key, label, color, ls in METHODS:
        if key not in data:
            continue
        L, _, avg = data[key]
        ax.plot(L, avg, label=label, color=color, ls=ls,
                lw=LINE_WIDTH.get(key, 1.8), marker=MARKERS[key], markersize=5)

    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_xlabel("Packet size L (bytes, log scale)")
    ax.set_ylabel("Avg allocated bytes per packet (log scale)")
    ax.set_title("Absolute Space Cost vs. Packet Size")
    ax.grid(True, which="both", ls="--", alpha=0.4)
    ax.legend(fontsize=9)
    fig.tight_layout()
    for ext in ("png", "pdf"):
        fig.savefig(os.path.join(outdir, f"space_absolute.{ext}"), dpi=300,
                    bbox_inches="tight")
    print("[OK] 已保存 space_absolute.png / space_absolute.pdf")


def plot_multiflow(dirpath, outdir):
    path = os.path.join(dirpath, "space_multiflow.csv")
    if not os.path.exists(path):
        print("[跳过] 未找到 space_multiflow.csv（先用 ./space_bench --multiflow 生成）")
        return
    amort = {"npp": [], "util": []}
    with open(path) as f:
        next(f)  # 表头
        for line in f:
            line = line.strip()
            if not line:
                continue
            cols = line.split(",")
            # scenario,K,npp,total_packets,packet_size,ring,block,alloc,util
            scenario = cols[0]
            if scenario == "amortize":
                amort["npp"].append(int(cols[2]))
                amort["util"].append(float(cols[8]) * 100.0)

    if not amort["npp"]:
        return
    order = np.argsort(amort["npp"])
    npp = np.array(amort["npp"])[order]
    util = np.array(amort["util"])[order]

    fig, ax = plt.subplots(figsize=(7.5, 5))
    ax.plot(npp, util, label="PSN Map (dynamic)", color="#1f77b4",
            marker="D", lw=2.8, markersize=5)
    ax.set_xscale("log")
    ax.set_xlabel("Per-connection occupancy npp (packets)")
    ax.set_ylabel("Space utilization (%)")
    ax.set_title("Ring Buffer Overhead Amortization (PSN Mapping)")
    ax.grid(True, which="both", ls="--", alpha=0.4)
    ax.legend(fontsize=9)
    fig.tight_layout()
    for ext in ("png", "pdf"):
        fig.savefig(os.path.join(outdir, f"space_multiflow.{ext}"), dpi=300,
                    bbox_inches="tight")
    print("[OK] 已保存 space_multiflow.png / space_multiflow.pdf")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default=".", help="csv 所在目录")
    ap.add_argument("--out", default=".", help="图片输出目录")
    ap.add_argument("--multiflow", action="store_true", help="额外画多流摊销图")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    data = load_summary(args.dir)
    plot_utilization(args.dir, args.out, data)
    plot_absolute(args.dir, args.out, data)
    if args.multiflow:
        plot_multiflow(args.dir, args.out)


if __name__ == "__main__":
    main()

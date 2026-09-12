#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
plot_behavior.py — 绘制 RDMA 网关「档位标记数组 + 分档环形数组」存/取行为图
----------------------------------------------------------------------------
读取 behavior_bench 输出的 csv，生成论文可用的图（PNG + PDF）。

依赖: numpy, matplotlib
安装: pip install numpy matplotlib   （或 apt install python3-matplotlib）

用法:
    python3 plot_behavior.py --dir . --out .      # 全部图

输出:
    cdf_store.png / pdf      （顺序 store vs 乱序 store 延迟 CDF，标 P50/P90/P99）
    cdf_retrieve.png / pdf   （found vs lost retrieve 延迟 CDF，标 P50/P90/P99）
    loss_sweep.png / pdf     （store/retrieve P50 vs 丢包率，双线，证明无关）
    correctness.png / pdf    （丢包判定准确率条形图，标 100% 理想参考线）
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

# 论文配色
C_STORE_SEQ = "#1f77b4"
C_STORE_OOO = "#ff7f0e"
C_RETR_FOUND = "#2ca02c"
C_RETR_LOST = "#d62728"


def load_latency(dirpath, name):
    path = os.path.join(dirpath, name)
    if not os.path.exists(path):
        return None
    vals = np.loadtxt(path, delimiter=",", skiprows=1, usecols=0)
    if vals.ndim == 0:
        vals = vals[None]
    return np.sort(vals)


def plot_cdf_file(fig_path, title, series):
    """series: list of (x, label, color, lw, marker)"""
    fig, ax = plt.subplots(figsize=(8, 5.5))
    for x, label, color, lw, marker in series:
        if x is None or len(x) == 0:
            continue
        y = np.arange(1, len(x) + 1) / len(x)
        ax.plot(x, y, label=label, color=color, lw=lw, marker=marker,
                markevery=max(1, len(x) // 20), markersize=4)
        # 标 P50/P90/P99 竖线
        for pct, ls in ((50, "--"), (90, ":"), (99, "-.")):
            v = np.percentile(x, pct)
            ax.axvline(v, color=color, ls=ls, alpha=0.45, lw=0.9)
            ax.annotate(f"P{pct}={v:.1f}ns", xy=(v, 0.02),
                        xytext=(v, 0.06 + 0.04 * (pct // 40)),
                        color=color, fontsize=7, rotation=90)

    ax.set_xscale("log")
    ax.set_xlabel("Latency (ns, log scale)")
    ax.set_ylabel("CDF  P(X ≤ latency)")
    ax.set_title(title)
    ax.grid(True, which="both", ls="--", alpha=0.4)
    ax.legend(loc="lower right", fontsize=9)
    fig.tight_layout()
    for ext in ("png", "pdf"):
        fig.savefig(f"{fig_path}.{ext}", dpi=300, bbox_inches="tight")
    print(f"[OK] 已保存 {fig_path}.png / {fig_path}.pdf")


def plot_cdf_store(dirpath, outdir):
    seq = load_latency(dirpath, "A_store_sequential.csv")
    ooo = load_latency(dirpath, "B_store_out_of_order.csv")
    series = [
        (seq, "Store (sequential)", C_STORE_SEQ, 2.6, "o"),
        (ooo, "Store (out-of-order)", C_STORE_OOO, 2.6, "s"),
    ]
    plot_cdf_file(os.path.join(outdir, "cdf_store"),
                  "Store Latency CDF — Tiered PSN Mapping", series)


def plot_cdf_retrieve(dirpath, outdir):
    found = load_latency(dirpath, "C_retrieve_found.csv")
    lost = load_latency(dirpath, "C_retrieve_lost.csv")
    series = [
        (found, "Retrieve (found)", C_RETR_FOUND, 2.6, "^"),
        (lost, "Retrieve (lost / miss)", C_RETR_LOST, 2.6, "v"),
    ]
    plot_cdf_file(os.path.join(outdir, "cdf_retrieve"),
                  "Retrieve Latency CDF — Found vs Lost", series)


def plot_loss_sweep(dirpath, outdir):
    path = os.path.join(dirpath, "E_loss_sweep.csv")
    if not os.path.exists(path):
        print("[跳过] 未找到 E_loss_sweep.csv")
        return
    data = np.loadtxt(path, delimiter=",", skiprows=1)
    if data.ndim == 1:
        data = data[None]
    rate = data[:, 0] * 100.0
    store = data[:, 1]
    found = data[:, 2]
    lost = data[:, 3]
    acc = data[:, 4]

    fig, ax1 = plt.subplots(figsize=(8, 5.5))
    ax1.plot(rate, store, label="Store P50", color=C_STORE_SEQ,
             marker="o", lw=2.4, markersize=5)
    ax1.plot(rate, found, label="Retrieve P50 (found)", color=C_RETR_FOUND,
             marker="^", lw=2.4, markersize=5)
    ax1.plot(rate, lost, label="Retrieve P50 (lost)", color=C_RETR_LOST,
             marker="v", lw=2.4, markersize=5)
    ax1.set_xscale("log")
    ax1.set_xlabel("Packet loss rate (%)")
    ax1.set_ylabel("P50 latency (ns)")
    ax1.grid(True, which="both", ls="--", alpha=0.4)

    ax2 = ax1.twinx()
    ax2.plot(rate, acc, label="Loss detection accuracy", color="#7f7f7f",
             marker="*", lw=1.8, ls="--", markersize=6)
    ax2.set_ylim(0, 105)
    ax2.set_ylabel("Detection accuracy (%)")
    ax2.axhline(100.0, color="#7f7f7f", ls=":", lw=1.0, alpha=0.6)

    lines1, labels1 = ax1.get_legend_handles_labels()
    lines2, labels2 = ax2.get_legend_handles_labels()
    ax1.legend(lines1 + lines2, labels1 + labels2, fontsize=8, loc="center right")
    ax1.set_title("Store/Retrieve Latency vs. Packet Loss Rate")
    fig.tight_layout()
    for ext in ("png", "pdf"):
        fig.savefig(os.path.join(outdir, f"loss_sweep.{ext}"), dpi=300,
                    bbox_inches="tight")
    print("[OK] 已保存 loss_sweep.png / loss_sweep.pdf")


def plot_correctness(dirpath, outdir):
    path = os.path.join(dirpath, "E_loss_sweep.csv")
    if not os.path.exists(path):
        print("[跳过] 未找到 E_loss_sweep.csv")
        return
    data = np.loadtxt(path, delimiter=",", skiprows=1)
    if data.ndim == 1:
        data = data[None]
    labels = [f"{r:.1f}%" for r in (data[:, 0] * 100.0)]
    acc = data[:, 4]

    fig, ax = plt.subplots(figsize=(8, 4.5))
    x = np.arange(len(labels))
    bars = ax.bar(x, acc, color="#1f77b4", width=0.6, label="Detection accuracy")
    ax.axhline(100.0, color="#d62728", ls="--", lw=1.4, label="Ideal 100%")
    ax.set_ylim(90, 105)
    ax.set_xticks(x)
    ax.set_xticklabels(labels)
    ax.set_xlabel("Packet loss rate")
    ax.set_ylabel("Loss detection accuracy (%)")
    ax.set_title("Loss Detection Accuracy vs. Packet Loss Rate (ideal = 100%)")
    ax.grid(True, axis="y", ls="--", alpha=0.4)
    ax.legend(fontsize=9)
    for i, v in enumerate(acc):
        ax.annotate(f"{v:.2f}%", xy=(x[i], v), xytext=(0, 4),
                    textcoords="offset points", ha="center", fontsize=7)
    fig.tight_layout()
    for ext in ("png", "pdf"):
        fig.savefig(os.path.join(outdir, f"correctness.{ext}"), dpi=300,
                    bbox_inches="tight")
    print("[OK] 已保存 correctness.png / correctness.pdf")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default=".", help="csv 所在目录")
    ap.add_argument("--out", default=".", help="图片输出目录")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    plot_cdf_store(args.dir, args.out)
    plot_cdf_retrieve(args.dir, args.out)
    plot_loss_sweep(args.dir, args.out)
    plot_correctness(args.dir, args.out)


if __name__ == "__main__":
    main()

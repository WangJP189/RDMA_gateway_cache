#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Fig 3 — 动态内存块自适应：S 阶梯 + 利用率（exp3_phased）。

两个子图（同一横轴 = 纪元 epoch）：
  (a) S 阶梯 + 阶段背景：S_after 随纪元的变化（阶梯线），浅色底纹标出
      四个阶段的包长（4096 B / 1024 B），并标注 resize 事件（shrink /
      阈值 grow / 紧急 overflow_grow）。
  (b) 利用率：util_adaptive（dynblock 自适应档位）vs util_static
      （兜底 MTU 固定档位）。framing：MTU 阶段两者同样高（~99%，因为
      S=MTU=4096 恰好匹配）；差别只在 <MTU 阶段（自适应 95.9% vs 静态 24.7%）。

输入（实验三 phased 运行的输出，全部已跑）：
    out/exp3_phased/resolved_config.json  阶段表（e3_phases：epochs×pkt_len）
    out/exp3_phased/S_trace.csv            epoch,phase,S_after,event,...
    out/exp3_phased/utilization_trace.csv  epoch,phase,util_adaptive,util_static
    out/exp3_phased/resize_events.csv      epoch,S_old,S_new,reason,moved_pkts

输出：paper_figures/fig3_adaptive.{png,pdf} @ 300 dpi。

风格沿用 260912 paper_figures/plot_figures.py：serif（Times 度量兼容替代）、
ColorBrewer 蓝灰系、我们的方法用深蓝 #08306b 高亮。
"""

import os
import csv
import json
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

# ---- serif 字体（Times 度量兼容替代）----
plt.rcParams["font.family"] = "serif"
plt.rcParams["font.serif"] = ["Times New Roman", "Liberation Serif",
                              "DejaVu Serif"]
plt.rcParams["axes.unicode_minus"] = False
plt.rcParams["font.size"] = 9

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT = os.path.join(ROOT, "out", "exp3_phased")

OURS = "#08306b"          # 深蓝高亮（我们的方法 / 自适应）
STATIC = "#969696"        # 灰（静态兜底 MTU 基线）
PHASE_FILL = "#eceff4"    # 阶段底纹
PHASE_FILL_ALT = "#f6f8fb"


def read_csv(name):
    path = os.path.join(OUT, name)
    with open(path, "r", encoding="utf-8") as f:
        return list(csv.DictReader(f))


def read_json(name):
    path = os.path.join(OUT, name)
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def phases_from_cfg(cfg):
    """从 resolved_config.json 取阶段表 → [(start_epoch, end_epoch, pkt_len), ...]"""
    n = int(cfg["e3_phase_n"])
    out, cum = [], 0
    for k in range(n):
        epochs = int(cfg["e3_phases"][k][0])
        pkt = int(cfg["e3_phases"][k][1])
        if epochs <= 0:
            continue
        out.append((cum, cum + epochs, pkt))
        cum += epochs
    return out


def main():
    cfg = read_json("resolved_config.json")
    phases = phases_from_cfg(cfg)

    s_rows = read_csv("S_trace.csv")
    util_rows = read_csv("utilization_trace.csv")
    resize_rows = read_csv("resize_events.csv")

    epoch = np.array([int(r["epoch"]) for r in s_rows], dtype=float)
    S = np.array([int(r["S_after"]) for r in s_rows], dtype=float)
    util_a = np.array([float(r["util_adaptive"]) for r in util_rows])
    util_s = np.array([float(r["util_static"]) for r in util_rows])
    u_epoch = np.array([int(r["epoch"]) for r in util_rows], dtype=float)

    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(7.0, 5.4), sharex=True)

    # ================= (a) S 阶梯 + 阶段背景 =================
    # 阶段底纹（半开区间 [start, end)）+ 包长标签（上部水平居中，含最后一个阶段）
    for i, (a, b, pkt) in enumerate(phases):
        color = PHASE_FILL if i % 2 == 0 else PHASE_FILL_ALT
        ax1.axvspan(a, b, facecolor=color, edgecolor="none", zorder=0)
        ax1.text((a + b) / 2.0, 0.92, "%d B" % pkt, ha="center", va="top",
                 fontsize=8, color="#555555",
                 transform=ax1.get_xaxis_transform())

    # S 阶梯（steps-post：S 在纪元内保持不变，下一纪元边界才切换）
    ax1.step(epoch, S, where="post", color=OURS, lw=2.0, zorder=3,
             label="S (adaptive block size)")

    # resize 事件标注（shrink 向下、grow/overflow_grow 向上）
    for r in resize_rows:
        e = int(r["epoch"])
        old = int(r["S_old"])
        reason = r["reason"]
        if reason == "shrink":
            label, dy = "shrink", -1
        elif reason == "overflow_grow":
            label, dy = "emerg. grow", 1
        else:
            label, dy = "threshold grow", 1
        ax1.annotate(label, xy=(e, old), xytext=(e, old + dy * 1000),
                     ha="center", fontsize=7.5, color=OURS,
                     arrowprops=dict(arrowstyle="-|>", color=OURS, lw=1.0))

    ax1.set_ylabel("Block size S (B)")
    ax1.set_ylim(512, 4900)
    ax1.set_yticks([1024, 4096])
    ax1.grid(axis="y", ls=":", color="#cccccc", zorder=1)
    ax1.legend(loc="lower left", frameon=False, fontsize=8)

    # ================= (b) 利用率 =================
    ax2.plot(u_epoch, util_a * 100.0, color=OURS, lw=2.0, marker="o",
             ms=2.5, label="adaptive (dynblock)")
    ax2.plot(u_epoch, util_s * 100.0, color=STATIC, lw=1.6, ls="--",
             label="static (fallback MTU)")
    # 95% 参考线（P2 稳态断言阈值）—— 标签放左端，避开右侧图例
    ax2.axhline(95.0, color="#b0b0b0", lw=0.8, ls=":", zorder=1)
    ax2.text(epoch.min(), 95.0, " 95%", fontsize=7.5, color="#808080",
             va="bottom", ha="left")

    # framing 说明：放在 <MTU 阶段的空档区（静态 24.7% 与自适应 95.9% 之间）
    ax2.text(9.5, 58.0,
             "MTU phase (4096 B): both ~99%\n"
             "(static matches because S=MTU)\n\n"
             "<MTU phase (1024 B):\n"
             "adaptive 95.9%  vs  static 24.7%",
             ha="left", va="center", fontsize=7.8, color="#333333")

    ax2.set_ylabel("Utilization (%)")
    ax2.set_ylim(0, 105)
    ax2.set_xlabel("Epoch")
    ax2.set_xlim(epoch.min(), epoch.max())
    ax2.grid(axis="y", ls=":", color="#cccccc", zorder=1)
    ax2.legend(loc="lower right", frameon=False, fontsize=8)

    fig.align_ylabels([ax1, ax2])
    fig.tight_layout(rect=(0, 0, 1, 0.99))

    for ext in ("png", "pdf"):
        out_path = os.path.join(HERE, "fig3_adaptive.%s" % ext)
        fig.savefig(out_path, dpi=300, bbox_inches="tight")
        print("wrote %s" % out_path)


if __name__ == "__main__":
    main()

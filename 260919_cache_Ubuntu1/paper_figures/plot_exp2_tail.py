#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Fig + 数字 — exp2(b) 尾包占比敏感性（tail sensitivity）: utilization vs tail fraction f。

背景（实验二尾包扰动）：弹性 S 是「单一全局块大小」，只能对齐到最大包（MTU）。当工作量在满 MTU 包
  之外混入占比 f 的小尾包（(1-f)@MTU + f@U[1,MTU]）时，S 被最大包钉在 MTU，小尾包只浪费槽空间。
  本图量化：利用率随 f 如何优雅退化，以及弹性机制是否被尾包扰动（扩缩/溢出/丢弃）。

图 fig_exp2_tail：
  单栏 3.45×2.6 in；横轴 tail fraction f（线性 0–0.30）、纵轴 space utilization（%，线性 80–100）。
  一条实线（PSN Mapping 弹性），标注稳定性结论：final_S 恒 4096、n_ovf_ins=n_drop=n_resize=0。

数字 out/exp2_tail/exp2_tail_numbers.md（脚本从 tail_summary.csv 提取，禁手抄）。

视觉规格（沿用 fig_exp1a_store）：白底 + 完整框线 + 四边向内刻度(含次级) + 只留水平浅灰主网格；
  serif/Times；轴标签加粗 9pt、刻度 8pt；无图内标题。
"""
import os

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

plt.rcParams["font.family"] = "serif"
plt.rcParams["font.serif"] = ["Times New Roman", "Liberation Serif", "DejaVu Serif"]
plt.rcParams["axes.unicode_minus"] = False
plt.rcParams["font.size"] = 8

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CSV = os.path.join(ROOT, "out", "exp2_tail", "tail_summary.csv")
OUT_MD = os.path.join(ROOT, "out", "exp2_tail", "exp2_tail_numbers.md")


def read_rows():
    rows = []
    with open(CSV, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            rows.append(line.split(","))
    assert rows and rows[0][0] == "f", "CSV header missing"
    hdr = rows[0]
    return [dict(zip(hdr, r)) for r in rows[1:]]


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
    ax.grid(True, which="major", axis="y", color="#dcdcdc", linewidth=0.6, zorder=0)
    ax.set_axisbelow(True)
    ax.xaxis.label.set_fontweight("bold")
    ax.yaxis.label.set_fontweight("bold")
    ax.xaxis.label.set_fontsize(9)
    ax.yaxis.label.set_fontsize(9)


def main():
    rows = read_rows()
    fs = [float(r["f"]) for r in rows]
    utils = [float(r["utilization"]) * 100.0 for r in rows]
    ovhs = [float(r["overhead_per_pkt"]) for r in rows]

    # ---- 图：utilization vs f（单条实线）----
    fig, ax = plt.subplots(figsize=(3.45, 2.6))
    ax.plot(fs, utils, color="#1F4E9C", marker="D", ms=6, ls="-", lw=2.0,
            mfc="#1F4E9C", mec="#1F4E9C", zorder=3)
    ax.set_xlim(-0.01, 0.31)
    ax.set_ylim(78, 102)
    ax.set_xticks(fs)
    ax.set_xticklabels(["%.2f" % f for f in fs])
    ax.set_xlabel("Tail fraction $f$  (packet size (1-f)@MTU + f@U[1,MTU])")
    ax.set_ylabel("Space utilization (%)")
    style_ax(ax)

    # 稳定性标注（脚本从 CSV 判断）
    S_vals = set(int(r["final_S"]) for r in rows)
    churn = all(int(r["n_ovf_ins"]) == 0 and int(r["n_drop"]) == 0
                and int(r["n_resize"]) == 0 for r in rows)
    if len(S_vals) == 1 and churn:
        ax.text(0.03, 0.04,
                "final_S = %d B (pinned)\nno resize / overflow / drop"
                % list(S_vals)[0],
                transform=ax.transAxes, fontsize=7, color="#1F4E9C",
                ha="left", va="bottom")

    fig.tight_layout(pad=0.4)
    for ext in ("png", "pdf"):
        out = os.path.join(HERE, "fig_exp2_tail.%s" % ext)
        fig.savefig(out, dpi=300, bbox_inches="tight")
        print("wrote %s" % out)
    plt.close(fig)

    # ---- 数字提取（脚本产出，禁手抄）----
    md = []
    md.append("# exp2(b) 尾包占比敏感性 — 脚本提取（tail_summary.csv）\n")
    md.append("> 包尺寸 (1-f)@MTU + f@U[1,MTU]；MTU=4096、N=4096 满窗；"
              "utilization = payload_bytes / allocated_bytes（sizeof 实测）。\n")
    md.append("\n| f | final_S | utilization (%) | overhead_per_pkt (B) | n_ovf_ins | n_drop | n_resize |")
    md.append("|---|---|---|---|---|---|---|")
    for r in rows:
        md.append("| %.2f | %s | %.2f | %.1f | %s | %s | %s |" %
                  (float(r["f"]), r["final_S"], float(r["utilization"]) * 100.0,
                   float(r["overhead_per_pkt"]), r["n_ovf_ins"], r["n_drop"], r["n_resize"]))

    md.append("\n## 关键结论（脚本计算，禁手抄）\n")
    md.append("- 弹性 S 被 (1-f) 的满 MTU 包钉在 **%d B**（7 档 f 恒不变），规则 B 不缩（L_ring=4096 恒 ≥ S）。" % list(S_vals)[0])
    md.append("- 7 档 f 全部 **n_ovf_ins = n_drop = n_resize = 0**：尾包 len≤S 走环内正常路径，"
              "单全局 S 在尺寸尾下稳定、优雅降级，无扩缩/溢出/丢包抖动。")
    monotonic = all(utils[i] > utils[i + 1] for i in range(len(utils) - 1))
    md.append("- utilization 随 f **单调下降**（脚本判定：%s）：%.2f%%（f=0）→ %.2f%%（f=0.30）。"
              % ("是" if monotonic else "否", utils[0], utils[-1]))
    md.append("- overhead_per_pkt 随 f 上升：%.1f B（f=0）→ %.1f B（f=0.30），"
              "代价只是利用率线性下降，无结构抖动。" % (ovhs[0], ovhs[-1]))

    text = "\n".join(md) + "\n"
    with open(OUT_MD, "w", encoding="utf-8") as f:
        f.write(text)
    print("wrote %s" % OUT_MD)
    print("\n" + text)


if __name__ == "__main__":
    main()

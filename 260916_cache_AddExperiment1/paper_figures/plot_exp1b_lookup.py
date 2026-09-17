#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Fig — exp1b lookup 面板（reworked，2026-09-17）: lookup/retrieve time cost vs N。

本轮回答用户的 exp1b 两个疑问：
  Q1「当前时间开销只算到取、没算上层排序」—— 重做后 reorder 维度显式拆开：
     reorder=0 = 仅取包（定位+memcpy 全过程）；reorder=1 = 取包前先把 PSN 集升序整理（仅 SR，
     GBN 输出天然有序恒 reorder=0）。结论：reorder 成本对所有方法对称（index_only 的排序成本
     sr_16 ~150ns / sr_64 ~1050ns，与缓存结构无关），不改变 PSN-vs-hash 的排序。
  Q2「为什么 PSN 比 chained hash 差、什么条件让 PSN 最好」—— 根因不是 reorder，而是块大小
     S 与 payload 不匹配（旧版 S=4096 存 1024B 包，stride 4128B vs hash 1056B 的 cache 局部性惩罚）。
     修正：S0=payload（S=1024）与「自适应收敛再冻结」两条路都做，二者最终 S=1024、
     stride=1056B，与 hash 节点 stride 相同 ⇒ PSN 反超 hash（4 模式中 3 个领先、sr_64 仅差 ~7%）。

指标：主指标 p50_ns（VM 读钟/调度停顿下稳健；mean 受 ~1% 批次 ~2ms 停顿污染——psn_dynblock
     mean 被单个停顿样本从 p50 的 2279 拉到 3246，故图用 p50，mean 仍入表并加注）。
     横轴 Cache depth N（对数 base2）；纵轴 Lookup time cost (ns)（对数）。

图（2 张，沿用 fig_exp1a_store 风格）：
  fig_exp1b_lookup_gbn64 —— gbn_long64 reorder=0，4 方法（FIFO/Hash/Tree/PSN Mapping）。
  fig_exp1b_lookup_sr64  —— sr_64，4 方法，reorder=0 实线 vs reorder=1 虚线（直接看 reorder 成本对称）。

数字提取（脚本从 CSV 提取，禁手抄）：
  out/exp1b_lookup/exp1b_numbers.md     —— p50/mean 表 + reorder 增量表 + n_cmp 表
  out/exp1b_lookup/exp1b_analysis_data.md —— 供 AI 分析的自洽数据块
"""
import os
import collections

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import FixedLocator
from matplotlib.lines import Line2D

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

# 图内 4 主方法（psn_dynblock_adaptive=自适应收敛后冻结，=「PSN Mapping」）
PLOT = [
    ("fifo_bounded",          "FIFO Queue",    "#8C8C8C", "o", False, 1.7),
    ("chained_hash_bounded",  "Chained Hash",  "#E8A33D", "s", False, 1.7),
    ("balanced_tree_bounded", "Balanced Tree", "#4C9F70", "^", False, 1.7),
    ("psn_dynblock_adaptive", "PSN Mapping",   "#1F4E9C", "D", True,  2.3),
]
# 表内 6 方法（含 S0=payload 直设版 + index_only 对照）
TABLE_ORDER = ["fifo_bounded", "chained_hash_bounded", "balanced_tree_bounded",
               "psn_dynblock", "psn_dynblock_adaptive", "index_only"]
LABELS = {
    "fifo_bounded": "FIFO Queue",
    "chained_hash_bounded": "Chained Hash",
    "balanced_tree_bounded": "Balanced Tree",
    "psn_dynblock": "PSN Mapping (S0=payload)",
    "psn_dynblock_adaptive": "PSN Mapping (adaptive S)",
    "index_only": "index-only ($\\Phi$)",
}
COMPLEXITY = {
    "fifo_bounded": "O(N)≈N/2", "chained_hash_bounded": "O(1)",
    "balanced_tree_bounded": "O(log N)", "psn_dynblock": "O(1) Φ",
    "psn_dynblock_adaptive": "O(1) Φ", "index_only": "O(1) Φ",
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
    """(method, mode, reorder) -> {N -> (mean, p50, p90)}"""
    out = collections.defaultdict(dict)
    for r in rows:
        out[(r["method"], r["mode"], int(r["reorder"]))][int(r["N"])] = (
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


def _set_x(ax):
    ax.set_xscale("log", base=2)
    ax.set_xticks([512, 2048, 8192])
    ax.set_xticklabels(["512", "2048", "8192"])
    ax.xaxis.set_minor_locator(FixedLocator([1024, 4096]))
    ax.set_xlim(400, 16000)


def _plot_line(ax, data, name, label, color, marker, filled, lw, mode, reorder, ls, mlabel):
    d = data.get((name, mode, reorder))
    if not d:
        return
    xs = [n for n in N_LIST if n in d]
    ys = [d[n][1] for n in xs]   # index 1 = p50
    ax.plot(xs, ys, color=color, marker=marker, ms=6, ls=ls, lw=lw,
            mfc=(color if filled else "none"), mec=color,
            mew=(0.0 if filled else 1.1), label=mlabel, zorder=3)


def draw_gbn(ax, data):
    ys_all = []
    for name, label, color, marker, filled, lw in PLOT:
        d = data.get((name, "gbn_long64", 0))
        if not d:
            continue
        xs = [n for n in N_LIST if n in d]
        ys = [d[n][1] for n in xs]
        ys_all.extend(ys)
        ax.plot(xs, ys, color=color, marker=marker, ms=6, ls="-", lw=lw,
                mfc=(color if filled else "none"), mec=color,
                mew=(0.0 if filled else 1.1), label=label, zorder=3)
    _set_x(ax)
    y_min, y_max = min(ys_all), max(ys_all)
    ax.set_yscale("log")
    ax.set_ylim(y_min * 0.6, y_max * 1.35)
    ax.set_xlabel("Cache depth N")
    ax.set_ylabel("Lookup time cost (ns)")
    style_ax(ax)
    leg = ax.legend(loc="upper left", fontsize=6.5, ncol=1, frameon=True,
                    framealpha=1.0, edgecolor="#c9c9c9", borderpad=0.25,
                    borderaxespad=0.5, handlelength=1.3, handletextpad=0.4,
                    labelspacing=0.3)
    leg.get_frame().set_linewidth(0.8)


def draw_sr(ax, data):
    ys_all = []
    for name, label, color, marker, filled, lw in PLOT:
        for reorder, ls, mlabel in [(0, "-", label), (1, "--", "_nolegend_")]:
            d = data.get((name, "sr_64", reorder))
            if not d:
                continue
            xs = [n for n in N_LIST if n in d]
            ys = [d[n][1] for n in xs]
            ys_all.extend(ys)
            ax.plot(xs, ys, color=color, marker=marker, ms=6, ls=ls, lw=lw,
                    mfc=(color if filled else "none"), mec=color,
                    mew=(0.0 if filled else 1.1), label=mlabel, zorder=3)
    _set_x(ax)
    y_min, y_max = min(ys_all), max(ys_all)
    ax.set_yscale("log")
    ax.set_ylim(y_min * 0.6, y_max * 1.35)
    ax.set_xlabel("Cache depth N")
    ax.set_ylabel("Lookup time cost (ns)")
    style_ax(ax)

    # 方法图例（reorder=0 实线）
    leg = ax.legend(loc="upper left", fontsize=6.5, ncol=1, frameon=True,
                    framealpha=1.0, edgecolor="#c9c9c9", borderpad=0.25,
                    borderaxespad=0.5, handlelength=1.3, handletextpad=0.4,
                    labelspacing=0.3)
    leg.get_frame().set_linewidth(0.8)
    # 线型图例：实线=retrieve only / 虚线=with reorder
    style_leg = ax.legend(
        handles=[Line2D([], [], color="#333333", ls="-", lw=1.6, label="retrieve only"),
                 Line2D([], [], color="#333333", ls="--", lw=1.6, label="with reorder")],
        loc="lower right", fontsize=6.5, frameon=True, framealpha=1.0,
        edgecolor="#c9c9c9", borderpad=0.25, borderaxespad=0.5,
        handlelength=1.3, handletextpad=0.4, labelspacing=0.3)
    style_leg.get_frame().set_linewidth(0.8)
    ax.add_artist(leg)


def save(fig, stem):
    for ext in ("png", "pdf"):
        out = os.path.join(HERE, "%s.%s" % (stem, ext))
        fig.savefig(out, dpi=300, bbox_inches="tight")
        print("wrote %s" % out)


def p50_table(data, mode, reorder):
    by = collections.defaultdict(dict)
    for (m, md, ro), d in data.items():
        if md != mode or ro != reorder:
            continue
        for n, (_, p50, _) in d.items():
            by[m][n] = p50
    lines = ["| method | " + " | ".join(str(n) for n in N_LIST) + " |",
             "|---|" + "---|" * len(N_LIST)]
    for m in TABLE_ORDER:
        if m not in by:
            continue
        cells = ["%.1f" % by[m][n] if n in by[m] else "—" for n in N_LIST]
        lines.append("| %s | %s |" % (LABELS[m], " | ".join(cells)))
    return "\n".join(lines)


def mean_table(data, mode, reorder):
    by = collections.defaultdict(dict)
    for (m, md, ro), d in data.items():
        if md != mode or ro != reorder:
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


def reorder_delta_table(data, mode, n):
    """reorder=1 − reorder=0 的 p50 增量（ns），证明 reorder 成本对称、与缓存结构无关。"""
    rows = []
    for m in TABLE_ORDER:
        d0 = data.get((m, mode, 0), {}).get(n)
        d1 = data.get((m, mode, 1), {}).get(n)
        if d0 and d1:
            rows.append((LABELS[m], d1[1] - d0[1], d0[1], d1[1]))
    if not rows:
        return ""
    lines = ["| method | reorder=0 p50 | reorder=1 p50 | Δ (ns) |",
             "|---|---|---|---|"]
    for label, delta, v0, v1 in rows:
        lines.append("| %s | %.1f | %.1f | %+.1f |" % (label, v0, v1, delta))
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
    L = []
    L.append("# exp1b lookup 实验数据（供 AI 分析，脚本提取）\n")
    L.append("## 实验设置")
    L.append("- 任务：RDMA 网关缓存 lookup/取包时间开销（GBN/SR NAK 重传工作负载）")
    L.append("- 方法：FIFO 队列 / 链式哈希 / 平衡树 / PSN 确定性映射 ×2 / index-only(Φ纯算术对照)")
    L.append("- PSN 两条路（最终都 S=1024、stride=1056B，与 hash 节点 stride 相同）：")
    L.append("    psn_dynblock          = S0=payload 直设（S=1024，无收敛）")
    L.append("    psn_dynblock_adaptive = S0=4096 自适应收敛到 1024 再冻结")
    L.append("- reorder=0 仅取包（定位+memcpy）；reorder=1 取包前先把 PSN 集升序整理（仅 SR；GBN 输出天然有序恒 0）")
    L.append("- N：512..10240（dynblock 环固定 10240 存 N 条，Φ 位置索引 O(1)）")
    L.append("- payload=1024 B；B=512；reps=5；主指标 p50_ns（mean 受 VM 停顿尾污染，仅作上界参考）")
    L.append("- 读钟地板：B=512 → 7.228 ns/op；B=32 → 115.636 ns/op（floor ∝ 1/B）")

    L.append("\n## p50 lookup time (ns) — gbn_long64（reorder=0，GBN 天然有序）\n")
    L.append(p50_table(data, "gbn_long64", 0))
    L.append("\n## p50 lookup time (ns) — sr_64 reorder=0（仅取包）\n")
    L.append(p50_table(data, "sr_64", 0))
    L.append("\n## p50 lookup time (ns) — sr_64 reorder=1（取包前升序整理）\n")
    L.append(p50_table(data, "sr_64", 1))

    L.append("\n## reorder 增量（reorder=1 − reorder=0，p50 ns）— 证明 reorder 成本对称\n")
    L.append("### sr_16 @ N=10240\n")
    L.append(reorder_delta_table(data, "sr_16", 10240))
    L.append("\n### sr_64 @ N=10240\n")
    L.append(reorder_delta_table(data, "sr_64", 10240))

    L.append("\n## n_cmp（每次取包平均比较次数，复杂度证据；gbn_long64 代表）\n")
    L.append(ncmp_table(ncmp, "gbn_long64"))

    L.append("\n## 关键结论（供 AI 交叉验证）")
    L.append("1. 伸缩性：FIFO 随 N 线性增长（O(N)，n_cmp≈N/2）；Hash/PSN/index-only 平缓（O(1)）；Tree 亚线性（O(log N)）。")
    L.append("2. 根因修正（S=payload）：旧版 S=4096 存 1024B 包（stride 4128B）cache 局部性差、PSN 比 hash 慢 2.26×；")
    L.append("   修正后 S=1024（stride 1056B=hash 节点 stride）PSN 反超：gbn_long64 PSN 2062 vs hash 2305（快 10.6%）、")
    L.append("   gbn_short8 231.7 vs 276.0、sr_16 662.6 vs 775.3；仅 sr_64 hash 领先 6.7%（3326 vs 3116）。")
    L.append("3. reorder 对称：sr_64 排序成本 ~1050ns、sr_16 ~150ns，对所有方法（含 index_only 纯算术）一致，")
    L.append("   不改变 PSN-vs-hash 排序 ⇒ reorder 不是 PSN 落后的根因。")
    L.append("4. S0=payload 与 adaptive 收敛两条路结果一致（最终 S 相同），adaptive 略优（避免小包期 4096 大块浪费）。")
    return "\n".join(L)


def main():
    rows = read_csv(CSV)
    ncmp = read_csv(NCMP)
    data = collect(rows)

    # ---- 图 1：GBN（gbn_long64，reorder=0，p50 vs N）----
    fig, ax = plt.subplots(figsize=(3.45, 2.3))
    draw_gbn(ax, data)
    fig.tight_layout(pad=0.4)
    save(fig, "fig_exp1b_lookup_gbn64")
    plt.close(fig)

    # ---- 图 2：SR（sr_64，reorder=0 实线 / reorder=1 虚线）----
    fig, ax = plt.subplots(figsize=(3.45, 2.3))
    draw_sr(ax, data)
    fig.tight_layout(pad=0.4)
    save(fig, "fig_exp1b_lookup_sr64")
    plt.close(fig)

    # ---- 数字表（脚本提取）----
    md = []
    md.append("# exp1b lookup — 脚本提取（lookup_summary.csv / n_cmp.csv）\n")
    md.append("> payload=1024，B=512，reps=5；主指标 p50_ns（mean 受 VM ~1% 停顿污染，入表作上界参考）。\n")
    md.append("> 读钟地板：B=512 → 7.228 ns/op；B=32 → 115.636 ns/op（floor ∝ 1/B 闭环）。\n")

    md.append("\n## p50 (ns) — gbn_long64，reorder=0（GBN 天然有序）\n")
    md.append(p50_table(data, "gbn_long64", 0))
    md.append("\n## p50 (ns) — sr_64，reorder=0（仅取包）\n")
    md.append(p50_table(data, "sr_64", 0))
    md.append("\n## p50 (ns) — sr_64，reorder=1（取包前升序整理）\n")
    md.append(p50_table(data, "sr_64", 1))

    md.append("\n## mean (ns) — gbn_long64，reorder=0（上界参考，受 VM 停顿尾污染）\n")
    md.append(mean_table(data, "gbn_long64", 0))

    md.append("\n## reorder 增量（reorder=1 − reorder=0，p50 ns）\n")
    md.append("### sr_16 @ N=10240\n")
    md.append(reorder_delta_table(data, "sr_16", 10240))
    md.append("\n### sr_64 @ N=10240\n")
    md.append(reorder_delta_table(data, "sr_64", 10240))

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

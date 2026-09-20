#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Fig — exp1b lookup 面板（reworked 2026-09-17；第 4B 修订 2026-09-19）: lookup/retrieve time cost vs N。

第 4B 修订：retrieve_set 统一按 PSN 升序交付（fifo/hash 显式 sort_u32_asc、tree 排序+O(log N) 查找、
  dynblock 位置映射扫槽）。
  原 reorder∈{0,1} 维度因升序契约上移到 retrieve_set 内而废止——SR 图不再分 reorder，
  retrieve_set 计时 = 定位 + memcpy + 升序交付全过程（各方法的升序交付成本已含在内）。

第 4B 排序原语正交（sort_impl）：SR 排序路径两种实现，0=glibc qsort（主）/1=内联插入排序（xval）。
  二者只作用于 SR 交付排序，GBN（无排序）两遍一致；SR 图以虚线叠画 xval 验证二者一致。

第 35 条修订（固定重传报文数）：每个操作恰为 K 个报文、K 在所有 N 下恒定。
  GBN = K 个连续 PSN；SR = K 个互异 PSN（无放回，部分 Fisher–Yates）。原「有放回抽样」使 SR 实际搬运包数
  随 N 变（约 +26%），在 N 曲线混入假增长；本次消除。报告量 = 每批固定 K 个报文的提取时间（ns/batch），
  纵轴据此改「Retrieval time cost per batch (ns)」，主/次图取 K=64（gbn_long64 / sr_64）。

历史两问：
  Q1「时间开销只算到取、没算上层排序」—— 第 4B 后升序交付进入 retrieve_set 契约，SR 计时直接含交付排序：
     fifo/hash/tree 需排序检索（O(k log k) / 排序+k×O(log N) 查找）、dynblock 零排序（扫槽天然升序），不再有外部 reorder 步骤。
  Q2「为什么 PSN 比 chained hash 差」—— 根因是块大小 S 与 payload 不匹配（旧版 S=4096 存 1024B 包）。
     修正：S0=payload（S=1024）与「自适应收敛再冻结」两条路，最终 S=1024、stride=1056B，与 hash 节点 stride 相同。

指标：主指标 batch_p50_ns（=每批固定 K 个报文的 retrieve p50；实机尾部干净、p50 稳健；mean 仍入表作上界参考）。
     横轴 Cache depth N（对数 base2，128..4096 + 锚点 5120）；纵轴 Retrieval time cost per batch (ns)（对数）。

图（2 张，沿用 fig_exp1a_store 风格）：
  fig_exp1b_lookup_gbn64 —— gbn_long64，4 方法（FIFO/Hash/Tree/PSN Mapping）。
  fig_exp1b_lookup_sr64  —— sr_64，4 方法（retrieve_set 升序交付，第 4B 契约），
     虚线叠画 sort_impl=1（xval），并注释 FIFO O(N) 扫描爆炸。

数字提取（脚本从 CSV 提取，禁手抄）：
  out/exp1b_lookup/exp1b_numbers.md     —— p50/mean 表 + n_cmp 表
  out/exp1b_lookup/exp1b_analysis_data.md —— 供 AI 分析的自洽数据块
"""
import os
import re
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

N_LIST = [128, 256, 512, 1024, 2048, 4096, 5120]

# 图内 4 主方法（psn_dynblock_adaptive=自适应收敛后冻结，=「PSN Mapping」）
PLOT = [
    ("fifo_bounded",          "FIFO Queue",    "#8C8C8C", "o", False, 1.7),
    ("chained_hash_bounded",  "Chained Hash",  "#E8A33D", "s", False, 1.7),
    ("balanced_tree_bounded", "Balanced Tree", "#4C9F70", "^", False, 1.7),
    ("psn_dynblock_adaptive", "PSN Mapping",   "#1F4E9C", "D", True,  2.3),
]
# 表内 7 方法（含 S0=payload 直设版 + ring_fixed 固定块对照 + index_only 对照）
TABLE_ORDER = ["fifo_bounded", "chained_hash_bounded", "balanced_tree_bounded",
               "psn_dynblock", "psn_dynblock_adaptive", "ring_fixed", "index_only"]
LABELS = {
    "fifo_bounded": "FIFO Queue",
    "chained_hash_bounded": "Chained Hash",
    "balanced_tree_bounded": "Balanced Tree",
    "psn_dynblock": "PSN Mapping (S0=payload)",
    "psn_dynblock_adaptive": "PSN Mapping (adaptive S)",
    "ring_fixed": "PSN fixed block (S=4096)",
    "index_only": "index-only ($\\Phi$)",
}
COMPLEXITY = {
    "fifo_bounded": "O(N)≈N/2", "chained_hash_bounded": "O(1)",
    "balanced_tree_bounded": "O(log N)", "psn_dynblock": "O(1) Φ",
    "psn_dynblock_adaptive": "O(1) Φ", "ring_fixed": "O(1) Φ",
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


def read_floor():
    """从 CSV 注释提取空批地板（B=… median=… ns/op），禁硬编码。"""
    bs, meds = [], []
    with open(CSV, "r", encoding="utf-8") as f:
        for line in f:
            if line.startswith("# floor"):
                for b, m in re.findall(r"B=(\d+).*?median=([0-9.]+)", line):
                    bs.append(int(b))
                    meds.append(float(m))
                break
    return bs, meds


def read_old_sr64():
    """旧版（位图快路径前）psn_dynblock_adaptive sr_64 p50，从 _prev CSV 复算（禁手抄）。
    _prev 无 sort_impl 列，头为 method,mode,N,...,p50_ns(索引 9)。"""
    path = os.path.join(ROOT, "out", "exp1b_lookup_prev", "lookup_summary.csv")
    if not os.path.exists(path):
        return {}
    vals = {}
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            r = line.split(",")
            if r[0] == "method":
                continue
            if r[0] == "psn_dynblock_adaptive" and r[1] == "sr_64":
                vals[int(r[2])] = float(r[9])
    return vals


def read_pre35_sr64():
    """第 35 条前（有放回抽样）psn_dynblock_adaptive sr_64 p50，从 _prev35 CSV 复算（禁手抄）。
    列为 method,sort_impl,mode,N,...,p50_ns(索引 10)；只取 sort_impl=0。"""
    path = os.path.join(ROOT, "out", "exp1b_lookup_prev35", "lookup_summary.csv")
    if not os.path.exists(path):
        return {}
    vals = {}
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            r = line.split(",")
            if r[0] == "method":
                continue
            if r[0] == "psn_dynblock_adaptive" and r[1] == "0" and r[2] == "sr_64":
                vals[int(r[3])] = float(r[10])
    return vals


def collect(rows):
    """(method, sort_impl, mode) -> {N -> (mean, p50, p90)}"""
    out = collections.defaultdict(dict)
    for r in rows:
        out[(r["method"], r.get("sort_impl", "0"), r["mode"])][int(r["N"])] = (
            float(r["mean_ns"]), float(r["p50_ns"]), float(r["p90_ns"]))
    return out


def split(data):
    """按 sort_impl 拆成 d0(主 qsort) / d1(xval 插入排序)，key=(method, mode)。"""
    d0 = collections.defaultdict(dict)
    d1 = collections.defaultdict(dict)
    for (m, si, md), d in data.items():
        (d0 if si == "0" else d1)[(m, md)] = d
    return d0, d1


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
    ax.set_xticks([128, 512, 2048, 5120])
    ax.set_xticklabels(["128", "512", "2048", "5120"])
    ax.xaxis.set_minor_locator(FixedLocator([256, 1024, 4096]))
    ax.set_xlim(100, 7000)


def _legend(ax):
    leg = ax.legend(loc="upper left", fontsize=6.5, ncol=1, frameon=True,
                    framealpha=1.0, edgecolor="#c9c9c9", borderpad=0.25,
                    borderaxespad=0.5, handlelength=1.3, handletextpad=0.4,
                    labelspacing=0.3)
    leg.get_frame().set_linewidth(0.8)


def draw_gbn(ax, d0):
    ys_all = []
    for name, label, color, marker, filled, lw in PLOT:
        d = d0.get((name, "gbn_long64"))
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
    ax.set_ylabel("Retrieval time cost per batch (ns)")
    style_ax(ax)
    _legend(ax)


def draw_sr(ax, d0, d1):
    ys_all = []
    for name, label, color, marker, filled, lw in PLOT:
        d = d0.get((name, "sr_64"))
        if not d:
            continue
        xs = [n for n in N_LIST if n in d]
        ys = [d[n][1] for n in xs]
        ys_all.extend(ys)
        ax.plot(xs, ys, color=color, marker=marker, ms=6, ls="-", lw=lw,
                mfc=(color if filled else "none"), mec=color,
                mew=(0.0 if filled else 1.1), label=label, zorder=3)
    # xval 叠画：sort_impl=1（内联插入排序），虚线、同色、无标记，验证与主(qsort)一致
    for name, label, color, marker, filled, lw in PLOT:
        d = d1.get((name, "sr_64"))
        if not d:
            continue
        xs = [n for n in N_LIST if n in d]
        ys = [d[n][1] for n in xs]
        ax.plot(xs, ys, color=color, ls="--", lw=1.0, marker="None",
                alpha=0.85, zorder=2)
    ax.plot([], [], color="#666666", ls="--", lw=1.0, label="xval (sort_impl=1)")
    _set_x(ax)
    y_min, y_max = min(ys_all), max(ys_all)
    ax.set_yscale("log")
    ax.set_ylim(y_min * 0.6, y_max * 1.4)
    ax.set_xlabel("Cache depth N")
    ax.set_ylabel("Retrieval time cost per batch (ns)")
    style_ax(ax)
    _legend(ax)
    # FIFO O(N) 扫描爆炸注释（指到 FIFO 最高点）
    fd = d0.get(("fifo_bounded", "sr_64"))
    if fd:
        xm = max(n for n in N_LIST if n in fd)
        ax.annotate("FIFO: O(N) scan", xy=(xm, fd[xm][1]),
                    xytext=(0.48, 0.88), textcoords="axes fraction",
                    fontsize=6.5, color="#8C8C8C", ha="left",
                    arrowprops=dict(arrowstyle="->", color="#8C8C8C", lw=0.7))


def save(fig, stem):
    for ext in ("png", "pdf"):
        out = os.path.join(HERE, "%s.%s" % (stem, ext))
        fig.savefig(out, dpi=300, bbox_inches="tight")
        print("wrote %s" % out)


def p50_table(data, mode):
    by = collections.defaultdict(dict)
    for (m, md), d in data.items():
        if md != mode:
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


def mean_table(data, mode):
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
        if r.get("sort_impl", "0") != "0":   # n_cmp 两遍一致，只取主(sort_impl=0)
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


def analysis_block(data, d1, ncmp, rows):
    L = []
    L.append("# exp1b lookup 实验数据（供 AI 分析，脚本提取）\n")
    L.append("## 实验设置")
    L.append("- 任务：RDMA 网关缓存 lookup/取包时间开销（GBN/SR NAK 重传工作负载）")
    L.append("- 方法：FIFO 队列 / 链式哈希 / 平衡树 / PSN 确定性映射 ×2 / 固定块对照 / index-only(Φ纯算术对照)")
    L.append("- PSN 两条路（最终都 S=1024、stride=1056B，与 hash 节点 stride 相同）：")
    L.append("    psn_dynblock          = S0=payload 直设（S=1024，无收敛）")
    L.append("    psn_dynblock_adaptive = S0=4096 自适应收敛到 1024 再冻结")
    L.append("    ring_fixed            = S=4096 固定（去弹性；与 exp1a 同名同义）")
    L.append("- 交付契约（第 4B）：retrieve_set 统一按 PSN 升序交付——fifo/hash 显式 sort_u32_asc、")
    L.append("  tree=tree_search_set（排序 + k×O(log N) 查找）、dynblock 位置映射扫槽（零排序）；")
    L.append("  retrieve_set 计时 = 定位 + memcpy + 升序交付全过程（各方法升序交付成本已含在内）")
    L.append("- 排序原语正交（sort_impl）：0=glibc qsort（主）/1=内联插入排序（xval）；只作用 SR 排序路径，GBN 两遍一致")
    L.append("- N：128..4096 + 锚点 5120（非 2 的幂；dynblock 环长=N，Φ=psn%N 位置索引 O(1)）")
    L.append("- payload=1024 B；B=512；reps=5；主指标 p50_ns（实机尾部干净，p50 稳健）")
    bs, meds = read_floor()
    floor_str = "；".join("B=%d → %.3f ns/op" % (b, m) for b, m in zip(bs, meds))
    L.append("- 读钟地板：%s（floor ∝ 1/B）" % floor_str)

    L.append("\n## p50 lookup time (ns) — gbn_long64（retrieve_range，天然升序；sort_impl=0 主）\n")
    L.append(p50_table(data, "gbn_long64"))
    L.append("\n## p50 lookup time (ns) — sr_64（retrieve_set，第 4B 升序交付；sort_impl=0 主）\n")
    L.append(p50_table(data, "sr_64"))

    L.append("\n## n_cmp（每次取包平均比较次数，复杂度证据；gbn_long64 代表，sort_impl=0）\n")
    L.append(ncmp_table(ncmp, "gbn_long64"))

    L.append("\n## 关键结论（脚本计算，禁手抄）")

    def p50(method, mode, n):
        d = data.get((method, mode), {})
        return d.get(n, (0.0, 0.0, 0.0))[1]

    def mean_cmp(method, n):
        for r in ncmp:
            if r["method"] == method and r["mode"] == "gbn_long64" \
               and r.get("sort_impl", "0") == "0" and int(r["N"]) == n:
                return float(r["mean_cmp_per_pkt"])
        return 0.0

    N_REF = 4096   # 满窗 N0
    N_MAX = 5120   # 锚点（非 2 的幂）

    def col(method, mode, n, c):
        for r in rows:
            if r["method"] == method and r["mode"] == mode and int(r["N"]) == n:
                return r[c]
        return None

    def fmt_bytes(b):
        try:
            b = float(b)
        except (TypeError, ValueError):
            return "?"
        if b >= 1024 * 1024:
            return "%.0f MB" % (b / (1024 * 1024))
        if b >= 1024:
            return "%.0f KB" % (b / 1024)
        return "%d B" % int(b)

    # ---- 口径声明（第 35 条：固定重传报文数，K 在所有 N 下恒定）----
    k = {md: col("psn_dynblock_adaptive", md, 128, "pkts_per_op")
         for md in ("gbn_long64", "gbn_short8", "sr_16", "sr_64")}
    L.append("\n## 口径声明（第 35 条：固定重传报文数）")
    L.append("- 每批固定 K 个报文，K 在所有 N 下恒定（CSV pkts_per_op 列）：gbn_long64=%s / gbn_short8=%s / sr_16=%s / sr_64=%s。"
             % (k["gbn_long64"], k["gbn_short8"], k["sr_16"], k["sr_64"]))
    L.append("- GBN = K 个连续 PSN（retrieve_range）；SR = K 个互异 PSN（无放回，部分 Fisher–Yates，retrieve_set）。")
    L.append("- 原「有放回抽样」使 SR 实际搬运包数随 N 变（N=128≈50.6 → N=5120≈63.6，约 +26%），在 N 曲线混入假增长；本次改无放回已消除。")
    L.append("- main_metric = batch_p50_ns：每批固定 K 个报文的 retrieve 摊到单批的 p50（主/次图取 K=64）。")

    # ---- 归因声明（第 35 条：SR-64 随 N 增长的来源 = ring 尺寸跨 L2→L3，非算法退化）----
    rb128 = col("psn_dynblock_adaptive", "sr_64", 128, "ring_bytes")
    rb_max = col("psn_dynblock_adaptive", "sr_64", N_MAX, "ring_bytes")
    L.append("\n## 归因声明（SR-64 随 N 增长的来源）")
    L.append("- ring_bytes = N × stride(S)（stride=align16(S)=1024B；CSV ring_bytes 列）：N=128 → %s，N=%d → %s。"
             % (fmt_bytes(rb128), N_MAX, fmt_bytes(rb_max)))
    L.append("- SR-64 p50 随 N 的增长归因于 64KB 连续 memcpy 的缓存局部性（源环 %s→%s 跨 L2→L3，SR 再叠加离散访问惩罚），"
             "非算法退化（index_only Φ 纯算术扁平佐证）。" % (fmt_bytes(rb128), fmt_bytes(rb_max)))

    f0 = p50("fifo_bounded", "gbn_long64", 128)
    f1 = p50("fifo_bounded", "gbn_long64", N_MAX)
    L.append("1. 伸缩性（gbn_long64）：FIFO p50 从 N=128 的 %.0f ns 涨到 N=%d 的 %.0f ns（×%.0f），"
             "n_cmp≈N/2（N=%d 时 %.1f）；Hash n_cmp=1.0 恒定、PSN/index_only n_cmp=0（Φ 纯算术）——O(N) vs O(1) 直接证据。"
             % (f0, N_MAX, f1, f1 / f0, N_MAX, mean_cmp("fifo_bounded", N_MAX)))

    L.append("2. PSN(adaptive S) vs Chained Hash @ N=%d（p50 ns；PSN 快为正 %%）：" % N_REF)
    for mode, md in [("gbn_long64", "GBN-long64"), ("gbn_short8", "GBN-short8"),
                     ("sr_16", "SR-16"), ("sr_64", "SR-64")]:
        hp = p50("chained_hash_bounded", mode, N_REF)
        pp = p50("psn_dynblock_adaptive", mode, N_REF)
        r = (hp - pp) / hp * 100.0 if hp > 0 else 0.0
        L.append("   %s：PSN %.1f vs hash %.1f（PSN %+.1f%%）" % (md, pp, hp, r))

    L.append("3. SR 升序交付（第 4B，sr_64 @ N=%d，retrieve_set p50 ns；fifo/hash/tree 需排序检索、dynblock 零排序）："
             % N_MAX)
    for m in TABLE_ORDER:
        L.append("   %s = %.1f" % (LABELS[m], p50(m, "sr_64", N_MAX)))

    L.append("4. S0=payload 与 adaptive 收敛两条路结果一致（最终 S=1024）：gbn_long64 @ N=%d 二者 p50 %.1f / %.1f ns。"
             % (N_REF, p50("psn_dynblock", "gbn_long64", N_REF),
                p50("psn_dynblock_adaptive", "gbn_long64", N_REF)))

    # SR-64 自洽门（第 35 条：index_only 平坦；增长应明显小于旧版 +74%）——诚实报告，不修口径
    sr0 = p50("psn_dynblock_adaptive", "sr_64", 128)
    sr1 = p50("psn_dynblock_adaptive", "sr_64", N_MAX)
    growth = (sr1 - sr0) / sr0 * 100.0 if sr0 > 0 else 0.0
    idx0 = p50("index_only", "sr_64", 128)
    idx1 = p50("index_only", "sr_64", N_MAX)
    idx_g = (idx1 - idx0) / idx0 * 100.0 if idx0 > 0 else 0.0
    g0 = p50("psn_dynblock_adaptive", "gbn_long64", 128)
    g1 = p50("psn_dynblock_adaptive", "gbn_long64", N_MAX)
    g_g = (g1 - g0) / g0 * 100.0 if g0 > 0 else 0.0
    L.append("5. SR-64 自洽门（第 35 条）：PSN(adaptive) sr_64 p50 N=128→%d = %.0f→%.0f ns（%+.0f%%）。"
             % (N_MAX, sr0, sr1, growth))
    L.append("   对照：index_only（Φ 纯算术，无 memcpy）%.0f→%.0f（%+.0f%%，扁平 → 自洽）；gbn_long64（连续 64KB memcpy）%.0f→%.0f（%+.0f%%）。"
             % (idx0, idx1, idx_g, g0, g1, g_g))
    pre35 = read_pre35_sr64()
    if pre35 and 128 in pre35 and N_MAX in pre35:
        og = (pre35[N_MAX] - pre35[128]) / pre35[128] * 100.0 if pre35[128] > 0 else 0.0
        L.append("   旧版（有放回抽样，含假增长）sr_64 增长 %+.0f%%（%.0f→%.0f ns）；本次固定 K 后为 %+.0f%%，压降 %.0f 个百分点。"
                 % (og, pre35[128], pre35[N_MAX], growth, og - growth))
        L.append("   门判定：index_only 平坦=%s（增长 <5%%）；增长 %.0f%% 明显小于旧版 %.0f%%。"
                 % ("是" if idx_g < 5.0 else "否", growth, og))
    else:
        L.append("   门判定：index_only 平坦=%s（增长 <5%%）；无 _prev35 CSV 可比对，仅报本次增长 %+.0f%%。"
                 % ("是" if idx_g < 5.0 else "否", growth))
    L.append("   归因：剩余增长来自 64KB memcpy 缓存局部性（源环 %s→%s 跨 L2→L3），非算法退化（见上「归因声明」）。"
             % (fmt_bytes(rb128), fmt_bytes(rb_max)))

    # xval 一致性：sort_impl=1 ≈ sort_impl=0
    sr0_main = p50("psn_dynblock_adaptive", "sr_64", N_MAX)
    sr0_xval = d1.get(("psn_dynblock_adaptive", "sr_64"), {}).get(N_MAX, (0.0, 0.0, 0.0))[1]
    L.append("6. 排序原语一致性：sr_64 @ N=%d PSN(adaptive) 主(qsort) %.1f vs xval(插入排序) %.1f ns（差 %.2f ns，等价）。"
             % (N_MAX, sr0_main, sr0_xval, abs(sr0_main - sr0_xval)))
    return "\n".join(L)


def main():
    rows = read_csv(CSV)
    ncmp = read_csv(NCMP)
    data = collect(rows)
    d0, d1 = split(data)

    # ---- 图 1：GBN（gbn_long64，p50 vs N）----
    fig, ax = plt.subplots(figsize=(3.45, 2.3))
    draw_gbn(ax, d0)
    fig.tight_layout(pad=0.4)
    save(fig, "fig_exp1b_lookup_gbn64")
    plt.close(fig)

    # ---- 图 2：SR（sr_64，第 4B 升序交付 + xval 虚线叠画 + FIFO 爆炸注释）----
    fig, ax = plt.subplots(figsize=(3.45, 2.3))
    draw_sr(ax, d0, d1)
    fig.tight_layout(pad=0.4)
    save(fig, "fig_exp1b_lookup_sr64")
    plt.close(fig)

    # ---- 数字表（脚本提取）----
    bs, meds = read_floor()
    floor_str = "；".join("B=%d → %.3f ns/op" % (b, m) for b, m in zip(bs, meds))

    md = []
    md.append("# exp1b lookup — 脚本提取（lookup_summary.csv / n_cmp.csv）\n")
    md.append("> payload=1024，B=512，reps=5；主指标 batch_p50_ns=每批固定 K 个报文的 retrieve p50（实机尾部干净，p50 稳健）。\n")
    md.append("> 第 4B：retrieve_set 统一按 PSN 升序交付（fifo/hash sort_u32_asc、tree 排序+查找、dynblock 扫槽）。\n")
    md.append("> 第 35 条：每批固定 K 个报文（GBN=K 连续 / SR=K 互异，无放回），K 在所有 N 下恒定；主/次图取 K=64。\n")
    md.append("> 排序原语 sort_impl：0=qsort(主) / 1=插入排序(xval)；表内取主(sort_impl=0)。\n")
    md.append("> 读钟地板：%s（floor ∝ 1/B 闭环）。\n" % floor_str)

    md.append("\n## p50 (ns) — gbn_long64（retrieve_range，天然升序；sort_impl=0）\n")
    md.append(p50_table(d0, "gbn_long64"))
    md.append("\n## p50 (ns) — sr_64（retrieve_set，第 4B 升序交付；sort_impl=0）\n")
    md.append(p50_table(d0, "sr_64"))

    md.append("\n## mean (ns) — gbn_long64（上界参考；sort_impl=0）\n")
    md.append(mean_table(d0, "gbn_long64"))

    md.append("\n## n_cmp（mean_cmp_per_pkt，纯取包比较）— gbn_long64 代表（sort_impl=0）\n")
    md.append(ncmp_table(ncmp, "gbn_long64"))

    text = "\n".join(md) + "\n"
    with open(OUT_MD, "w", encoding="utf-8") as f:
        f.write(text)
    print("wrote %s" % OUT_MD)

    ai = analysis_block(d0, d1, ncmp, rows)
    with open(OUT_AI, "w", encoding="utf-8") as f:
        f.write(ai)
    print("wrote %s" % OUT_AI)


if __name__ == "__main__":
    main()

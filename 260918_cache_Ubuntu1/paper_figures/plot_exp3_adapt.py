#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Fig — exp3 弹性消融: 块大小 S 随纪元的自适应阶梯（ablation vs slow-response control）。

两条件（同一相位序列，仅弹性旋钮不同）：
  ablation   --e3-preset=ablate   k_dwell=1, ovf_thresh=1, j_quiet=1, hist_decay=2（去滞后）
  slow       --e3-preset=default  k_dwell=2, ovf_thresh=256, j_quiet=4, hist_decay=8（默认滞后）

相位序列（5 档 MTU 跳变 + 256↔4096 ×16 振荡，见 config.h §E）：
  {2,4096} 预热；{8,256}→{8,512}→{8,1024}→{8,2048}→{8,4096} 五档跳变（相位 0..5）；
  {2,256},{2,4096} ×16 快速振荡（相位 6..37，每档 2 纪元，测跟踪与稳定性）。

数字提取：out/exp3_adapt/exp3_numbers.md（脚本从 S_trace.csv / resize_events.csv /
  summary.csv / resolved_config.json 提取，禁手抄）。

视觉规格（沿用 exp1a/exp2 重画版）：serif/Times；白底完整框线；四边向内刻度(含次级)；
  只留水平浅灰主网格；轴标签加粗；图例在轴内；无图内标题；无 O(·) 标注。
  ablation=#1F4E9C 实线○，slow 对照=#8C8C8C 虚线□。
"""
import os
import json
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

plt.rcParams["font.family"] = "serif"
plt.rcParams["font.serif"] = ["Times New Roman", "Liberation Serif", "DejaVu Serif"]
plt.rcParams["axes.unicode_minus"] = False
plt.rcParams["font.size"] = 8

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT = os.path.join(ROOT, "out", "exp3_adapt")
ABL = os.path.join(ROOT, "out", "exp3_ablation")
SLOW = os.path.join(ROOT, "out", "exp3_slow")

TIERS = [256, 512, 1024, 2048, 4096]
OURS = "#1F4E9C"      # ablation（我们的方法，去滞后）
SLOWC = "#8C8C8C"     # slow-response control（默认滞后）
FILL_256 = "#eceff4"  # 256 B 相位底纹
FILL_4096 = "#f6f8fb" # 4096 B 相位底纹

OSC_START = 6   # 振荡段起始相位下标（0=预热, 1..5=五档跳变）


def read_csv(path):
    rows = []
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            rows.append(line)
    if not rows:
        return []
    hdr = rows[0].split(",")
    return [dict(zip(hdr, r.split(","))) for r in rows[1:]]


def read_json(path):
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def phases_from_cfg(cfg):
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


def s_by_epoch(rows):
    return {int(r["epoch"]): int(r["S_after"]) for r in rows}


def collect(run_dir):
    cfg = read_json(os.path.join(run_dir, "resolved_config.json"))
    s_rows = read_csv(os.path.join(run_dir, "S_trace.csv"))
    rz_rows = read_csv(os.path.join(run_dir, "resize_events.csv"))
    sum_rows = read_csv(os.path.join(run_dir, "summary.csv"))
    sm = sum_rows[0] if sum_rows else {}
    return {
        "cfg": cfg,
        "s": s_by_epoch(s_rows),
        "rz": rz_rows,
        "knobs": {k: int(cfg[k]) for k in ("k_dwell", "ovf_thresh", "j_quiet", "hist_decay")},
        "n_resize": len(rz_rows),
        "grow": sum(1 for r in rz_rows if r["reason"] in ("overflow_grow", "grow")),
        "shrink": sum(1 for r in rz_rows if r["reason"] == "shrink"),
        "resize_ns": [int(r["resize_ns"]) for r in rz_rows],
        "drain_ns": [int(r["drain_ns"]) for r in rz_rows],
        "n_drop": int(sm.get("n_drop", 0)),
        "hit": int(sm.get("hit", 0)),
        "miss": int(sm.get("miss", 0)),
        "n_ovf_ins": int(sm.get("n_ovf_ins", 0)),
    }


def latency_in_phase(smap, start, end, target):
    """相位 [start,end) 内 S 首次命中 target 的相对纪元数；未命中返回 None。"""
    for e in range(start, end):
        if smap.get(e) == target:
            return e - start
    return None


def ramp_latencies(smap, phases):
    """五档跳变（相位 1..5）每个跳变的响应延迟。"""
    out = []
    for i in range(1, OSC_START):
        start, end, mtu = phases[i]
        prev_mtu = phases[i - 1][2]
        out.append((prev_mtu, mtu, latency_in_phase(smap, start, end, mtu)))
    return out


def osc_summary(smap, phases):
    """振荡段（相位 OSC_START.. 末尾）按方向汇总：grow(256→4096) / shrink(4096→256)。
    返回 dict：方向 → (相位数, 收敛数, 未收敛数, 收敛相位延迟列表)。"""
    grow = {"n": 0, "conv": 0, "lat": []}
    shrink = {"n": 0, "conv": 0, "lat": []}
    tracked = 0
    total = 0
    for i in range(OSC_START, len(phases)):
        start, end, mtu = phases[i]
        prev_mtu = phases[i - 1][2]
        total += 1
        if smap.get(end - 1) == mtu:
            tracked += 1
        if mtu > prev_mtu:      # grow
            grow["n"] += 1
            d = latency_in_phase(smap, start, end, mtu)
            if d is not None:
                grow["conv"] += 1
                grow["lat"].append(d)
        elif mtu < prev_mtu:    # shrink
            shrink["n"] += 1
            d = latency_in_phase(smap, start, end, mtu)
            if d is not None:
                shrink["conv"] += 1
                shrink["lat"].append(d)
    return grow, shrink, tracked, total


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


def fmt_conv(conv, n, lat):
    if conv == n:
        return "%d/%d（延迟 %s 纪元）" % (conv, n, sorted(set(lat))[0] if lat and len(set(lat)) == 1 else "均%d" % (np.mean(lat)) if lat else "0")
    return "%d/%d（未收敛 %d）" % (conv, n, n - conv)


def main():
    os.makedirs(OUT, exist_ok=True)
    abl = collect(ABL)
    slow = collect(SLOW)
    phases = phases_from_cfg(abl["cfg"])
    max_epoch = max(max(abl["s"]), max(slow["s"]))

    abl_ramp = ramp_latencies(abl["s"], phases)
    slow_ramp = ramp_latencies(slow["s"], phases)
    abl_g, abl_s, abl_trk, abl_tot = osc_summary(abl["s"], phases)
    slow_g, slow_s, slow_trk, slow_tot = osc_summary(slow["s"], phases)

    def hit_rate(x):
        req = x["hit"] + x["miss"]
        return (x["hit"] / req) if req else 0.0

    md = []
    md.append("# exp3 弹性消融 — 脚本提取（S_trace.csv / resize_events.csv / summary.csv / resolved_config.json）\n")
    md.append("> 两条件同一相位序列（5 档 MTU 跳变 + 256↔4096 ×16 振荡），仅弹性旋钮不同。\n")
    md.append("\n## 弹性旋钮\n")
    md.append("| 旋钮 | ablation | slow 对照 |\n|---|---|---|")
    labels = {"k_dwell": "k_dwell（同向纪元数）", "ovf_thresh": "ovf_thresh（阈值扩条数）",
              "j_quiet": "j_quiet（缩前安静纪元数）", "hist_decay": "hist_decay（溢出史清空纪元数）"}
    for k in ("k_dwell", "ovf_thresh", "j_quiet", "hist_decay"):
        md.append("| %s | %d | %d |" % (labels[k], abl["knobs"][k], slow["knobs"][k]))

    md.append("\n## 五档跳变响应延迟（MTU 跳变后 S 首次命中目标的纪元数）\n")
    md.append("| 跳变 | ablation | slow 对照 |\n|---|---|---|")
    for (pm, m, d), (_, _, sd) in zip(abl_ramp, slow_ramp):
        md.append("| %d B → %d B | %s | %s |" % (
            pm, m,
            "%d" % d if d is not None else "未收敛",
            "%d" % sd if sd is not None else "未收敛"))

    md.append("\n## 振荡段（256↔4096 ×16，32 相位）跟踪\n")
    md.append("| 方向 | 相位数 | ablation 收敛 | slow 收敛 |\n|---|---|---|---|")
    md.append("| grow（256→4096） | %d | %s | %s |" % (
        abl_g["n"], fmt_conv(abl_g["conv"], abl_g["n"], abl_g["lat"]),
        fmt_conv(slow_g["conv"], slow_g["n"], slow_g["lat"])))
    md.append("| shrink（4096→256） | %d | %s | %s |" % (
        abl_s["n"], fmt_conv(abl_s["conv"], abl_s["n"], abl_s["lat"]),
        fmt_conv(slow_s["conv"], slow_s["n"], slow_s["lat"])))
    md.append("| 相位末跟踪率（S==MTU） | %d | %.1f%% | %.1f%% |" % (
        abl_tot, 100.0 * abl_trk / abl_tot, 100.0 * slow_trk / slow_tot))

    md.append("\n## resize 计数 / 成本\n")
    md.append("| 指标 | ablation | slow 对照 |\n|---|---|---|")
    md.append("| n_resize（总） | %d | %d |" % (abl["n_resize"], slow["n_resize"]))
    md.append("| grow（overflow_grow/grow） | %d | %d |" % (abl["grow"], slow["grow"]))
    md.append("| shrink | %d | %d |" % (abl["shrink"], slow["shrink"]))

    def ns_stats(v):
        return "mean %.0f / max %d" % (np.mean(v), max(v)) if v else "—"
    md.append("| resize_ns（单次切换） | %s | %s |" % (ns_stats(abl["resize_ns"]), ns_stats(slow["resize_ns"])))
    md.append("| drain_ns（单次排空） | %s | %s |" % (ns_stats(abl["drain_ns"]), ns_stats(slow["drain_ns"])))
    md.append("\n> resize_ns 语义：gen_switch 临界区耗时（pool_alloc/pool_free 的 mmap/munmap + 指针切换）。\n"
              "> 慢对照的均值被 6 次振荡扩的同步 munmap 拉高：其 gen_switch 被延后到 old_live 恰好归零的"
              "当次 store，旧 16.9 MB 池（4096 个已提交页）的 munmap 落在计时区内（≈0.4 ms）；\n"
              "> ablation 因 j_quiet=1/k_dwell=1 提前一个纪元排空，旧池经 release_old_if_drained 在普通 "
              "store 里释放（不计时），故 resize_ns 只剩 mmap（≈1 µs）。两者最终都 munmap 同一池，"
              "resize_ns 衡量的只是「临界区内的峰值延迟」，不是累计 mmap/munmap 总量。\n")

    md.append("\n## 稳定性（丢包 / 命中）\n")
    md.append("| 指标 | ablation | slow 对照 |\n|---|---|---|")
    md.append("| n_drop（溢出满+延后切换丢弃） | %d | %d |" % (abl["n_drop"], slow["n_drop"]))
    md.append("| n_ovf_ins（溢出插入数） | %d | %d |" % (abl["n_ovf_ins"], slow["n_ovf_ins"]))
    md.append("| hit / miss（NAK 重传） | %d / %d | %d / %d |" %
              (abl["hit"], abl["miss"], slow["hit"], slow["miss"]))
    md.append("| hit_rate | %.4f | %.4f |" % (hit_rate(abl), hit_rate(slow)))

    text = "\n".join(md) + "\n"
    with open(os.path.join(OUT, "exp3_numbers.md"), "w", encoding="utf-8") as f:
        f.write(text)
    print(text)

    # ---- 画图（单栏 3.45×2.3 in；S 阶梯 + 相位底纹） ----
    fig, ax = plt.subplots(figsize=(3.45, 2.3))
    for start, end, mtu in phases:
        if mtu == 256:
            ax.axvspan(start, end, facecolor=FILL_256, edgecolor="none", zorder=0)
        elif mtu == 4096:
            ax.axvspan(start, end, facecolor=FILL_4096, edgecolor="none", zorder=0)

    ax.plot(sorted(abl["s"]), [abl["s"][e] for e in sorted(abl["s"])],
            color=OURS, lw=2.3, marker="o", ms=3, ls="-", mfc="none", mec=OURS, mew=1.0,
            label="ablation (de-hysteresis)", zorder=3)
    ax.plot(sorted(slow["s"]), [slow["s"][e] for e in sorted(slow["s"])],
            color=SLOWC, lw=1.7, marker="s", ms=3, ls="--", mfc="none", mec=SLOWC, mew=1.0,
            label="slow control (default)", zorder=3)

    ax.set_yscale("log", base=2)
    ax.set_yticks(TIERS)
    ax.set_yticklabels([str(t) for t in TIERS])
    ax.set_ylim(180, 5500)
    ax.set_xlim(0, max_epoch)
    ax.set_xlabel("Epoch")
    ax.set_ylabel("Block size S (B)")
    style_ax(ax)

    leg = ax.legend(loc="upper right", fontsize=6.5, ncol=1, frameon=True,
                    framealpha=1.0, edgecolor="#c9c9c9", borderpad=0.3,
                    borderaxespad=0.5, handlelength=1.6, handletextpad=0.5,
                    labelspacing=0.4)
    leg.get_frame().set_linewidth(0.8)

    fig.tight_layout(pad=0.4)
    for ext in ("png", "pdf"):
        out_path = os.path.join(HERE, "fig_exp3_adapt.%s" % ext)
        fig.savefig(out_path, dpi=300, bbox_inches="tight")
        print("wrote %s" % out_path)
    plt.close(fig)


if __name__ == "__main__":
    main()

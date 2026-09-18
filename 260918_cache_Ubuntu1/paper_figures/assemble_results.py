#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
assemble_results.py —— 把四个实验的脚本提取数字组装成完整 RESULTS.md。

数字全部来自各 *_numbers.md（由对应 plot 脚本从 CSV 提取），本脚本只做：
  (1) 写头部 + 章节标题 + 图指针 + 定性结论（不含任何手抄数字；具体值一律指向
      各 numbers.md 的「关键结论」表，由脚本从 CSV 计算）；
  (2) 把 *_numbers.md 的正文（去掉各自 H1 标题行）原样拼入对应章节。

因此 RESULTS.md 完全由脚本生成，禁手抄数字。运行：
  python3 paper_figures/plot_exp1a_store.py   # exp1a 数字 + 图
  python3 paper_figures/plot_exp1b_lookup.py  # exp1b 数字 + 图
  python3 paper_figures/plot_exp2_space.py    # exp2 数字 + 图
  python3 paper_figures/plot_exp3_adapt.py    # exp3 数字 + 图
  python3 paper_figures/assemble_results.py   # 组装 RESULTS.md
"""
import os

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


def load_stripped(rel):
    """读 *_numbers.md，去掉首行 H1 标题，返回正文。"""
    with open(os.path.join(ROOT, rel), "r", encoding="utf-8") as f:
        lines = f.read().splitlines()
    for i, ln in enumerate(lines):
        if ln.startswith("# "):
            del lines[i]
            break
    while lines and not lines[0].strip():
        lines.pop(0)
    return "\n".join(lines) + "\n"


def section(title, fig, conclusion, body):
    return "\n---\n\n## %s\n\n**图：** `%s`（同目录同名 `.png` @300dpi）。\n\n%s\n\n%s" % (
        title, fig, conclusion, body)


def main():
    header = (
        "# RESULTS — 实验结果（数字由脚本从各 CSV 提取）\n\n"
        "> 生成：各实验 `plot_expX_*.py` 写 `*_numbers.md`，再由 "
        "`paper_figures/assemble_results.py` 拼成本文；表内数字禁止手抄。\n"
        "> 环境：实机 Ubuntu 22.04.5 / HWE 6.8.0-138 / i7-14700K（见 docs/ENV_CHECK.md）。\n"
    )

    e1a = section(
        "exp1a — store time cost（存时间开销）",
        "paper_figures/fig_exp1a_store.pdf",
        "横轴 packet size（256/512/1024/2048/4096 B，RDMA 5 档 MTU）、纵轴 store time cost（ns），双对数；"
        "N=4096，主指标 p50、次指标 p90。图内 4 方法；index_only 与 PSN(fixed S=4096) 只入下表。"
        "结论：PSN Mapping 在小包（256B）存时间略慢于 FIFO/Chained Hash，在大包（4096B）反超成为最快——"
        "验证「PSN 映射小包开销高、大包持平/更快」的假设（自适应 S 收敛到各档，见「收敛后 S」表）。",
        load_stripped("out/exp1a_store/exp1a_numbers.md"),
    )

    e1b = section(
        "exp1b — lookup time cost（取时间开销）",
        "paper_figures/fig_exp1b_lookup_gbn64.pdf",
        "横轴 Cache depth N（128/256/512/1024/2048/4096/5120）、纵轴 lookup time cost（ns），双对数；"
        "payload=1024、B=512、reps=5，主指标 p50。两条 PSN 变体：S0=payload（槽大小固定=包长）与 "
        "adaptive S（弹性槽大小）。结论：① FIFO 呈 O(N)（n_cmp≈N/2）、Tree O(log N)、Hash O(1)，"
        "而 PSN Mapping 两变体 n_cmp=0、与 index-only Φ 同阶（O(1) 平坦），持平/反超 Hash、远优于 Tree/FIFO；"
        "② reorder 增量对所有方法对称（见「reorder 增量」表，非 PSN 特有劣势）。"
        "SR 图另见 `fig_exp1b_lookup_sr64.pdf`（reorder=0 实线 / reorder=1 虚线）。",
        load_stripped("out/exp1b_lookup/exp1b_numbers.md"),
    )

    e2 = section(
        "exp2 — space utilization（空间利用率）",
        "paper_figures/fig_exp2_space.pdf",
        "横轴 packet size（5 档 MTU）、纵轴 space utilization（%，线性 0-100）；"
        "utilization = payload_bytes / allocated_bytes（满窗 N=4096，cache_footprint_bytes sizeof 实测）。"
        "结论：PSN Mapping（弹性）利用率非最高，但与最优基线 FIFO 的差距 <=5%（断言成立，见「关键结论」表），"
        "且优于 Chained Hash / Balanced Tree；而 PSN(fixed S=4096) 在 256B 档崩到个位数——"
        "证明「弹性内存槽大小机制」解决了固定内存块的空间利用率塌陷。",
        load_stripped("out/exp2_space/exp2_numbers.md"),
    )

    e3 = section(
        "exp3 — elastic ablation（弹性消融 / 稳定性）",
        "paper_figures/fig_exp3_adapt.pdf",
        "横轴 epoch、纵轴 block size S（B，对数 base2）；ablation（去滞后，实线○）vs slow 对照（默认滞后，虚线□），"
        "背景底纹标 256B/4096B 相位（5 档 MTU 跳变 + 256↔4096 ×16 振荡，两条件同一相位序列，仅弹性旋钮不同）。"
        "结论：去滞后消融（k_dwell=1/ovf_thresh=1/j_quiet=1/hist_decay=2）对 256↔4096 快速振荡全跟踪、"
        "零丢包零 miss（代价是 resize 次数更多）；默认滞后缩得太晚 → gen_switch 延后 → 溢出打满 → "
        "每次振荡扩丢包（见「稳定性（丢包/命中）」与「振荡段跟踪」表）。resize_ns 语义见 numbers.md 内注。",
        load_stripped("out/exp3_adapt/exp3_numbers.md"),
    )

    text = header + e1a + e1b + e2 + e3 + "\n---\n"
    out = os.path.join(ROOT, "RESULTS.md")
    with open(out, "w", encoding="utf-8") as f:
        f.write(text)
    print("wrote %s" % out)


if __name__ == "__main__":
    main()

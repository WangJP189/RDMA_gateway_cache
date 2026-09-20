#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
assemble_results.py —— 把各实验的脚本提取数字组装成完整 RESULTS.md。

数字全部来自各 *_numbers.md（由对应 plot 脚本从 CSV 提取），本脚本只做：
  (1) 写头部 + 章节标题 + 图指针 + 定性结论（不含任何手抄数字；具体值一律指向
      各 numbers.md 的「关键结论」表，由脚本从 CSV 计算）；
  (2) 把 *_numbers.md 的正文（去掉各自 H1 标题行）原样拼入对应章节。

因此 RESULTS.md 完全由脚本生成，禁手抄数字。运行：
  python3 paper_figures/plot_exp1a_store.py              # exp1a 数字 + 双面板图
  python3 paper_figures/plot_exp1a_alloc_sensitivity.py  # 第 5 条：分配敏感性图 + 数字
  python3 paper_figures/plot_exp1b_lookup.py             # exp1b 数字 + 图（含 SR-64 门诊断）
  python3 paper_figures/plot_exp2_space.py               # exp2 数字 + 双 y 轴图
  python3 paper_figures/plot_exp2_tail.py                # exp2(b) 尾包敏感性数字 + 图
  python3 paper_figures/plot_pareto.py                   # 综合权衡散点数字 + 图
  python3 paper_figures/plot_exp3_adapt.py               # exp3 数字 + 图
  python3 paper_figures/assemble_results.py              # 组装 RESULTS.md
"""
import os

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


def _read(rel):
    with open(os.path.join(ROOT, rel), "r", encoding="utf-8") as f:
        return f.read()


def load_stripped(rel):
    """读 *_numbers.md，去掉首行 H1 标题，返回正文。"""
    lines = _read(rel).splitlines()
    for i, ln in enumerate(lines):
        if ln.startswith("# "):
            del lines[i]
            break
    while lines and not lines[0].strip():
        lines.pop(0)
    return "\n".join(lines) + "\n"


def load_conclusion(rel, marker="## 关键结论"):
    """从指定 numbers/analysis 文件提取「关键结论」节（marker 到文件尾）。"""
    text = _read(rel)
    idx = text.find(marker)
    return (text[idx:] + "\n") if idx >= 0 else ""


def section(title, fig, conclusion, body):
    return "\n---\n\n## %s\n\n**图：** `%s`（同目录同名 `.png` @300dpi）。\n\n%s\n\n%s" % (
        title, fig, conclusion, body)


def main():
    header = (
        "# RESULTS — 实验结果（数字由脚本从各 CSV 提取）\n\n"
        "> 生成：各实验 `plot_expX_*.py` 写 `*_numbers.md`，再由 "
        "`paper_figures/assemble_results.py` 拼成本文；表内数字禁止手抄。\n"
        "> 环境：实机（见 docs/ENV_CHECK.md）。\n"
    )

    e1a = section(
        "exp1a — store time cost（存时间开销）",
        "paper_figures/fig_exp1a_store.pdf",
        "横轴 packet size（256/512/1024/2048/4096 B，RDMA 5 档 MTU）、纵轴 store time cost（ns），双对数；"
        "N=4096，主指标 p50、次指标 p90。左图 4 方法（FIFO Queue / Chained Hash / Balanced Tree / PSN Mapping）；"
        "右图 PSN Mapping（adaptive S）vs PSN fixed block（S=4096），逐档标注 fixed 相对 elastic 的 p50 百分比差。"
        "结论：PSN Mapping 存时间与 FIFO/Chained Hash 同阶（O(1)，见 p50 主表），全程远优于 Balanced Tree（O(log N)）"
        "——验证「PSN 映射与线性结构同阶、远优于树」；右图证明弹性槽大小使 PSN 在小包档快于固定块（百分比差见右图表），"
        "至 4096 B 两者 S 相同、差收敛到 0。",
        load_stripped("out/exp1a_store/exp1a_numbers.md"),
    )

    e1a_alloc = section(
        "exp1a-alloc — 分配策略敏感性（第 5 条正交矩阵：pooled vs perstore）",
        "paper_figures/fig_exp1a_alloc_sensitivity.pdf",
        "横轴 packet size、纵轴 store time cost（ns），双对数；4 结构各两条线（pooled 实线 / perstore 虚线）。"
        "唯一变量 = 分配策略：pooled 一次性预分配+复用（n_malloc=0）、perstore 逐 store malloc/free "
        "（n_malloc=n_free=实际 store 次数）；索引结构 / 淘汰 / 计时口径相同（alloc_equiv_test 验证字节一致）。"
        "结论：perstore 相对 pooled 的固定开销 ≈ 裸 malloc/free 成本，随 payload 增大被 memcpy 摊薄（见下表 Δ/Δ% 与关键结论）。",
        load_stripped("out/exp1a_store/alloc_sensitivity.md"),
    )

    e1b = section(
        "exp1b — lookup time cost（取时间开销）",
        "paper_figures/fig_exp1b_lookup_gbn64.pdf / fig_exp1b_lookup_sr64.pdf",
        "横轴 Cache depth N（128/256/512/1024/2048/4096/5120）、纵轴 lookup time cost（ns），双对数；"
        "payload=1024、B=512、reps=5，主指标 p50。三条 PSN 变体（S0=payload / adaptive S / ring_fixed 固定 S=4096）。"
        "第 4B 统一交付契约：retrieve_set 按 PSN 升序交付（fifo/hash 显式 sort_u32_asc、tree 排序+查找、dynblock 扫槽）。"
        "结论见下方关键结论（脚本计算）：① GBN 下 FIFO O(N)、PSN/index_only n_cmp=0 与 Φ 同阶、持平/反超 Hash；"
        "② SR 下 PSN 位图快路径零排序交付、优于 fifo/hash 的 qsort 与 tree 的中序；"
        "③ SR-64 伸缩性门未过——诊断为 64KB memcpy 缓存局部性（index_only 扁平佐证）而非算法退化，"
        "位图快路径已把旧版（位图快路径前）增长压降一个数量级以上，保留现状并如实报告（见关键结论第 5 条）。",
        load_stripped("out/exp1b_lookup/exp1b_numbers.md")
        + load_conclusion("out/exp1b_lookup/exp1b_analysis_data.md"),
    )

    e2 = section(
        "exp2 — space utilization（空间利用率）",
        "paper_figures/fig_exp2_space.pdf",
        "横轴 packet size（5 档 MTU）；左轴 space utilization（%，线性）、右轴 overhead per pkt（B，对数）。"
        "utilization = payload_bytes / allocated_bytes（满窗 N=4096，cache_footprint_bytes sizeof 实测）。"
        "5 方法：三个基线 + PSN Mapping（adaptive S）+ PSN fixed block（S=4096）。"
        "结论见下方关键结论（脚本计算）：PSN（弹性）利用率反超原最优基线 FIFO（5 档全部反超）且贴近理想上界；"
        "PSN fixed block（S=4096）在小包档利用率塌陷、overhead 冲高——证明弹性槽大小机制解决了固定块空间利用率塌陷。",
        load_stripped("out/exp2_space/exp2_numbers.md"),
    )

    e2_tail = section(
        "exp2(b) — 尾包占比敏感性（tail sensitivity）",
        "paper_figures/fig_exp2_tail.pdf",
        "横轴尾包占比 f（包尺寸 (1-f)@MTU + f@U[1,MTU]；MTU=4096、N=4096 满窗）、纵轴 space utilization（%，线性）。"
        "结论见下方关键结论（脚本计算）：弹性 S 被满 MTU 包钉在 4096、7 档 f 全程零扩缩/零溢出/零丢包，"
        "utilization 随 f 单调优雅下降——单全局 S 在尺寸尾下稳定、无结构抖动。",
        load_stripped("out/exp2_tail/exp2_tail_numbers.md"),
    )

    pareto = section(
        "综合权衡（Pareto）— store vs SR-64 lookup",
        "paper_figures/fig_pareto.pdf",
        "散点（双对数）：x = store p50 @ 1024 B pooled、y = SR-64 retrieve_set p50 @ N=4096；左下角 = 更好。"
        "5 点：三个基线 + PSN Mapping + index-only（Φ 纯算术对照）。"
        "结论见下方主导关系（脚本计算）：PSN Mapping 在两个维度同时严格优于三个基线（FIFO/Chained Hash/Balanced Tree），"
        "是唯一真实方法的 Pareto 最优点；另一个前沿点是 index-only（Φ 地板，无 payload 拷贝，"
        "但 SR 交付走排序路径故 lookup 略高于 PSN 位图快路径）。",
        load_stripped("out/pareto/pareto_numbers.md"),
    )

    e3 = section(
        "exp3 — elastic ablation（弹性消融 / 稳定性）",
        "paper_figures/fig_exp3_adapt.pdf",
        "横轴 epoch、纵轴 block size S（B，对数）；两条件同一相位序列（11 相位 = 1 预热 + 5 升档 + 5 降档，"
        "每档稳定 16 纪元再切换），仅弹性旋钮不同（ablation 去滞后 vs slow 默认滞后）。"
        "另报收敛时间、逐相位利用率、resize 计数/成本、稳定性四表。"
        "结论见下方各表（脚本计算）：每档稳定 16 纪元下两条件都零丢包零 miss（见稳定性表）；"
        "区别在收敛速度与 resize 临界区峰值延迟——默认滞后的 gen_switch 延后到 old_live 归零、旧池同步 munmap，"
        "故临界区峰值更高（见 resize 计数/成本表）。",
        load_stripped("out/exp3_adapt/exp3_numbers.md"),
    )

    text = (header + e1a + e1a_alloc + e1b + e2 + e2_tail + pareto + e3 + "\n---\n")
    out = os.path.join(ROOT, "RESULTS.md")
    with open(out, "w", encoding="utf-8") as f:
        f.write(text)
    print("wrote %s" % out)


if __name__ == "__main__":
    main()

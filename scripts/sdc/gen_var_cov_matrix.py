#!/usr/bin/env python3
# gen_var_cov_matrix.py — 生成蒙特卡洛 VaR 协方差/精度矩阵,喂给 Eigen Cholesky。
#
# 论文背景场景:银行风险引擎做蒙特卡洛 VaR(Value at Risk)计算时,需要
# 对投资组合的"风险因子协方差矩阵 Σ"做 Cholesky 分解(Σ = L·Lᵀ),再用
# L 把独立正态随机数染成具有相关性的场景因子:x = L·z。Σ 规模 = 风险因子
# 数(利率/汇率/股价/信用利差等),典型 256~1024。这个矩阵正好是稀疏结构
# (板块内强相关、跨板块弱相关),是 SimplicialCholesky 的典型工业负载。
#
# ─── 关键建模决策(论文里要讲清楚)──────────────────────────────────
# 金融图模型/风险管理的标准做法是:精度矩阵 Θ = Σ⁻¹ 天然稀疏(它的零结构
# 编码"条件独立":Θ[i,j]=0 ⟺ 风险因子 i,j 在给定其他因子下条件独立)。
# 而"板块内强相关、跨板块弱相关"的结构,其精度矩阵正好是块状稀疏的——
# 强相关块内 Θ 稠密、弱相关跨块 Θ 接近 0。所以本脚本【直接构造稀疏 Θ】,
# 而不是"构造稠密 Σ → 求逆 → 裁剪"(后者会破坏正定性、且 Σ⁻¹ 未必稀疏)。
# 这与真实金融风险模型(如 glasserman/schorfheide 类图模型)一致。
# ─────────────────────────────────────────────────────────────────
#
# 输出(CSR + CSC,双格式方便 Eigen ColMajor/Lower 喂入):
#   var_cov_N{N}_sector{SS}_corr{CS}.csc  — 4 行格式:header/Ap/Ai/Ax(CSC)
#   同名 .csr                            — 同布局(CSR,row pointers)
#   同名 .meta.json                      — 条件数/稀疏率/每列nnz/板块结构报告
#
# 用法:
#   python3 gen_var_cov_matrix.py --n 512 --sector-seed 42 --corr-seed 43
#   python3 gen_var_cov_matrix.py --selftest   # 确定性自检
#
# SPDX-License-Identifier: Apache-2.0
# Copyright 2025 Intel Corporation.

import argparse
import json
import os
import sys
import numpy as np

# GICS 11 行业板块及其在 MSCI World 中的近似市值权重(公开数据,2024 量级)。
# 用这些权重做多项分布,把每个风险因子分配到一个板块,形成分块结构。
GICS_SECTORS = [
    ("InformationTechnology", 0.299),
    ("Financials",             0.158),
    ("HealthCare",            0.119),
    ("ConsumerDiscretionary",  0.104),
    ("Industrials",           0.103),
    ("CommunicationServices", 0.087),
    ("ConsumerStaples",        0.062),
    ("Energy",                0.039),
    ("Utilities",             0.025),
    ("RealEstate",            0.023),
    ("Materials",             0.021),
]


def assign_sectors(n, sector_seed):
    """把 n 个因子【确定性】分配到 11 个 GICS 板块(按市值权重做多项分布)。

    确定性来源:np.random.default_rng(sector_seed) 是可种子化的现代 RNG,
    同一 sector_seed 必产生同一分配。返回每个因子的板块 id。
    """
    rng = np.random.default_rng(sector_seed)
    weights = np.array([w for _, w in GICS_SECTORS])
    weights = weights / weights.sum()
    sectors = rng.choice(len(GICS_SECTORS), size=n, p=weights)
    return sectors


def build_precision_matrix(n, sectors, intra_corr_range, inter_corr_range,
                            sparsity_thresh, ridge, corr_seed):
    """构造块对角稀疏正定精度矩阵 Θ = Σ⁻¹(直接构造,非求逆裁剪)。

    关键:相关系数的随机性由【显式传入的 corr_seed】控制(经
    np.random.default_rng(corr_seed)),不再从 sectors 派生。这两个种子
    互相独立,可单独做"固定行业结构、只扫相关系数种子"或反过来的对照实验。

    ─── 块对角构造(方案2,2026-08-13 重构)──────────────────────────
    不再走"构造稠密 R → 整体求逆 → 阈值裁剪"路径(该路径下,跨板块若取
    0~0.15 的正小值,则无任何条件独立,R⁻¹ 必然稠密,稀疏率只有 ~2%,
    远低于 80%~95% 目标)。改为直接按板块分组构造块对角 Θ:

    1. sectors 已按板块排序(见 generate() 的 np.sort),故同一板块的因子
       索引连续,可直接用板块边界切片(np.unique 返回排序后的板块id及边界),
       不必逐元素判断 sectors[i]==sectors[j]。
    2. 对每个板块 s(因子索引区间 [lo, hi)):
       a. 构造板块内稠密相关矩阵 R_s(维度 m=hi-lo):对角 1,非对角在
          intra_corr_range 区间均匀采样;
       b. 求逆前加 ridge·I 修正正定(应用粒度=每板块子矩阵,而非整个大
          矩阵;若某板块因子少、相关系数抽样接近奇异,提前修正);
       c. Θ_s = inv(R_s + ridge·I);
       d. (可选)板块内做保守小值裁剪:|Θ_s[i,j]| < sparsity_thresh 且
          i≠j 置 0 —— 默认不启用(见下 sparsity_thresh 语义说明),避免
          过度稀疏化削弱 SDC 触发窗口;
       e. 把 Θ_s 放进全局 Θ 的对角块位置 [lo:hi, lo:hi]。
    3. 跨板块位置:【从设计上精确 0.0】—— 从不计算、不涉及,不是阈值裁剪
       出来的浮点噪声置零。这是块对角结构的数学性质,数值上最干净。
    4. 全局 Θ 自动正定:块对角 + 每块正定 ⇒ 整体正定。

    ─── sparsity_thresh 语义变化(重要)──────────────────────────────
    旧语义:对整个 Θ 做阈值裁剪实现稀疏(现已废弃,见上)。
    新语义:【仅用于板块内部可选裁剪,默认不启用】。因跨板块位置从设计上
    就是精确 0,不再依赖阈值裁剪实现稀疏。板块内默认全保留(不裁剪任何
    元素),避免削弱 SDC 触发窗口(列更新循环每列非零元越多暴露越长)。
    若需板块内更稀疏,设 sparsity_thresh>0 启用保守裁剪,但通常不必要。

    ─── inter_corr_range 参数 ────────────────────────────────────────
    块对角设计下跨板块相关系数恒为 0(板块间条件独立 → Θ 稀疏),inter_corr_range
    不再参与构造。保留参数为向后兼容,仅作标注用(论文里说明"跨板块协方差
    设计为 0"这一建模假设),实际不采样。

    返回稀疏的 Θ(稠密 ndarray,但跨板块位置为精确 0)。
    同时通过附加返回值 sector_corr_stats 给出每板块实际相关系数分布。
    """
    rng = np.random.default_rng(corr_seed)
    Theta = np.zeros((n, n), dtype=np.float64)
    sector_corr_stats = []   # 收集每板块实际相关系数 min/mean/max,供 sanity 报告

    # beta 采样区间:对 intra_corr_range 上下限开根号。
    # 单因子模型:R_s[i,j]=beta_i*beta_j,故相关系数 = beta_i*beta_j ∈ [lo², hi²]。
    # 令 beta 区间 = [sqrt(lo), sqrt(hi)] ⇒ 相关乘积落在 [lo, hi](理论上下界)。
    beta_lo = np.sqrt(intra_corr_range[0])
    beta_hi = np.sqrt(intra_corr_range[1])

    # 找板块边界:sectors 已排序,同值连续。np.unique 返回排序后的板块id
    # 及每个板块首次出现的索引(即边界)。
    sector_ids, starts = np.unique(sectors, return_index=True)
    # 按板块 id 升序处理(保证跨板块 corr_seed 抽签顺序确定)
    for k, sid in enumerate(sector_ids):
        lo = starts[k]
        hi = starts[k + 1] if k + 1 < len(sector_ids) else n
        m = hi - lo
        if m < 2:
            # 单因子板块:Θ_s = 1/(1) = 1(对角标量),无相关系数可统计
            Theta[lo, lo] = 1.0   # R_s=1 → Θ_s=inv(1)=1;无 ridge 干扰
            sector_corr_stats.append({
                "sector": GICS_SECTORS[sid][0], "m": int(m),
                "intra_corr_min": None, "intra_corr_mean": None,
                "intra_corr_max": None})
            continue
        # 2a. 单因子模型构造 R_s(从构造上保证正定):
        #   采样载荷 beta_i ~ U[sqrt(lo), sqrt(hi)],所有 beta_i < 1(因 hi<1)
        #   R_s = beta·betaᵀ + diag(1 - beta_i²)
        #     · beta·betaᵀ 半正定(秩1)
        #     · diag(1-beta_i²) 正定(因 beta_i<1 ⇒ 1-beta_i²>0)
        #     · 和严格正定,无需特征值截断/后处理修复
        #   R_s[i,j]=beta_i*beta_j(i≠j),对角=1。这是真实量化风控单因子模型。
        beta = rng.uniform(beta_lo, beta_hi, size=m)
        R_s = np.outer(beta, beta)              # beta·betaᵀ
        np.fill_diagonal(R_s, 1.0)             # 对角强制为1(理论 1-beta²+beta²=1)
        # 2b. 极小 ridge 作为浮点安全垫(非正定修复;单因子模型已严格正定)
        R_s_reg = R_s + ridge * np.eye(m)
        # 2c. Θ_s = inv(R_s_reg)
        Theta_s = np.linalg.inv(R_s_reg)
        Theta_s = (Theta_s + Theta_s.T) / 2.0
        # 2d. 板块内可选裁剪(默认 sparsity_thresh=0.02 会做轻度裁剪)。
        #     注意:单因子模型下 R_s 已正定,但裁剪 Θ_s 可能破坏正定 →
        #     若裁剪后 min_eig<0,sanity_report 的正定性检查会捕获(见下)。
        if sparsity_thresh and sparsity_thresh > 0 and m > 2:
            off_diag = ~np.eye(m, dtype=bool)
            small = (np.abs(Theta_s) < sparsity_thresh) & off_diag
            Theta_s[small] = 0.0
            Theta_s = (Theta_s + Theta_s.T) / 2.0
        # 2e. 放进全局 Θ 对角块
        Theta[lo:hi, lo:hi] = Theta_s

        # 收集该板块实际相关系数分布(beta_i*beta_j)
        # 取 R_s 严格上三角作为成对相关系数样本
        triu = R_s[np.triu_indices(m, k=1)]
        sector_corr_stats.append({
            "sector": GICS_SECTORS[sid][0], "m": int(m),
            "intra_corr_min": float(triu.min()),
            "intra_corr_mean": float(triu.mean()),
            "intra_corr_max": float(triu.max())})

    return Theta, sector_corr_stats



def condition_number(M):
    """2-范数条件数 = max|min 奇异值。"""
    sv = np.linalg.svd(M, compute_uv=False)
    sv = sv[sv > 1e-15]
    return sv[0] / sv[-1] if len(sv) else float("inf")


def to_csc_arrays(Theta):
    """稠密(已裁剪)→ CSC 三元组:Ap(col_ptr), Ai(row_idx), Ax(vals)。
    Eigen SimplicialCholesky<SparseMatrix<double, ColMajor, int>> 的输入格式。
    """
    n = Theta.shape[0]
    cols = [[] for _ in range(n)]
    for j in range(n):
        for i in range(n):
            if Theta[i, j] != 0.0:
                cols[j].append((i, float(Theta[i, j])))
    Ap = [0]
    Ai = []
    Ax = []
    for j in range(n):
        for (i, v) in cols[j]:
            Ai.append(i)
            Ax.append(v)
        Ap.append(len(Ai))
    return np.array(Ap, dtype=np.int32), np.array(Ai, dtype=np.int32), np.array(Ax, dtype=np.float64)


def to_csr_arrays(Theta):
    """CSR 三元组:Ap(row_ptr), Ai(col_idx), Ax(vals)。"""
    n = Theta.shape[0]
    rows = [[] for _ in range(n)]
    for i in range(n):
        for j in range(n):
            if Theta[i, j] != 0.0:
                rows[i].append((j, float(Theta[i, j])))
    Ap = [0]
    Ai = []
    Ax = []
    for i in range(n):
        for (j, v) in rows[i]:
            Ai.append(j)
            Ax.append(v)
        Ap.append(len(Ai))
    return np.array(Ap, dtype=np.int32), np.array(Ai, dtype=np.int32), np.array(Ax, dtype=np.float64)


def adjacent_same_sector_ratio(sectors):
    """相邻索引对(i,i+1)属于同一板块的比例 —— fill-in 代理指标。

    配合下游 NaturalOrdering(不做重排序)时,消元顺序 = 索引顺序。若同板块
    因子索引连续(相邻对多为同板块),则同板块的强相关块在消元时集中处理,
    填充低;若随机散布,则弱相关的跨板块非零元会在消元中大量被"激活"成
    L 的新非零元,fill-in 爆炸(极端接近稠密)。故此比例越高,对
    NaturalOrdering 消元越友好,填充越低。理想:排序后大部分相邻对同板块,
    仅板块交界处跨板块。
    """
    if len(sectors) < 2:
        return 1.0
    same = int(np.sum(sectors[:-1] == sectors[1:]))
    return same / (len(sectors) - 1)


def sanity_report(n, Theta, Ap_csc, sectors, sparsity_thresh,
                   min_avg_nnz_warn, sector_seed, corr_seed,
                   sectors_pre_sort=None, sector_corr_stats=None):
    """合理性检查 + 健全性检查 + 【永久正定性门禁】。

    sanity(合理性):条件数(纯描述性,不做 pass/fail)、稀疏率、板块内相关
        系数实际分布、块对角验证,对照公开金融研究区间。
        【κ 不做判定】单因子模型(R_s=beta·betaᵀ+diag(1-beta²))本质是秩1+
        对角扰动结构,天然良态,κ 远低于稠密经验相关矩阵是数学必然,不是
        构造错误。参考区间 1e3~1e5 是给稠密经验矩阵设的,不适用,故删该判定。
        【板块结构】不用旧 sector_block_structure_preserved(基于 Σ 块内均值,
        单因子模型下 Σ 块内协方差因特殊项吸收而偏低,该判定不适用)。改用
        block_diagonal_verified(inter≈0)+ sector_correlation_stats(Θ/R_s 块内
        实际分布)这两项更准确的验证。
    health(健全性):每列非零元个数 —— 它直接决定 SDC 触发窗口长短
        (每列非零元越少,列更新循环暴露越短,越不易触发);若 avg 低于阈值,
        说明 sparsity_thresh 过严、削弱了触发窗口,应调低重新生成。
    fill-in 代理:adjacent_same_sector_ratio,排序前后都算(若传 sectors_pre_sort),
        验证"按板块聚合索引"确实降低了 NaturalOrdering 消元的填充预期。
    【永久正定性门禁】(2026-08-13 加):用 np.linalg.eigvalsh 算 min 特征值,
        检查 Θ 是否正定。任何构造方式改动导致的不正定都在此被捕获,不再等到
        下游 C++ Eigen 分解失败才发现。is_positive_definite=False 时,main() 会
        报错退出,不生成矩阵文件。此项为默认不可跳过的检查。
    """
    nnz = int(np.count_nonzero(Theta))
    total = n * n
    sparsity = 1.0 - nnz / total

    # ── 健全性:每列非零元个数(用 CSC 的 Ap 直接算,与输出格式同源)────
    # 第 j 列非零元数 = Ap_csc[j+1] - Ap_csc[j];Ap 长度 N+1。
    col_nnzs = np.diff(Ap_csc)            # 长度 N,每列非零元数
    avg_nnz_per_col = float(col_nnzs.mean())
    min_nnz_per_col = int(col_nnzs.min())
    max_nnz_per_col = int(col_nnzs.max())
    low_density_warning = avg_nnz_per_col < min_avg_nnz_warn

    # ── 合理性:条件数 ──
    # 数学性质:κ(A)=κ(A⁻¹) 对任意可逆矩阵恒成立(A 的奇异值 σ,A⁻¹ 的
    # 奇异值 1/σ,相除比值不变)。故 Θ 与 Σ=Θ⁻¹ 的条件数必然相等,只需算一次。
    # (原先误以为"κ(Θ) 因放大小特征值更高"——错误,已纠正。)
    cond = condition_number(Theta)

    # ── 【永久正定性门禁】(2026-08-13 加)─────────────────────────────
    # 用 eigvalsh(对称矩阵专用,比 eig 快且稳定)算 min 特征值,判正定。
    # 单因子模型从构造保证正定,但任何后续构造改动(裁剪/ridge/别的模型)
    # 若破坏正定,这里第一时间捕获,不让不正定矩阵流到下游 C++。
    min_eig = float(np.linalg.eigvalsh(Theta).min())
    is_pd = min_eig > 0

    # ── 合理性:板块结构保持度(Θ 反推 Σ=Θ⁻¹,看同板块 vs 跨板块相关)──
    Sigma_check = np.linalg.inv(Theta)
    Sigma_check = (Sigma_check + Sigma_check.T) / 2.0

    intra_corrs = []
    inter_corrs = []
    for i in range(n):
        for j in range(i + 1, n):
            if sectors[i] == sectors[j]:
                intra_corrs.append(Sigma_check[i, j])
            else:
                inter_corrs.append(Sigma_check[i, j])
    intra_mean = float(np.mean(intra_corrs)) if intra_corrs else 0.0
    inter_mean = float(np.mean(inter_corrs)) if inter_corrs else 0.0
    # 块对角构造验证信号:inter_mean 应非常接近 0(因 Θ 块对角 ⇒ Σ=Θ⁻¹
    # 也块对角 ⇒ 跨板块协方差精确为 0)。若 inter_mean 明显偏离 0,说明
    # 块对角构造有误(或 ridge/裁剪破坏了块结构)。这是对新构造方式的
    # 直接验证。
    block_diag_verified = abs(inter_mean) < 1e-10

    sector_counts = {}
    for s in sectors:
        name = GICS_SECTORS[s][0]
        sector_counts[name] = sector_counts.get(name, 0) + 1

    # ── fill-in 代理:相邻同板块比例(排序前后对比)────────────────────
    adj_post = adjacent_same_sector_ratio(sectors)
    if sectors_pre_sort is not None:
        adj_pre = adjacent_same_sector_ratio(sectors_pre_sort)
    else:
        adj_pre = None

    report = {
        "n": n,
        "sector_seed": sector_seed,
        "corr_seed": corr_seed,
        "factor_index_sorted_by_sector": True,   # 标注:因子索引已按板块排序
        "nnz": nnz,
        "sparsity": float(sparsity),
        "sparsity_threshold": float(sparsity_thresh),
        # 健全性:每列非零元 —— SDC 触发窗口直接相关指标
        "avg_nnz_per_col": avg_nnz_per_col,
        "min_nnz_per_col": min_nnz_per_col,
        "max_nnz_per_col": max_nnz_per_col,
        "low_density_warning": bool(low_density_warning),
        "min_avg_nnz_warn_threshold": float(min_avg_nnz_warn),
        # fill-in 代理:相邻同板块比例(配合 NaturalOrdering 消元)
        "adjacent_same_sector_ratio_pre_sort": float(adj_pre) if adj_pre is not None else None,
        "adjacent_same_sector_ratio_post_sort": float(adj_post),
        # 合理性:条件数
        # κ(Θ)=κ(Σ) 是矩阵求逆下的不变性质(σ 与 1/σ 比值不变),两者恒等,
        # 只算一次。保留两个字段名是为了兼容下游可能读取特定字段名的代码,
        # 实际值相同;新代码应优先读 condition_number 字段。
        "condition_number": float(cond),
        "cond_precision_theta": float(cond),   # = condition_number(兼容旧字段名)
        "cond_covariance_sigma": float(cond),  # = condition_number(兼容旧字段名)
        # κ 为纯描述性指标(不做 pass/fail 判定)。单因子模型
        # (R_s=beta·betaᵀ+diag(1-beta²))本质是秩1+对角扰动结构,天然良态,
        # κ 远低于稠密经验相关矩阵是数学必然,不是构造错误。参考区间
        # 1e3~1e5 是给稠密经验矩阵设的,不适用于单因子模型,故删该判定。
        # 详见字段内注释。
        "condition_number_note": (
            "descriptive only (no pass/fail). Single-factor model "
            "(R_s=beta*beta^T+diag(1-beta^2)) is rank-1 + diagonal perturbation, "
            "inherently well-conditioned; kappa far below dense empirical "
            "correlation matrices (1e3~1e5, Lead-Dead 2018) is mathematical "
            "necessity, not a construction error. This is actually favorable "
            "for SDC attribution: well-conditioned matrix rules out numerical "
            "ill-conditioning as the SDC cause."
        ),
        # Σ=Θ⁻¹ 块内/跨板块均值(纯数值;单因子模型下 Σ 块内协方差低,
        # 因大部分方差被特殊项 1-beta² 吸收,故 intra_mean 偏小是正常的)
        "intra_sector_mean_corr": intra_mean,
        "inter_sector_mean_corr": inter_mean,
        # 块对角构造验证:inter_mean 应≈0(Σ=Θ⁻¹ 块对角 ⇒ 跨板块协方差精确0)
        "block_diagonal_verified": bool(block_diag_verified),
        # 【永久正定性门禁】
        "min_eigenvalue": min_eig,
        "is_positive_definite": bool(is_pd),
        # 每板块实际相关系数分布(单因子模型:corr=beta_i*beta_j,分布比独立
        # 采样更集中于中段;对照 intra_corr_range 目标区间看是否偏)
        "sector_correlation_stats": sector_corr_stats or None,
        "sector_counts": sector_counts,
        # ── 对照公开金融研究(论文引用区)──────────────────────
        # κ(Θ)=κ(Σ) 恒等(求逆不变性质),单因子模型天然良态故不做区间判定。
        "reference_sparsity_theta": "80%~95% (sparse graphical models in finance)",
    }
    return report


def write_csc(path, Ap, Ai, Ax, n):
    """写 Eigen 可直接读的 CSC 文件。

    格式(与 mru_eigenmc.c 的 escanalyze/escfactorize 接口一致):
      line 1: N nnz              (header)
      line 2: Ap[0..N]            (col pointers, space-separated)
      line 3: Ai[0..nnz-1]        (row indices, space-separated)
      line 4: Ax[0..nnz-1]        (values, space-separated)
    """
    with open(path, "w") as f:
        f.write(f"{n} {len(Ax)}\n")
        f.write(" ".join(str(int(x)) for x in Ap) + "\n")
        f.write(" ".join(str(int(x)) for x in Ai) + "\n")
        f.write(" ".join(f"{x:.17g}" for x in Ax) + "\n")


def write_csr(path, Ap, Ai, Ax, n):
    """写 CSR 格式(同上布局,Ap 是 row pointers)。"""
    with open(path, "w") as f:
        f.write(f"{n} {len(Ax)}\n")
        f.write(" ".join(str(int(x)) for x in Ap) + "\n")
        f.write(" ".join(str(int(x)) for x in Ai) + "\n")
        f.write(" ".join(f"{x:.17g}" for x in Ax) + "\n")


def generate(n, sector_seed, corr_seed, intra, inter,
              sparsity_thresh, ridge, min_avg_nnz_warn, out_dir):
    """完整生成流程,返回 (base, Theta, report)。供 main 与 selftest 复用。"""
    sectors_raw = assign_sectors(n, sector_seed)
    # 按板块聚合索引:同板块因子在索引上连续排列。配合下游 NaturalOrdering
    # (不做重排序)消元时,同板块强相关块集中处理,fill-in 显著降低;
    # 否则随机散布会让 L 在消元中激增非零元(极端接近稠密)。
    # 排序确定性:对确定性数组排序仍确定性(相同 sector_seed → 相同排序结果)。
    sectors = np.sort(sectors_raw)
    Theta, sector_corr_stats = build_precision_matrix(
        n, sectors, intra, inter, sparsity_thresh, ridge, corr_seed)

    Ap_csc, Ai_csc, Ax_csc = to_csc_arrays(Theta)
    Ap_csr, Ai_csr, Ax_csr = to_csr_arrays(Theta)

    report = sanity_report(
        n, Theta, Ap_csc, sectors, sparsity_thresh,
        min_avg_nnz_warn, sector_seed, corr_seed,
        sectors_pre_sort=sectors_raw,   # 算排序前后相邻同板块比例对比
        sector_corr_stats=sector_corr_stats)   # 每板块实际相关分布

    base = f"var_cov_N{n}_sector{sector_seed}_corr{corr_seed}_sorted"
    os.makedirs(out_dir, exist_ok=True)
    csc_path = os.path.join(out_dir, base + ".csc")
    csr_path = os.path.join(out_dir, base + ".csr")
    meta_path = os.path.join(out_dir, base + ".meta.json")
    write_csc(csc_path, Ap_csc, Ai_csc, Ax_csc, n)
    write_csr(csr_path, Ap_csr, Ai_csr, Ax_csr, n)
    report["files"] = {"csc": csc_path, "csr": csr_path, "meta": meta_path}
    with open(meta_path, "w") as f:
        json.dump(report, f, indent=2, ensure_ascii=False)

    return base, Theta, report


def run_selftest():
    """确定性自检 + 排序聚合验证。

    1. 确定性:同 (sector_seed=42, corr_seed=43) 跑两次,Θ 逐元素完全一致
       (验证复现性 bug 已修 + np.sort 排序仍确定性)。
    2. 排序聚合:排序后 adjacent_same_sector_ratio 应显著高于排序前
       (验证按板块聚合索引确实降低了 NaturalOrdering 消元的填充预期)。
    3. corr_seed 真起作用:换 corr_seed → Θ 变化。
    """
    print("=== 确定性自检 (sector_seed=42, corr_seed=43, 跑两次) ===")
    import tempfile
    tmp = tempfile.mkdtemp(prefix="var_cov_selftest_")
    _, Theta1, rep1 = generate(256, 42, 43, [0.4, 0.8], [0.0, 0.15],
                                0.02, 1e-6, 10, tmp)
    _, Theta2, rep2 = generate(256, 42, 43, [0.4, 0.8], [0.0, 0.15],
                                0.02, 1e-6, 10, tmp)
    if np.array_equal(Theta1, Theta2):
        print("[PASS] 两次生成的 Θ 逐元素完全一致 → 确定性 OK(含 np.sort)")
    else:
        diff = np.count_nonzero(Theta1 != Theta2)
        print(f"[FAIL] 两次生成的 Θ 有 {diff} 个元素不一致 → 复现性 bug 未修!")
        raise SystemExit(1)

    # 排序聚合验证:排序前后 adjacent_same_sector_ratio 对比
    pre = rep1["adjacent_same_sector_ratio_pre_sort"]
    post = rep1["adjacent_same_sector_ratio_post_sort"]
    print(f"\n=== 排序聚合验证 (adjacent_same_sector_ratio) ===")
    print(f"    排序前 = {pre:.4f}  (随机散布,接近随机水平)")
    print(f"    排序后 = {post:.4f}  (同板块聚合,应显著提高)")
    if post > pre + 0.3:
        print(f"[PASS] 排序后相邻同板块比例显著提高({pre:.4f}→{post:.4f})→ fill-in 预期降低")
    else:
        print(f"[WARN] 排序后比例提升不足({pre:.4f}→{post:.4f}),聚合效果可疑")

    # corr_seed 真起作用(换 corr_seed,sector_seed 不变 → Θ 应变)
    _, Theta3, _ = generate(256, 42, 999, [0.4, 0.8], [0.0, 0.15],
                             0.02, 1e-6, 10, tmp)   # 换 corr_seed
    if np.array_equal(Theta1, Theta3):
        print("\n[WARN] 改 corr_seed 后 Θ 没变 —— corr_seed 未起作用?")
    else:
        print("[PASS] 改 corr_seed → Θ 变化(相关系数种子真起作用)")

    # 注:不测"改 sector_seed → Θ 变",因为 np.sort 后,sector_seed 仅通过
    # "各板块因子计数"影响 Θ;若两个 sector_seed 恰好产生相同板块计数,
    # 排序后 sectors 数组完全相同 → Θ 也相同。这是 np.sort 的设计结果
    # (抹掉索引位置,只保留板块结构),不是 bug。确定性由上面"同 seed 两次
    # 一致"的断言保证。


def main():
    ap = argparse.ArgumentParser(description="VaR 协方差/精度矩阵生成器(Eigen Cholesky 输入)")
    ap.add_argument("--n", type=int, default=512, choices=range(256, 1025),
                    help="风险因子数(矩阵维度),256~1024,默认 512")
    # 两个互相独立的种子 —— 不再派生,可单独控制做对照实验
    ap.add_argument("--sector-seed", type=int, default=42,
                    help="因子→11 GICS 板块分配的种子,默认 42")
    ap.add_argument("--corr-seed", type=int, default=43,
                    help="板块内/跨板块相关系数取值的种子,默认 43")
    ap.add_argument("--out-dir", type=str, default="/tmp",
                    help="输出目录,默认 /tmp")
    ap.add_argument("--intra", type=float, nargs=2, default=[0.4, 0.8],
                    help="板块内相关系数区间 [min max],默认 0.4 0.8")
    ap.add_argument("--inter", type=float, nargs=2, default=[0.0, 0.15],
                    help="跨板块相关系数区间 [min max],默认 0.0 0.15")
    ap.add_argument("--sparsity-thresh", type=float, default=0.02,
                    help="精度矩阵稀疏阈值,|Θ[i,j]|<此值且非对角置 0,默认 0.02")
    ap.add_argument("--ridge", type=float, default=1e-10,
                    help="浮点安全垫(单因子模型从构造保证正定,此值非正定修复,仅防浮点噪声),默认 1e-10")
    ap.add_argument("--min-avg-nnz-warn", type=float, default=10,
                    help="健全性告警阈值:avg 每列非零元 < 此值则警告削弱触发窗口,默认 10")
    ap.add_argument("--print-sanity", action="store_true",
                    help="打印合理性报告到 stdout")
    ap.add_argument("--selftest", action="store_true",
                    help="跑确定性自检(跑完即退出,不生成文件)")
    args = ap.parse_args()

    if args.selftest:
        run_selftest()
        return

    n = args.n
    print(f"[*] 构造 VaR 协方差矩阵(块对角精度矩阵): N={n}, sector_seed={args.sector_seed}, corr_seed={args.corr_seed}")
    print(f"    板块内相关: [{args.intra[0]}, {args.intra[1]}]")
    print(f"    跨板块相关: [{args.inter[0]}, {args.inter[1]}]  (块对角构造下不采样,跨板块协方差设计为0)")
    print(f"    板块内稀疏阈值: {args.sparsity_thresh} (仅板块内可选裁剪,默认轻度), ridge: {args.ridge}")

    sectors_raw = assign_sectors(n, args.sector_seed)
    sectors = np.sort(sectors_raw)   # 按板块聚合索引(与 generate() 内部一致)
    sector_counts = {}
    for s in sectors:
        name = GICS_SECTORS[s][0]
        sector_counts[name] = sector_counts.get(name, 0) + 1
    print(f"    板块分配(已按板块排序聚合索引): {sector_counts}")

    base, Theta, report = generate(
        n, args.sector_seed, args.corr_seed,
        args.intra, args.inter, args.sparsity_thresh, args.ridge,
        args.min_avg_nnz_warn, args.out_dir)

    # 【永久正定性硬门禁】 Θ 不正定直接退出,不写矩阵文件(不能让不正定矩阵
    # 流到下游 C++ Eigen 分解才被发现)。此项不可跳过,无需命令行开关。
    if not report["is_positive_definite"]:
        print("\n" + "!" * 72)
        print(f"! [致命] Θ 不正定!min_eigenvalue = {report['min_eigenvalue']:.6e} ≤ 0")
        print(f"!   下游 Eigen SimplicialLLT 分解必然失败。不生成矩阵文件,直接退出。")
        print(f"!   检查 build_precision_matrix 构造方式(单因子模型应保证正定,")
        print(f"!   若裁剪/ridge/模型改动破坏正定,这里第一时间捕获)。")
        print("!" * 72)
        sys.exit(1)

    if args.print_sanity:
        print("\n=== 合理性 + 健全性报告 ===")
        print(json.dumps(report, indent=2, ensure_ascii=False))

    print(f"\n[+] 输出:")
    print(f"    CSC: {report['files']['csc']}  (Eigen ColMajor Lower 输入,因子索引已按板块排序)")
    print(f"    CSR: {report['files']['csr']}")
    print(f"    META: {report['files']['meta']}")
    print(f"\n[+] 摘要:")
    print(f"    nnz={report['nnz']}  稀疏率={report['sparsity']*100:.1f}%  "
          f"κ={report['condition_number']:.2e}  (描述性,单因子模型天然良态,不做判定)")
    print(f"    正定性: min_eig={report['min_eigenvalue']:.3e}  "
          f"is_positive_definite={'✅ 是' if report['is_positive_definite'] else '❌ 否'}  "
          f"块对角验证={'✅' if report['block_diagonal_verified'] else '❌'}(inter={report['inter_sector_mean_corr']:.3e}应≈0)")
    print(f"    每列非零元: avg={report['avg_nnz_per_col']:.1f}  "
          f"min={report['min_nnz_per_col']}  max={report['max_nnz_per_col']}")
    # 各板块实际相关系数分布(单因子模型 corr=beta_i*beta_j,对照目标区间)
    if report.get("sector_correlation_stats"):
        print(f"    各板块实际相关分布(对照目标 [{args.intra[0]}, {args.intra[1]}]):")
        for st in report["sector_correlation_stats"]:
            if st["intra_corr_min"] is None:
                print(f"      {st['sector']:<24} m={st['m']:<4} (单因子,无成对相关)")
            else:
                print(f"      {st['sector']:<24} m={st['m']:<4} "
                      f"min={st['intra_corr_min']:.3f} mean={st['intra_corr_mean']:.3f} max={st['intra_corr_max']:.3f}")
    # fill-in 代理指标:排序前后相邻同板块比例对比
    pre = report["adjacent_same_sector_ratio_pre_sort"]
    post = report["adjacent_same_sector_ratio_post_sort"]
    print(f"    相邻同板块比例: 排序前={pre:.4f} → 排序后={post:.4f}  "
          f"(配合NaturalOrdering消元的fill-in代理;越高填充越低)")

    # 健全性告警(醒目)
    if report["low_density_warning"]:
        print()
        print("!" * 72)
        print(f"! [健全性告警] avg 每列非零元 = {report['avg_nnz_per_col']:.1f} < "
              f"{args.min_avg_nnz_warn}")
        print(f"!   当前稀疏阈值 --sparsity-thresh={args.sparsity_thresh} 可能过严,")
        print(f"!   削弱了 SDC 触发窗口(每列非零元越少 → 列更新循环暴露越短 → 越不易触发)。")
        print(f"!   建议:调低 --sparsity-thresh 重新生成,使 avg 每列非零元 ≥ {args.min_avg_nnz_warn}。")
        print("!" * 72)

    print(f"\n    参考: κ(Θ)=κ(Σ) 求逆不变(单因子模型天然良态,不做区间判定); "
          f"稀疏率 80%~95%")


if __name__ == "__main__":
    main()

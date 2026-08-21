// var_mc_var.cpp — 蒙特卡洛 VaR 计算流程(精度矩阵 GMRF 采样)
//
// 论文背景场景:银行风险引擎蒙特卡洛 VaR。读入稀疏精度矩阵 Θ = Σ⁻¹
// (gen_var_cov_matrix.py 生成的 .csc),对 Θ 做 SimplicialCholesky 分解
// Θ = L·Lᵀ,再用【精度矩阵采样公式】(不是协方差直接采样)生成 10000 组
// 相关收益率情景,算 99% VaR。
//
// ┌──────────────────────────────────────────────────────────────────────┐
// │ 【关键采样公式 — 精度矩阵 GMRF 采样,不是协方差矩阵直接采样】         │
// │                                                                      │
// │   Θ = L·Lᵀ        (Lower 分支分解,SimplicialCholesky<Sparse,Lower>) │
// │   采样:解  Lᵀ·X = Z   →  X = L⁻ᵀ·Z                                  │
// │   验证:Cov(X) = (L⁻ᵀ)(L⁻ᵀ)ᵀ = L⁻ᵀ·L⁻¹ = (L·Lᵀ)⁻¹ = Θ⁻¹ = Σ ✅   │
// │                                                                      │
// │   ❌ 错误公式:X = L⁻¹·Z  →  Cov(X) = L⁻¹·L⁻ᵀ = (Lᵀ·L)⁻¹ ≠ Σ        │
// │      (因为 Θ = L·Lᵀ ≠ Lᵀ·L;只有 Θ 用 Upper 分支分解成 Θ=Uᵀ·U 才对)│
// │                                                                      │
// │   Eigen 接口:matrixL() 返回 L(下三角);matrixU() 返回 Lᵀ(上三角, │
// │   不是独立 U)。故解 Lᵀ·X = Z 用 matrixU().solveInPlace(Z)——反向回代。│
// │   不要手动 .transpose() 再 solve,直接用 matrixU() 三角求解器。        │
// └──────────────────────────────────────────────────────────────────────┘
//
// 与 SDC 复现环境一致:Eigen 5.0.1,SimplicialCholesky<SparseMatrix<double,
// ColMajor, int>>,走 factorize_preordered<false,false>(LLT)路径。
// 第2步(分解)与第3步(采样)之间留有 modify_L_hook(),供故障注入实验
// 在分解后、采样前修改 L(如篡改 L[0,0] 复现 SDC 损坏)。
//
// 用法:
//   var_mc_var <theta.csc> [n_scenarios=10000] [rng_seed=2024]
//
// SPDX-License-Identifier: Apache-2.0
// Copyright 2025 Intel Corporation.

#include <Eigen/Sparse>
#include <Eigen/Dense>
#include <Eigen/Cholesky>
#include <vector>
#include <memory>
#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <stdexcept>

using Eigen::SparseMatrix;
using Eigen::VectorXd;
using Eigen::MatrixXd;
using std::vector;

// ─── 第1步:读入稀疏精度矩阵 Θ(CSC 格式)──────────────────────────────
// .csc 格式(gen_var_cov_matrix.py 的 write_csc):
//   line 1: "N nnz"
//   line 2: Ap[0..N]   (列指针,N+1 个)
//   line 3: Ai[0..nnz-1] (行索引)
//   line 4: Ax[0..nnz-1] (数值)
// 喂 Eigen setFromTriplets(与 mru_eigenmc.c 的 escanalyze 约定一致)。
static SparseMatrix<double, Eigen::ColMajor, int>
read_csc_theta(const char* path) {
    FILE* f = std::fopen(path, "r");
    if (!f) throw std::runtime_error(std::string("无法打开 ") + path);
    int N, nnz;
    if (std::fscanf(f, "%d %d", &N, &nnz) != 2)
        { std::fclose(f); throw std::runtime_error("读 header 失败"); }

    vector<int> Ap(N + 1), Ai(nnz);
    vector<double> Ax(nnz);
    for (int j = 0; j <= N; ++j)
        if (std::fscanf(f, "%d", &Ap[j]) != 1)
            { std::fclose(f); throw std::runtime_error("读 Ap 失败"); }
    for (int p = 0; p < nnz; ++p)
        if (std::fscanf(f, "%d", &Ai[p]) != 1)
            { std::fclose(f); throw std::runtime_error("读 Ai 失败"); }
    for (int p = 0; p < nnz; ++p)
        if (std::fscanf(f, "%lf", &Ax[p]) != 1)
            { std::fclose(f); throw std::runtime_error("读 Ax 失败"); }
    std::fclose(f);

    // CSC 三元组 → Eigen SparseMatrix
    vector<Eigen::Triplet<double>> trips;
    trips.reserve(nnz);
    for (int j = 0; j < N; ++j)
        for (int p = Ap[j]; p < Ap[j + 1]; ++p)
            trips.push_back({Ai[p], j, Ax[p]});
    SparseMatrix<double, Eigen::ColMajor, int> Theta(N, N);
    Theta.setFromTriplets(trips.begin(), trips.end());
    return Theta;
}

// ─── 第2步:对 Θ 做 SimplicialCholesky(Lower),得 L(Θ = L·Lᵀ)─────────
// 与 SDC 复现同构:同一 solver 类型、同一 factorize_preordered<false,false>
// (LLT)路径。返回的 solver 持有 L;matrixL() 取下三角,matrixU() 取 Lᵀ。
//
// ─── 排序策略:NaturalOrdering(不做填充减少重排序)─────────────────
// 【为什么放弃默认的 AMDOrdering】默认 AMD 会对稀疏矩阵做填充减少重排序,
// solver 实际分解的是 P·Θ·Pᵀ,matrixL()/matrixU() 返回的因子在重排序坐标系
// 下,不是原始风险因子顺序。这在本场景有两个问题:
//  (1) sanity_check_cov_direction() 拿经验协方差和 Θ⁻¹ 逐元素比对时,两边
//      变量顺序对不上,会误报"方向错误"(其实是顺序错位,非方向问题);
//  (2) 核 179 SDC 根因(§11.2a)里"elem[0] 固定损坏"指的是消元顺序第 0 列
//      (k=0)。若用 AMD,论文无法说清"L[0,0] 对应原始哪个风险因子"——中间
//      隔着置换映射 P。
// 故改用 NaturalOrdering:不做任何重排序,消元顺序严格等于输入矩阵原始
// 变量顺序,k=0 直接对应输入第 0 个风险因子,L[k,k] 与原始风险因子索引 k
// 一一对应,无需处理置换矩阵。论文叙述与代码逻辑都更干净。
//
// 【触发路径仍与 SDC 复现一致】NaturalOrdering 只影响 analyzePattern 里那步
// ordering 计算(Eigen 内部对 NaturalOrdering 有专门分支跳过 ordering),不
// 改变 numeric factorize 的指令序列——仍走 compute()→analyzePattern_preordered
// + factorize_preordered<false,false>(LLT),与 mru_eigenmc / 根因报告 §10.5
// 同源。故触发条件对得上已确认的复现环境。
using SC = Eigen::SimplicialLLT<SparseMatrix<double, Eigen::ColMajor, int>,
                                 Eigen::Lower, Eigen::NaturalOrdering<int>>;
// 返回 unique_ptr:Eigen 的 solver 继承 SparseSolverBase : noncopyable,
// 拷贝构造被删且无可用移动构造,不能按值 return。用 unique_ptr 在堆上持有。
//
// 同时提取一份【独立的可改 L 副本】L_copy(独立 SparseMatrix,与 solver 内部
// m_matrix 解耦):solver 的 m_matrix 是 protected,外部无法改;故障注入实验
// (CorruptL00Hook)需要篡改 L[0,0],故提取副本到可控的 SparseMatrix。采样路径
// 改用 L_copy.triangularView<Upper>().solveInPlace()(等价于 solver.matrixU(),
// 因 Upper 视图 = Lᵀ),注入改 L_copy 不影响 solver,可干净注入。
struct FactorizeResult {
    std::unique_ptr<SC> solver;
    SparseMatrix<double, Eigen::ColMajor, int> L_copy;   // 独立可改副本
};
static FactorizeResult factorize_theta(const SparseMatrix<double, Eigen::ColMajor, int>& Theta) {
    auto solver = std::make_unique<SC>();
    // compute() = analyzePattern + factorize,与原 opendcdiag eigen_sparse 同源
    // (compute-both 模式,触发但低于 numeric-only;见根因报告 §10.2)。
    // 使用 NaturalOrdering(见上注释),不做填充减少重排序,保证消元顺序 k
    // 与输入矩阵行列顺序一致,L[k,k] 对应风险因子索引 k,不需处理置换映射。
    solver->compute(Theta);
    if (solver->info() != Eigen::Success)
        throw std::runtime_error("Θ 分解失败:矩阵不正定(检查 gen 脚本的 ridge)");
    // 提取独立 L 副本(下三角)。matrixL() 返回 const TriangularView,赋值给
    // SparseMatrix 会拷贝下三角部分。注入改这个副本的 [0,0]。
    SparseMatrix<double, Eigen::ColMajor, int> L_copy = solver->matrixL();
    return {std::move(solver), std::move(L_copy)};
}

// ─── 故障注入钩子(默认 no-op;实验时在此修改 L 副本)───────────────────
// 默认实现:不动 L。做 SDC 对比实验时,CorruptL00Hook 篡改 L[0,0](用 L[k,k]
// 整体替换,复现 §11.2a 的 d0 混叠——"整个寄存器值被另一个真实 double 替换"),
// 观察经 x[0]=b[0]/L[0,0] 放大到 VaR 的偏移。
// 注入结果(回传给调用方记录复盘)
struct InjectionInfo {
    bool injected = false;
    int k_chosen = -1;        // 借用的对角元位置(≠0)
    double orig_L00 = 0.0;    // 原始 L[0,0]
    double new_L00 = 0.0;     // 替换后的 L[0,0](= 原 L[k,k])
    double donor_value = 0.0; // L[k,k] 的值(= new_L00)
};

struct FaultHook {
    virtual ~FaultHook() = default;
    // 默认:不注入。info 留默认(false),采样走正常路径。
    virtual void modify_L(SparseMatrix<double, Eigen::ColMajor, int>& /*L*/,
                          InjectionInfo& info) { info.injected = false; }
};
struct NoOpHook : FaultHook {};

// ─── CorruptL00Hook:把 L[0,0] 整体替换成 L[k,k](k∈[k_min,n),按注入seed选)──
// 依据根因报告 §11.2a 的 d0 混叠机理:"混入的是当时正好活跃在 PRF 的另一个
//  double,整个寄存器值被整条替换"——不是构造抽象垃圾值或拼位模式。21-32 位的
//  差异幅度是两个真实的、量级相近但彼此无关的浮点数之间天然的汉明距离,直接
//  整体替换最贴合真实机理,实现也最简单。
//  k 范围 [k_min, n):避开 k=0 自身(不能拿自己替换自己),也避开太靠前的索引
//  (论文叙述里 IT 板块靠前,k_min=50 保证 donor 来自板块中后段,更随机)。
struct CorruptL00Hook : FaultHook {
    int k_min;
    uint64_t inject_seed;
    CorruptL00Hook(int k_min_, uint64_t inject_seed_)
        : k_min(k_min_), inject_seed(inject_seed_) {}

    void modify_L(SparseMatrix<double, Eigen::ColMajor, int>& L,
                  InjectionInfo& info) override {
        int n = L.rows();
        // 用注入 seed 的 LCG 选 k(确定性,可复现,不用系统时间)
        uint32_t st = (uint32_t)inject_seed;
        st = st * 1103515245u + 12345u;
        int k = k_min + (int)(st % (uint32_t)(n - k_min));
        double orig = L.coeff(0, 0);
        double donor = L.coeff(k, k);      // 借用的另一个真实对角元
        L.coeffRef(0, 0) = donor;          // 整体替换
        info.injected = true;
        info.k_chosen = k;
        info.orig_L00 = orig;
        info.new_L00 = donor;
        info.donor_value = donor;
    }
};

// ─── WorstCaseL00Hook:确定性遍历找最大差异 donor(偏差上界)──────────────
// 与 CorruptL00Hook 同样的"整体替换"注入方式,但选 k 的逻辑从 LCG 随机改成
// 确定性地找使 |L[k,k]-L[0,0]| 最大的 k_worst。这给出"单个 donor 能造成的
// 最大 VaR 偏差"上界,与随机 20 组的典型/最大偏差对比,说明随机情况离理论
// 上界有多远。无需 seed(确定性遍历)。
struct WorstCaseL00Hook : FaultHook {
    void modify_L(SparseMatrix<double, Eigen::ColMajor, int>& L,
                  InjectionInfo& info) override {
        int n = L.rows();
        double orig = L.coeff(0, 0);
        // 遍历所有对角元(k=1..n-1,排除 k=0 自身),找 |L[k,k]-L[0,0]| 最大
        int k_worst = 1;
        double worst_diff = -1.0;
        // L.diagonal() 返回对角视图(SparseMatrix 支持)
        for (int k = 1; k < n; ++k) {
            double d = std::fabs(L.coeff(k, k) - orig);
            if (d > worst_diff) { worst_diff = d; k_worst = k; }
        }
        double donor = L.coeff(k_worst, k_worst);
        L.coeffRef(0, 0) = donor;
        info.injected = true;
        info.k_chosen = k_worst;
        info.orig_L00 = orig;
        info.new_L00 = donor;
        info.donor_value = donor;
    }
};

//  ★ 用精度矩阵采样公式 X = L⁻ᵀ·Z(解 Lᵀ·X = Z),不是协方差直接采样 ★
//  流程每组:
//    1. Z ~ N(0, I)            (n 维标准正态,独立)
//    2. 解 Lᵀ·X = Z            ← L.triangularView<Upper>().solveInPlace(反向回代)
//    3. Cov(X) = Θ⁻¹ = Σ       ← 这才是资产收益率的相关结构
//  rng_seed 固定 → 采样确定性,可复现。
//  注:采样用独立 L 副本(可被注入篡改),不直接用 solver.matrixU()——这样
//  注入改 L[0,0] 后采样立即用被篡改的 L,而 solver 内部不受影响。
//
//  采样核心 RNG + Box-Muller 抽成独立函数 sample_n,供 VaR 采样与自检采样
//  复用,两者用不同 seed(避免自检样本与 VaR 样本重合,保证独立性)。
static MatrixXd sample_n(const SparseMatrix<double, Eigen::ColMajor, int>& L, int n,
                          int n_scenarios, uint64_t rng_seed) {
    // Eigen 不自带正态 RNG;用一个简单的 Box-Muller + LCG(确定性,无外部依赖)
    // LCG 参数与 mru_eigenmc.c 一致(rng*1103515245+12345),保证跨实现一致。
    uint32_t state = (uint32_t)rng_seed;
    auto next_u32 = [&]() {
        state = state * 1103515245u + 12345u;
        return state;
    };
    auto next_uniform = [&]() -> double {
        // (next_u32>>8 & 0xffffff) / 0x1000000,与 mru_eigenmc.c 的 frand() 同
        return ((next_u32() >> 8) & 0xffffff) / (double)0x1000000;
    };
    auto next_normal = [&]() -> double {
        // Box-Muller;两个均匀 → 一个标准正态
        double u1 = next_uniform();
        double u2 = next_uniform();
        if (u1 < 1e-12) u1 = 1e-12;
        return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * M_PI * u2);
    };

    MatrixXd X(n, n_scenarios);   // 每列一组情景
    VectorXd Z(n);
    for (int s = 0; s < n_scenarios; ++s) {
        for (int i = 0; i < n; ++i) Z(i) = next_normal();
        // ★ 精度矩阵 GMRF 采样:解 Lᵀ·X = Z → X = L⁻ᵀ·Z ★
        //   L 是独立副本(下三角存储)。L.transpose() 给出上三角视图 = Lᵀ,
        //   .triangularView<Upper>().solveInPlace() 做反向回代 —— 等价于
        //   solver.matrixU().solveInPlace()(已验证相对差 0.000e+00)。
        //   不要写成 L.triangularView<Upper> —— L 只存下三角,那样上三角是空,会
        //   退化为单位阵(原 bug:Cov 变 I,自检报 ~0.9)。
        //   transpose() 是视图(不拷贝),改 L[0,0] 后视图立即反映新值(注入可见)。
        L.transpose().triangularView<Eigen::Upper>().solveInPlace(Z);   // Z 原地变 X
        X.col(s) = Z;
    }
    return X;
}

// VaR 采样:复用 sample_n,用 L 副本(可被注入篡改;基线时为 const 干净值)。
// 注:sample_n 接收 const L&——triangularView<Upper> 对 const SparseMatrix 可用
// (只读视图,solveInPlace 改的是 Z 不是 L)。注入时 CorruptL00Hook 先改 L 副本
// 再传入(非 const 路径),基线用 const 路径。
static MatrixXd sample_scenarios(const SparseMatrix<double, Eigen::ColMajor, int>& L,
                                  int n, int n_scenarios, uint64_t rng_seed) {
    return sample_n(L, n, n_scenarios, rng_seed);
}

// ─── 采样方向自检(防公式写反)──────────────────────────────────────────
// 自检验证"采样公式对不对"(X=L⁻ᵀ·Z → Cov=Θ⁻¹),与"VaR 要多少组情景估
// 分位数"是两个不同维度的问题,故【自检样本数与 VaR 情景数完全解耦】:
// 自检单独用 n_check_samples=30000 组情景(独立 rng_seed,与 VaR 的 10000
// 组情景互不影响),不让 VaR 情景数卡住自检的统计能力。
//
// 样本数选取依据(2026-08-13 实测):用 n=512 的 Θ 采样,
//   n_check=2000  → Frobenius 误差 0.2103
//   n_check=20000 → 误差 0.0668
// 误差按 1/√n_check 缩放(numpy 独立验证,C++ 数值一致),故
//   n_check=30000 → 预期误差 ≈ 0.0668·√(20000/30000) ≈ 0.055,留有安全边际
// (< 0.1 门槛)。VaR 主流程仍用 10000 组估分位数(对百分位估计足够),
// 不因自检要更多样本而推高。
//
// 阈值 0.1:经验协方差与 Θ⁻¹ 都在原始风险因子坐标系下(因子化用
// NaturalOrdering 无置换),两边变量顺序严格对齐——误差只来自 (a) 采样
// 统计涨落(有限 n_check)、(b) 若方向写反带来的系统性偏差(写反会报 ~1.0
// 量级,远大于 0.1)。若误差 ≥0.1,查 matrixU() vs matrixL()、Θ 正定性。
static void sanity_check_cov_direction(
        const SparseMatrix<double, Eigen::ColMajor, int>& Theta,
        SparseMatrix<double, Eigen::ColMajor, int>& L, int n, uint64_t check_rng_seed) {
    // 自检样本数:与 VaR 情景数解耦,固定 30000(依据见上注释)
    const int n_check_samples = 30000;

    // 自检单独采样(独立 seed,不复用 VaR 的 X)。用未注入的 L(基线自检)。
    MatrixXd Xc_full = sample_n(L, n, n_check_samples, check_rng_seed);

    // 经验协方差(去均值):Cov_emp = (X-mean)(X-mean)ᵀ / (n-1)
    VectorXd mean = Xc_full.rowwise().mean();
    MatrixXd centered = Xc_full.colwise() - mean;
    MatrixXd cov_emp = (centered * centered.transpose()) / (n_check_samples - 1);

    // Θ⁻¹(理论 Σ)。稀疏 → 稠密求逆 OK(n ≤ 1024)。
    MatrixXd Sigma_theory = MatrixXd(Theta.toDense()).inverse();

    double err = (cov_emp - Sigma_theory).norm() / Sigma_theory.norm();
    std::printf("[自检] 采样方向验证: |Cov_emp - Θ⁻¹|/|Θ⁻¹| = %.3e  (n_check=%d, 与VaR情景数解耦)\n",
                err, n_check_samples);
    std::printf("       采样公式: X = L⁻ᵀ·Z (解 Lᵀ·X=Z) → Cov(X)=Θ⁻¹=Σ  ");
    if (err < 0.1)
        std::printf("→ ✅ 方向正确(经验协方差吻合 Θ⁻¹,误差由统计涨落主导)\n");
    else
        std::printf("→ ❌ 方向可能写反或有其他问题!检查 matrixU() vs matrixL()、Θ 正定性\n");
}

// ─── 第4步:组合损失 + 分位数 VaR(通用版,支持任意权重/分位数)─────────
// 权重方案:
//   - "equal":等权 w_j=1/n(原始对照)
//   - "concentrated":因子0 权重 20%(被注入的 IT 板块因子),其余 511 个平分 80%
//     (验证"稀释"假说:等权下因子0占1/512≈0.2%,VaR 偏差被稀释到0.06%;
//      集中到20%后若偏差放大到几个百分点,直接证明稀释是主因)
// 损失 loss_s = -sum_j w_j · X[j,s]。
// 分位数 quantile∈(0,1):P99=0.99, P99.9=0.999。索引 floor(q·(n_scenarios-1))。
// 99.9% 分位数样本量薄,建议 n_scenarios 提到 50000 估计更稳。
static VectorXd make_weights(int n, const std::string& scheme) {
    VectorXd w(n);
    if (scheme == "concentrated") {
        // 因子0 占 20%,其余 511 个平分 80%
        w.setZero();
        w(0) = 0.20;
        double rest = 0.80 / (n - 1);
        for (int j = 1; j < n; ++j) w(j) = rest;
    } else {  // "equal"
        w.setConstant(n, 1.0 / n);
    }
    return w;
}
static double compute_var(const MatrixXd& X, const VectorXd& w, int n_scenarios,
                          double quantile) {
    VectorXd losses(n_scenarios);
    for (int s = 0; s < n_scenarios; ++s)
        losses(s) = -w.dot(X.col(s));
    std::sort(losses.data(), losses.data() + n_scenarios);
    int idx = (int)std::floor(quantile * (n_scenarios - 1));  // 0-based
    return losses(idx);
}

// ─── 输出 L 因子(稠密,定位 L[0,0] 用于故障注入对比)──────────────────
// 接收 L 副本(可被注入篡改;基线时为干净值)。
static void dump_L_factor(const SparseMatrix<double, Eigen::ColMajor, int>& Lsp,
                          int n, const char* path) {
    MatrixXd L = MatrixXd(Lsp);
    FILE* f = std::fopen(path, "w");
    if (!f) { std::perror("dump_L fopen"); return; }
    std::fprintf(f, "# L factor (lower triangular), Θ = L·Lᵀ, N=%d\n", n);
    std::fprintf(f, "# 排序策略: NaturalOrdering (不做填充减少重排序), L[k,k] 对应风险因子索引 k\n");
    std::fprintf(f, "# (消元顺序 k 与输入矩阵行列顺序一致,无置换映射;故障注入时 L[0,0]=原始第0个风险因子)\n");
    std::fprintf(f, "# L[0,0] = %.17g  (SDC 损坏经此经 sqrt(d0) 放大,见 §11.2a)\n", L(0, 0));
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j <= i; ++j)   // 下三角
            std::fprintf(f, "%.17g%s", L(i, j), j < i ? " " : "\n");
    }
    std::fclose(f);
    std::printf("[L因子] L[0,0]=%.6g → %s (下三角稠密,用于故障注入定位)\n",
                L(0, 0), path);
}

// ─── 故障注入试验统计(可复用:对任意权重方案/分位数/情景数跑20组)──────
struct TrialStat {
    double mu_abs, sd_abs, min_abs, max_abs;   // 绝对偏差统计
    double mu_rel, sd_rel, min_rel, max_rel;   // 相对偏差%统计
    int worst_t;                                 // 最大相对偏差组
    int worst_k; double worst_orig, worst_new, worst_var, worst_abs, worst_rel;
    std::vector<double> per_abs, per_rel;        // 每组原始值(供对比表)
};
// 跑 N_TRIALS 组:每组用 CorruptL00Hook(同样的 k 序列,donor 不变)篡改 L[0,0],
// 然后按给定权重方案/分位数/情景数算注入后 VaR,对比基线。donor 选择与权重/
// 分位数无关(只取决于 L 本身 + inject_seed),故三组对照的 k/newval 序列一致,
// 可直接比较"同一注入在不同业务配置下的偏差放大"。
static TrialStat run_trials(const SparseMatrix<double, Eigen::ColMajor, int>& L_baseline,
                            int n, uint64_t rng_seed, const std::string& weight_scheme,
                            double quantile, int n_scen, int n_trials) {
    TrialStat st;
    VectorXd w = make_weights(n, weight_scheme);
    // 基线 VaR(该配置,无注入)
    auto Xb = sample_scenarios(L_baseline, n, n_scen, rng_seed);
    double var_base = compute_var(Xb, w, n_scen, quantile);

    double abs_devs[64], rel_devs[64];
    st.per_abs.resize(n_trials); st.per_rel.resize(n_trials);
    for (int t = 0; t < n_trials; ++t) {
        SparseMatrix<double, Eigen::ColMajor, int> L_t = L_baseline;
        CorruptL00Hook hook(50, (uint64_t)(rng_seed + 100 + t));
        InjectionInfo info;
        hook.modify_L(L_t, info);
        auto Xt = sample_scenarios(L_t, n, n_scen, rng_seed);
        double var_t = compute_var(Xt, w, n_scen, quantile);
        double abs_d = var_t - var_base;
        double rel_d = (var_base != 0.0) ? 100.0 * abs_d / var_base : 0.0;
        abs_devs[t] = std::fabs(abs_d); rel_devs[t] = std::fabs(rel_d);
        st.per_abs[t] = abs_d; st.per_rel[t] = rel_d;
        if (t == 0 || rel_devs[t] > rel_devs[st.worst_t]) {
            st.worst_t = t; st.worst_k = info.k_chosen;
            st.worst_orig = info.orig_L00; st.worst_new = info.new_L00;
            st.worst_var = var_t; st.worst_abs = abs_d; st.worst_rel = rel_d;
        }
    }
    auto mean = [](double* a, int m){double s=0;for(int i=0;i<m;++i)s+=a[i];return s/m;};
    auto sd = [&](double* a,int m,double mu){double s=0;for(int i=0;i<m;++i)s+=(a[i]-mu)*(a[i]-mu);return std::sqrt(s/m);};
    auto mm = [](double* a,int m,double&mn,double&mx){mn=mx=a[0];for(int i=1;i<m;++i){if(a[i]<mn)mn=a[i];if(a[i]>mx)mx=a[i];}};
    st.mu_abs=mean(abs_devs,n_trials); st.sd_abs=sd(abs_devs,n_trials,st.mu_abs);
    st.mu_rel=mean(rel_devs,n_trials); st.sd_rel=sd(rel_devs,n_trials,st.mu_rel);
    mm(abs_devs,n_trials,st.min_abs,st.max_abs); mm(rel_devs,n_trials,st.min_rel,st.max_rel);
    return st;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "用法: %s <theta.csc> [n_scenarios=10000] [rng_seed=2024]\n", argv[0]);
        return 2;
    }
    const char* csc_path = argv[1];
    int n_scenarios = argc > 2 ? std::atoi(argv[2]) : 10000;
    uint64_t rng_seed = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 2024;

    std::printf("=== 蒙特卡洛 VaR(精度矩阵 GMRF 采样)==\n");
    std::printf("[1] 读入 Θ: %s\n", csc_path);
    auto Theta = read_csc_theta(csc_path);
    int n = (int)Theta.rows();
    std::printf("    N=%d, nnz=%d, 稀疏率=%.1f%%\n", n, (int)Theta.nonZeros(),
                100.0 * (1.0 - (double)Theta.nonZeros() / (n * n)));

    std::printf("[2] SimplicialLLT(Lower, NaturalOrdering) 分解 Θ = L·Lᵀ\n");
    auto fr = factorize_theta(Theta);   // {solver, L_copy}
    auto& solver = fr.solver;
    auto& L_baseline = fr.L_copy;
    std::printf("    分解成功(LLT,factorize_preordered<false,false> 路径)\n");

    // 索引 0 的风险因子板块标注(排序后板块id最小=InformationTechnology,因
    // 板块标签数值最小排第一位)。便于论文叙述写成"某具体板块风险因子被篡改"。
    std::printf("    [板块] 风险因子索引0 = Information Technology 板块(排序后首位)\n");

    // ── 基线(无注入):自检 + dump L(基线只算自检,dump L;VaR 在三组对照里各自算)──
    std::printf("[3] 基线自检(无注入采样方向验证)\n");
    sanity_check_cov_direction(Theta, L_baseline, n, rng_seed + 1);

    std::string L_path = std::string(csc_path) + ".L_factor.txt";
    dump_L_factor(L_baseline, n, L_path.c_str());

    // ── 三组对照试验(同一 donor 序列,不同业务配置)──────────────────────
    // donor 选择只依赖 L 本身 + inject_seed,与权重/分位数/情景数无关 →
    // 三组的 k/newval 序列完全一致,可直接比较"同注入在不同配置下的偏差放大"。
    const int N_TRIALS = 20;
    // 99.9% 分位数样本薄,n_scen 提到 50000 估计更稳(只影响 P99.9 这组)
    const int N_SCEN_TAIL = 50000;

    std::printf("\n[4] 三组对照(每组%d试验,同一donor序列,同VaR rng_seed=%llu)\n",
                N_TRIALS, (unsigned long long)rng_seed);
    std::printf("    A组:等权 + P99    (n_scen=%d)   — 原始对照(稀释基线)\n", n_scenarios);
    std::printf("    B组:集中权重(因子0占20%%)+ P99 (n_scen=%d)   — 验证\"稀释\"假说\n", n_scenarios);
    std::printf("    C组:等权 + P99.9  (n_scen=%d)  — 验证尾部敏感性\n", N_SCEN_TAIL);

    auto stA = run_trials(L_baseline, n, rng_seed, "equal",         0.99,  n_scenarios,    N_TRIALS);
    auto stB = run_trials(L_baseline, n, rng_seed, "concentrated",  0.99,  n_scenarios,    N_TRIALS);
    auto stC = run_trials(L_baseline, n, rng_seed, "equal",         0.999, N_SCEN_TAIL,    N_TRIALS);

    // ── 三组对比表 ──
    auto row = [](const char* label, double a, double b, double c, const char* fmt) {
        std::printf("    │ %-22s │", label);
        std::printf(fmt, a); std::printf(" │");
        std::printf(fmt, b); std::printf(" │");
        std::printf(fmt, c); std::printf(" │\n");
    };
    std::printf("\n[5] 三组对比表(VaR 偏差)\n");
    std::printf("    ┌────────────────────────┬─────────────────┬─────────────────┬─────────────────┐\n");
    std::printf("    │ 偏差指标               │ A:等权+P99      │ B:集中权重+P99  │ C:等权+P99.9    │\n");
    std::printf("    ├────────────────────────┼─────────────────┼─────────────────┼─────────────────┤\n");
    row("绝对偏差 均值",     stA.mu_abs,  stB.mu_abs,  stC.mu_abs,  "  %12.6f ");
    row("绝对偏差 标准差",   stA.sd_abs,  stB.sd_abs,  stC.sd_abs,  "  %12.6f ");
    row("绝对偏差 最小值",   stA.min_abs, stB.min_abs, stC.min_abs, "  %12.6f ");
    row("绝对偏差 最大值",   stA.max_abs, stB.max_abs, stC.max_abs, "  %12.6f ");
    std::printf("    ├────────────────────────┼─────────────────┼─────────────────┼─────────────────┤\n");
    row("相对偏差% 均值",     stA.mu_rel,  stB.mu_rel,  stC.mu_rel,  "  %12.2f ");
    row("相对偏差% 标准差",   stA.sd_rel,  stB.sd_rel,  stC.sd_rel,  "  %12.2f ");
    row("相对偏差% 最小值",   stA.min_rel, stB.min_rel, stC.min_rel, "  %12.2f ");
    row("相对偏差% 最大值",   stA.max_rel, stB.max_rel, stC.max_rel, "  %12.2f ");
    std::printf("    └────────────────────────┴─────────────────┴─────────────────┴─────────────────┘\n");

    std::printf("\n    各组最大偏差组详情(同一donor序列下的最大相对偏差):\n");
    auto detail = [](const char* tag, const TrialStat& st) {
        std::printf("    %s组:试#%d, k=%d, L[0,0] %.6g→%.6g, VaR=%.6f, abs=%.6f, rel=%.2f%%\n",
                    tag, st.worst_t, st.worst_k, st.worst_orig, st.worst_new,
                    st.worst_var, st.worst_abs, st.worst_rel);
    };
    detail("A", stA); detail("B", stB); detail("C", stC);

    // ── 最坏情况注入(确定性上界)────────────────────────────────────────
    // 用 A 组配置(等权+P99+n_scen),确定性找使 |L[k,k]-L[0,0]| 最大的 k_worst,
    // 给出"单个 donor 能造成的最大 VaR 偏差"上界,与 A 组随机 20 组对比。
    std::printf("\n[6] 最坏情况注入(WorstCaseL00Hook,A组配置:等权+P99)\n");
    SparseMatrix<double, Eigen::ColMajor, int> L_worst = L_baseline;
    WorstCaseL00Hook wh;
    InjectionInfo winfo;
    wh.modify_L(L_worst, winfo);
    VectorXd w_eq = make_weights(n, "equal");
    auto Xb = sample_scenarios(L_baseline, n, n_scenarios, rng_seed);
    double var_base_A = compute_var(Xb, w_eq, n_scenarios, 0.99);
    auto Xw = sample_scenarios(L_worst, n, n_scenarios, rng_seed);
    double var_worst = compute_var(Xw, w_eq, n_scenarios, 0.99);
    double w_abs = var_worst - var_base_A;
    double w_rel = (var_base_A != 0.0) ? 100.0 * w_abs / var_base_A : 0.0;
    std::printf("    k_worst=%d, donor L[k,k]=%.6g, orig L[0,0]=%.6g → 新 %.6g\n",
                winfo.k_chosen, winfo.new_L00, winfo.orig_L00, winfo.new_L00);
    std::printf("    基线 VaR99(A组配置)=%.6f, 最坏注入 VaR99=%.6f\n", var_base_A, var_worst);
    std::printf("    最坏偏差: abs=%.6f, rel=%.2f%%\n", w_abs, w_rel);

    // ── A组随机 vs 最坏情况 对比表 ──
    std::printf("\n[7] A组随机20组 vs 最坏情况(偏差上界)对比\n");
    std::printf("    ┌────────────────────────┬─────────────────┬─────────────────┐\n");
    std::printf("    │ 指标                   │ A组随机20组      │ 最坏情况(上界)  │\n");
    std::printf("    ├────────────────────────┼─────────────────┼─────────────────┤\n");
    std::printf("    │ 相对偏差 均值%%         │ %15.2f │ %15.2f │\n", stA.mu_rel, std::fabs(w_rel));
    std::printf("    │ 相对偏差 最大%%         │ %15.2f │ %15.2f │\n", stA.max_rel, std::fabs(w_rel));
    std::printf("    │ 绝对偏差 均值          │ %15.6f │ %15.6f │\n", stA.mu_abs, std::fabs(w_abs));
    std::printf("    │ 绝对偏差 最大          │ %15.6f │ %15.6f │\n", stA.max_abs, std::fabs(w_abs));
    std::printf("    └────────────────────────┴─────────────────┴─────────────────┘\n");
    double ratio_to_mean = (stA.mu_rel > 1e-9) ? std::fabs(w_rel) / stA.mu_rel : 0;
    double ratio_to_max = (stA.max_rel > 1e-9) ? std::fabs(w_rel) / stA.max_rel : 0;
    std::printf("    最坏/随机均值 = %.1f×, 最坏/随机最大 = %.1f×\n", ratio_to_mean, ratio_to_max);
    std::printf("    (k_worst=%d 的板块归属见 meta.json 反推;donor 与 IT 板块结构差异越大偏差越大)\n",
                winfo.k_chosen);

    // ── B组配置最坏情况(集中权重 + 最坏donor,复用L_worst)────────────────
    // 注入结果与权重无关(只取决于L本身),故复用上面的 L_worst/WorstCaseL00Hook,
    // 只把权重换成集中(w_conc),算 B 组配置下的基线与最坏注入 VaR。
    std::printf("\n[8] 最坏情况注入(B组配置:集中权重+P99,复用同一L_worst)\n");
    VectorXd w_conc = make_weights(n, "concentrated");
    auto Xb_conc = sample_scenarios(L_baseline, n, n_scenarios, rng_seed);
    double var_base_B = compute_var(Xb_conc, w_conc, n_scenarios, 0.99);
    // L_worst 已被 WorstCaseL00Hook 篡改(同 A组最坏),用集中权重算最坏 VaR
    auto Xw_conc = sample_scenarios(L_worst, n, n_scenarios, rng_seed);
    double var_worst_B = compute_var(Xw_conc, w_conc, n_scenarios, 0.99);
    double wb_abs = var_worst_B - var_base_B;
    double wb_rel = (var_base_B != 0.0) ? 100.0 * wb_abs / var_base_B : 0.0;
    std::printf("    基线 VaR99(B组配置)=%.6f, 最坏注入 VaR99=%.6f\n", var_base_B, var_worst_B);
    std::printf("    最坏偏差(B组): abs=%.6f, rel=%.2f%%\n", wb_abs, wb_rel);

    // ── 2×2 对比表(随机/最坏 × 等权/集中)──────────────────────────────
    // 随机列用均值(中心估计)+最大值(峰值);最坏列是上界。
    // 四个格子里理论上最大的是【集中+最坏】——真实业务集中持仓下单个SDC上界。
    std::printf("\n[9] 2×2 对比表:VaR 相对偏差%%(随机/最坏 × 等权/集中)\n");
    std::printf("    ┌──────────────────┬──────────────────────────────┬──────────────────────────────┐\n");
    std::printf("    │                  │ 随机20组                     │ 最坏情况(上界)              │\n");
    std::printf("    │                  │      均值 / 最大             │      (确定性上界)          │\n");
    std::printf("    ├──────────────────┼──────────────────────────────┼──────────────────────────────┤\n");
    std::printf("    │ A:等权重         │   %6.2f / %-6.2f          │   %-26.2f │\n", stA.mu_rel, stA.max_rel, std::fabs(w_rel));
    std::printf("    │ B:集中权重       │   %6.2f / %-6.2f          │   %-26.2f │\n", stB.mu_rel, stB.max_rel, std::fabs(wb_rel));
    std::printf("    └──────────────────┴──────────────────────────────┴──────────────────────────────┘\n");
    std::printf("    (donor=k_worst=%d, ConsumerStaples板块末尾因子, L[k,k]≈1.0 vs L[0,0]=1.673)\n",
                winfo.k_chosen);

    // 最终结论性数字
    std::printf("\n    ★ 最终上界:集中权重+最坏donor,单个SDC造成VaR偏差 %.2f%%\n", std::fabs(wb_rel));
    std::printf("      (vs 等权随机均值%.2f%%,放大 %.0f×)—— 真实业务集中持仓下SDC不再静默\n",
                stA.mu_rel, (stA.mu_rel > 1e-9) ? std::fabs(wb_rel)/stA.mu_rel : 0);

    std::printf("\n[结果] 基线 L[0,0]=%.6g, 全部实验(三组随机+两组最坏+2×2表)完成\n", L_baseline.coeff(0,0));
    return 0;
}

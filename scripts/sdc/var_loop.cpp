// var_loop.cpp — 核179真跑循环版(v2:AMD排序 + 对角线扫描 + 同进程循环factorize)
//
// 【v2 调整(2026-08-14)】
// 调整1:排序从NaturalOrdering改回默认AMDOrdering。上次用NaturalOrdering导致
//   L[0,0]那一列非零元个数(暴露窗口)和当初定位触发条件时不是同一回事;
//   改回AMD恢复原始触发窗口。代价:AMD重排打乱"L[k,k]对应第k个风险因子"映射,
//   故不再假设异常只在L[0,0]。比对逻辑扩为扫描整个对角线每个L[k,k]。
// 调整2:从"每轮起新进程做1次compute"改成"同进程analyzePattern一次+循环factorize"。
//   这是§10.2里触发率高4倍的numeric-only模式(复用符号分解,反复数值分解)。
//   单轮耗时会变短(无进程启动开销),需重新实测。
//
// 【MISMATCH甄别标准调整】不再用"是不是在L[0,0]"作唯一判据,改成看
// "是不是集中在对角线单一位置、且非对角线元素基本不变"——这是已知机理更本质的
// 特征(损坏经sqrt(d0)写入某一个具体的L[k,k],不是随机分散)。日志里记录
// 所有变化的对角元位置,便于甄别是"单点对角损坏"还是"分散浮点噪声"。
//
// 数据保全(维持v1标准):每轮写完L+VaR后立即fsync+fdatasync+system sync三重保险。
// 结果日志每次写入也fsync。诚实边界不变:崩溃那一轮可能追不回。
//
// 用法:var_loop <theta.csc> <n_rounds> <out_dir> <golden_L> <golden_var99> [rng_seed] [round_start_id]
//   n_rounds: 进程内循环调用factorize的次数(不是单轮了)
//   round_start_id: 轮次编号起点(外层多窗口时用,默认1)
// 退出码:0=全MATCH, 2=有MISMATCH, 3=有factorize失败, 其他=异常
//
// SPDX-License-Identifier: Apache-2.0
#include <Eigen/Sparse>
#include <Eigen/Dense>
#include <vector>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <ctime>

using namespace Eigen;
// 调整1:改回默认AMD(不显式指定Ordering模板参数,Eigen默认AMDOrdering)
using SC = Eigen::SimplicialLLT<SparseMatrix<double, Eigen::ColMajor, int>>;

// 读 CSC
static SparseMatrix<double, Eigen::ColMajor, int> read_theta(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) { fprintf(stderr, "open %s fail\n", path); exit(2); }
    int N, nnz; fscanf(f, "%d %d", &N, &nnz);
    std::vector<int> Ap(N+1), Ai(nnz); std::vector<double> Ax(nnz);
    for (int j=0;j<=N;++j) fscanf(f,"%d",&Ap[j]);
    for (int p=0;p<nnz;++p) fscanf(f,"%d",&Ai[p]);
    for (int p=0;p<nnz;++p) fscanf(f,"%lf",&Ax[p]);
    fclose(f);
    std::vector<Eigen::Triplet<double>> t;
    for (int j=0;j<N;++j) for (int p=Ap[j];p<Ap[j+1];++p) t.push_back({Ai[p],j,Ax[p]});
    SparseMatrix<double, Eigen::ColMajor, int> Th(N,N); Th.setFromTriplets(t.begin(),t.end());
    return Th;
}

static void hard_flush(int fd) {
    if (fd >= 0) { fsync(fd); fdatasync(fd); }
    std::system("sync");
}

static void write_file_fsync(const char* path, const char* data, size_t len) {
    int fd = open(path, O_WRONLY|O_CREAT|O_TRUNC, 0644);
    if (fd < 0) { perror(path); return; }
    if (write(fd, data, len) < 0) perror("write");
    hard_flush(fd);
    close(fd);
}

static double read_golden_var(const char* path) {
    std::ifstream f(path);
    std::string line, num;
    while (std::getline(f, line)) {
        std::istringstream iss(line);
        std::string tok;
        while (iss >> tok) {
            if (tok.find('=') != std::string::npos) {
                num = tok.substr(tok.find('=')+1);
            } else {
                char* end; double d = strtod(tok.c_str(), &end);
                if (end != tok.c_str()) return d;
            }
        }
    }
    return strtod(num.c_str(), nullptr);
}

// 读golden L为稠密矩阵(用于逐元素+对角线扫描比对)
static MatrixXd read_golden_L(const char* path, int N) {
    MatrixXd G = MatrixXd::Zero(N, N);
    std::ifstream f(path);
    std::string line;
    int i = 0;
    while (std::getline(f, line) && i < N) {
        size_t p = line.find_first_not_of(" \t");
        if (p == std::string::npos) continue;
        if (line[p] == '#') continue;
        std::istringstream iss(line);
        for (int j = 0; j <= i; ++j) {
            double v; if (iss >> v) G(i, j) = v;
        }
        i++;
    }
    return G;
}

int main(int argc, char** argv) {
    if (argc < 6) {
        fprintf(stderr, "用法: %s <theta.csc> <n_rounds> <out_dir> <golden_L> <golden_var99> [rng_seed=2024] [round_start_id=1]\n", argv[0]);
        return 2;
    }
    const char* csc = argv[1];
    int n_rounds = atoi(argv[2]);
    const char* outdir = argv[3];
    const char* golden_L_path = argv[4];
    const char* golden_var_path = argv[5];
    uint32_t rng_seed = argc > 6 ? (uint32_t)strtoul(argv[6],0,10) : 2024;
    int rid_start = argc > 7 ? atoi(argv[7]) : 1;

    auto Theta = read_theta(csc);
    int N = Theta.rows();

    // 读golden L(稠密,用于全量+对角线比对)
    MatrixXd G_L = read_golden_L(golden_L_path, N);
    double g_var = read_golden_var(golden_var_path);

    // 调整2:analyzePattern一次(符号分解,建立稀疏结构)
    SC solver;
    solver.analyzePattern(Theta);
    if (solver.info() != Eigen::Success) {
        fprintf(stderr, "analyzePattern失败\n");
        return 4;
    }

    int total_mismatch = 0, total_fail = 0;
    int mismatch_in_window = 0;   // 窗口内MISMATCH计数(区分第一次vs后续)
    char path[512];

    for (int r = 0; r < n_rounds; ++r) {
        int rid = rid_start + r;

        // 调整2:循环调用factorize(numeric-only模式,§10.2触发率高4倍)
        solver.factorize(Theta);
        if (solver.info() != Eigen::Success) {
            total_fail++;
            char logbuf[256];
            int ll = snprintf(logbuf, sizeof(logbuf),
                "round %04d: FACTORIZE_FAIL (不正定)\n", rid);
            snprintf(path, sizeof(path), "%s/result_log.txt", outdir);
            int fd = open(path, O_WRONLY|O_CREAT|O_APPEND, 0644);
            if (fd >= 0) { write(fd, logbuf, ll); hard_flush(fd); close(fd); }
            printf("round %04d: FACTORIZE_FAIL\n", rid);
            continue;
        }

        SparseMatrix<double, Eigen::ColMajor, int> L = solver.matrixL();
        // 稠密化便于对角线扫描比对
        MatrixXd Ld = MatrixXd(L);

        // 采样10000情景(每轮用同一rng_seed,因为factorize前状态固定→应给同一L→同一VaR)
        // 注:同rng_seed是为了"每次factorize结果应完全一致"这个确定性前提;
        // 若L变了(真实SDC),VaR才会变。RNG不变排除RNG非确定性。
        uint32_t st = rng_seed; auto u32=[&](){st=st*1103515245u+12345u;return st;};
        auto unif=[&](){return ((u32()>>8)&0xffffff)/(double)0x1000000;};
        auto norm=[&](){double u1=unif(),u2=unif(); if(u1<1e-12)u1=1e-12; return std::sqrt(-2*std::log(u1))*std::cos(2*M_PI*u2);};
        MatrixXd X(N, 10000); VectorXd Z(N);
        for (int s=0;s<10000;++s) {
            for (int i=0;i<N;++i) Z(i)=norm();
            L.transpose().triangularView<Eigen::Upper>().solveInPlace(Z);
            X.col(s)=Z;
        }

        // VaR99
        VectorXd losses(10000); double w=1.0/N;
        for (int s=0;s<10000;++s) losses(s) = -w * X.col(s).sum();
        std::sort(losses.data(), losses.data()+10000);
        double var99 = losses[(int)std::floor(0.99*9999)];

        // 写L+VaR,fsync
        snprintf(path, sizeof(path), "%s/run_%04d_L.txt", outdir, rid);
        std::ostringstream oss;
        oss << "# round " << rid << " L factor (AMD ordering), N=" << N << "\n";
        // 调整1:头部打印所有对角元(便于快速扫描哪个变了)
        oss << "# diag[0..4]=";
        for (int k=0;k<5 && k<N;++k) oss << " " << std::setprecision(17) << Ld(k,k);
        oss << "\n";
        for (int i=0;i<N;++i) {
            for (int j=0;j<=i;++j) oss << Ld(i,j) << (j<i?" ":"\n");
        }
        std::string Lstr = oss.str();
        write_file_fsync(path, Lstr.data(), Lstr.size());

        snprintf(path, sizeof(path), "%s/run_%04d_var99.txt", outdir, rid);
        char vbuf[128]; int vl = snprintf(vbuf,sizeof(vbuf),"%.17g\n",var99);
        write_file_fsync(path, vbuf, vl);

        // 比对:VaR + L全量 + 对角线专门扫描
        bool var_match = (std::fabs(var99 - g_var) < 1e-9);

        // L全量比对(下三角)
        bool L_match = true; int L_diff_count = 0; double L_max_diff = 0;
        for (int i=0;i<N;++i) {
            for (int j=0;j<=i;++j) {
                double diff = std::fabs(Ld(i,j) - G_L(i,j));
                if (diff > 1e-12) {
                    L_diff_count++;
                    if (diff > L_max_diff) L_max_diff = diff;
                    if (diff > 1e-9) L_match = false;
                }
            }
        }

        // 调整1:对角线专门扫描 —— 记录哪些对角元变了(已知机理:单点对角损坏)
        int diag_diff_count = 0;
        std::string diag_diff_positions;  // 变化的对角元位置列表
        for (int k=0;k<N;++k) {
            double d = std::fabs(Ld(k,k) - G_L(k,k));
            if (d > 1e-12) {
                diag_diff_count++;
                char buf[64]; snprintf(buf,sizeof(buf),"%d(%.3e)%s",
                    k, d, diag_diff_count<10?",":"...");
                diag_diff_positions += buf;
            }
        }

        bool round_match = var_match && L_match;
        // 问题2:区分"本窗口第一次MISMATCH"(独立候选事件)vs"后续MISMATCH"(可能连锁)
        const char* mismatch_tag = "";
        if (!round_match) {
            total_mismatch++;
            mismatch_in_window++;
            mismatch_tag = (mismatch_in_window == 1)
                ? "FIRST_MISMATCH(独立候选事件)"
                : "SUBSEQUENT_MISMATCH(可能连锁,需单独标记)";
        }

        // 结果日志(追加+fsync)—— MISMATCH标注第一次/后续
        snprintf(path, sizeof(path), "%s/result_log.txt", outdir);
        char logbuf[896];
        int ll = snprintf(logbuf, sizeof(logbuf),
            "round %04d: VaR99=%.17g (g %.17g) %s | L %s (diff=%d max=%.3e) | diag_diff=%d [%s]%s%s\n",
            rid, var99, g_var,
            var_match?"MATCH":"MISMATCH",
            L_match?"MATCH":"MISMATCH", L_diff_count, L_max_diff,
            diag_diff_count, diag_diff_positions.c_str(),
            mismatch_tag[0]?" | ":"",
            mismatch_tag);
        int fd = open(path, O_WRONLY|O_CREAT|O_APPEND, 0644);
        if (fd >= 0) { write(fd, logbuf, ll); hard_flush(fd); close(fd); }

        printf("round %04d: var_%s L_%s diag_diff=%d %s\n", rid,
               var_match?"MATCH":"MISMATCH",
               L_match?"MATCH":"MISMATCH", diag_diff_count,
               mismatch_tag[0]?mismatch_tag:"");

        // 问题2:MISMATCH后重新analyzePattern重置solver内部状态
        // 权衡:重置后后续轮次的factorize不再是"同一符号分解的连续numeric-only"
        // → §10.2的numeric-only高触发率优势在重置后到下一次MISMATCH之间仍保留
        //   (重置只影响"MISMATCH之后到窗口结束"这段,这段的状态可能已被污染)
        // → 重置比"在可能已污染状态上持续跑"更安全,触发率损失可接受
        //   (因为重置只在MISMATCH后做一次,不是每轮都做)
        if (!round_match) {
            solver.analyzePattern(Theta);   // 重置符号分解状态
            if (solver.info() != Eigen::Success) {
                fprintf(stderr, "r%d: MISMATCH后重新analyzePattern失败\n", rid);
                return 4;
            }
        }
    }

    // 退出码(var_loop内部):0=全MATCH, 2=有MISMATCH, 3=factorize失败
    if (total_mismatch > 0) return 2;
    if (total_fail > 0) return 3;
    return 0;
}


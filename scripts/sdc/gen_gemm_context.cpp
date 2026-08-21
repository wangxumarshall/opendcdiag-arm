// 复现 GEMM 测试的输入矩阵 A/B 和参考 C, dump 成二进制供 Python 做精确匹配.
// 关键依据:
//   1. sandstone 把 srand/srandom/srand48 全部 abort(), Eigen::Random 用 std::rand(),
//      默认 srand(1) 种子 -> 序列完全确定可复现.
//   2. 必须 *按 --enable 顺序* 依次 init 每个测试, 因为它们共享同一个 std::rand() 全局序列.
// enable 顺序 (来自日志 command-line):
//   eigen_gemm_double14 (256x256 double)
//   eigen_gemm_cdouble_dynamic_square (221x221 complex<double>, 每元素2次rand)
//   eigen_gemm_double_dynamic_square (256x256 double)   <-- 我们要这个
//   eigen_gemm_float_dynamic_square (256x256 float)    <-- 和这个
//   ... (后面是 sparse/svd, 不消费 std::rand, 不影响)
// 每个 test init: lhs = Random(N,N); rhs = Random(N,N); (lhs*rhs 在我们这不需要复现,
//   因为我们直接重算 prod; 但为对齐日志, 我们也复现 prod)
//
// Eigen random<double> = -1 + 2*std::rand()/RAND_MAX  (x=-1,y=1)
// Eigen random<float>  同理用 float
// Eigen random<complex<double>> = complex(random<double>(), random<double>)

#include <Eigen/Core>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace Eigen;

typedef Matrix<double, Dynamic, Dynamic> DMat;
typedef Matrix<float, Dynamic, Dynamic> FMat;
typedef Matrix<std::complex<double>, Dynamic, Dynamic> CDMat;

static void dump_matrix(const char* path, const double* data, size_t n) {
    FILE* f = fopen(path, "wb");
    fwrite(data, sizeof(double), n, f);
    fclose(f);
    printf("wrote %s (%zu doubles)\n", path, n);
}
static void dump_matrixf(const char* path, const float* data, size_t n) {
    FILE* f = fopen(path, "wb");
    fwrite(data, sizeof(float), n, f);
    fclose(f);
    printf("wrote %s (%zu floats)\n", path, n);
}

int main() {
    // 关键: 不调 srand, 用默认种子 1 (与 opendcdiag 运行时一致, 因为 srand 被 abort)
    // 注意: 本程序是独立进程, 其 std::rand 默认就是 srand(1) 行为, 与 opendcdiag 内一致.

    // 1. eigen_gemm_double14: 256x256 double, 消费 256*256*2 个 rand
    {
        DMat lhs = DMat::Random(256, 256);
        DMat rhs = DMat::Random(256, 256);
        (void)(lhs * rhs);  // prod 不 dump (日志没用到), 但复现消费
        printf("double14 init done (consumed rand)\n");
    }
    // 2. eigen_gemm_cdouble_dynamic_square: 221x221 complex<double>, 每元素2次rand
    {
        CDMat lhs = CDMat::Random(221, 221);
        CDMat rhs = CDMat::Random(221, 221);
        (void)(lhs * rhs);
        printf("cdouble init done (consumed rand)\n");
    }
    // 3. eigen_gemm_double_dynamic_square: 256x256 double  <-- 目标
    {
        DMat lhs = DMat::Random(256, 256);
        DMat rhs = DMat::Random(256, 256);
        DMat prod = lhs * rhs;   // 参考值 = expected
        dump_matrix("ctx_dd_lhs.bin", lhs.data(), 256*256);
        dump_matrix("ctx_dd_rhs.bin", rhs.data(), 256*256);
        dump_matrix("ctx_dd_prod.bin", prod.data(), 256*256);
        printf("double_dynamic_square init done\n");
    }
    // 4. eigen_gemm_float_dynamic_square: 256x256 float  <-- 目标
    {
        FMat lhs = FMat::Random(256, 256);
        FMat rhs = FMat::Random(256, 256);
        FMat prod = lhs * rhs;
        dump_matrixf("ctx_ff_lhs.bin", lhs.data(), 256*256);
        dump_matrixf("ctx_ff_rhs.bin", rhs.data(), 256*256);
        dump_matrixf("ctx_ff_prod.bin", prod.data(), 256*256);
        printf("float_dynamic_square init done\n");
    }
    return 0;
}

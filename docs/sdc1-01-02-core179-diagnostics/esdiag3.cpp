#include <Eigen/Sparse>
#include <Eigen/Dense>
#include <vector>
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <arm_acle.h>
using namespace Eigen;
constexpr int N=256;
static uint32_t rng_state;
static double frandom_scale(double s){ rng_state=rng_state*1103515245u+12345u; return s*((rng_state>>8)&0xffffff)/(double)0x1000000; }
static uint32_t crcA(const SparseMatrix<double>&A){
    // CRC over the value array of the sparse matrix (compressed form)
    uint32_t c=0xffffffffu; const double*v=A.valuePtr(); int nnz=A.nonZeros();
    for(int i=0;i<nnz*8;i++) c=__crc32cb(c,((const uint8_t*)v)[i]);
    return ~c;
}
int main(int argc,char**argv){
    int iters=(argc>1)?atoi(argv[1]):1000; rng_state=(argc>2)?(uint32_t)strtoul(argv[2],0,10):12345;
    std::vector<Triplet<double>> trip;
    for(int i=0;i<N;i++) for(int j=i+1;j<N;j++){ double x=frandom_scale(1.0); if(x<0.1){ trip.push_back({i,j,x}); trip.push_back({j,i,x}); } }
    for(int i=0;i<N;i++){ double x=fabs(frandom_scale(1.0))+0.05; trip.push_back({i,i,x}); }
    SparseMatrix<double> A(N,N); A.setFromTriplets(trip.begin(),trip.end());
    VectorXd b=VectorXd::Random(N);
    uint32_t a_crc=crcA(A);
    fprintf(stderr,"A built once: nnz=%d crcA=0x%08x (fixed)\n",A.nonZeros(),a_crc);
    SimplicialCholesky<SparseMatrix<double>> solver;
    VectorXd golden=solver.compute(A).solve(b);
    uint32_t gold_crc=0xffffffffu; for(int i=0;i<N*8;i++) gold_crc=__crc32cb(gold_crc,((const uint8_t*)golden.data())[i]); gold_crc=~gold_crc;
    fprintf(stderr,"golden crc=0x%08x\n",gold_crc);
    int fails=0, acorrupt=0;
    for(int k=0;k<iters;k++){
        uint32_t c=crcA(A);
        if(c!=a_crc) acorrupt++;
        SimplicialCholesky<SparseMatrix<double>> s2;
        VectorXd x=s2.compute(A).solve(b);
        uint32_t xc=0xffffffffu; for(int i=0;i<N*8;i++) xc=__crc32cb(xc,((const uint8_t*)x.data())[i]); xc=~xc;
        if(xc!=gold_crc){ fails++; if(fails<=3) fprintf(stderr,"k=%d x differs (a_crc_ok=%d)\n",k,c==a_crc); }
    }
    fprintf(stderr,"RESULT: A_corrupt=%d/%d  x_wrong=%d/%d\n",acorrupt,iters,fails,iters);
    printf("%d %d %d %d\n",acorrupt,iters,fails,iters);
    return 0;
}

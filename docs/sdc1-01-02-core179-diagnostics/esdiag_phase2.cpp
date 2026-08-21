// Separate Cholesky symbolic vs numeric. Reuse ONE solver instance that has
// analyzePattern done, then call factorize() repeatedly (numeric only).
// mode 0 = full compute each iter (symbolic+numeric)
// mode 1 = analyzePattern ONCE, then factorize() each iter (numeric-only)
// mode 2 = analyzePattern each iter only, then ONE factorize+solve at end to compare
#include <Eigen/Sparse>
#include <Eigen/Dense>
#include <vector>
#include <cstdio>
#include <cstdint>
#include <arm_acle.h>
using namespace Eigen;
constexpr int N=256;
static uint32_t rng_state;
static double frandom_scale(double s){ rng_state=rng_state*1103515245u+12345u; return s*((rng_state>>8)&0xffffff)/(double)0x1000000; }
static uint32_t crcVec(const VectorXd&v){ uint32_t c=0xffffffffu; const uint8_t*p=(const uint8_t*)v.data(); for(int i=0;i<N*8;i++) c=__crc32cb(c,p[i]); return ~c; }
int main(int argc,char**argv){
    int iters=argc>1?atoi(argv[1]):1000; rng_state=argc>2?(uint32_t)strtoul(argv[2],0,10):12345;
    int mode=argc>3?atoi(argv[3]):0;
    std::vector<Triplet<double>> trip;
    for(int i=0;i<N;i++) for(int j=i+1;j<N;j++){ double x=frandom_scale(1.0); if(x<0.1){ trip.push_back({i,j,x}); trip.push_back({j,i,x}); } }
    for(int i=0;i<N;i++){ double x=abs(frandom_scale(1.0))+0.05; trip.push_back({i,i,x}); }
    SparseMatrix<double> A(N,N); A.setFromTriplets(trip.begin(),trip.end());
    VectorXd b=VectorXd::Random(N);
    SimplicialCholesky<SparseMatrix<double>> gs;
    VectorXd golden=gs.compute(A).solve(b);
    uint32_t gc=crcVec(golden);
    fprintf(stderr,"golden crc=0x%08x\n",gc);
    int fail=0;
    if(mode==1){
        // numeric-only: reuse one symbolic pattern
        SimplicialCholesky<SparseMatrix<double>> s;
        s.analyzePattern(A);
        for(int k=0;k<iters;k++){
            s.factorize(A);
            VectorXd x=s.solve(b);
            if(crcVec(x)!=gc) fail++;
        }
    } else if(mode==2){
        // symbolic-only: redo analyzePattern each iter, factorize once to probe
        for(int k=0;k<iters;k++){
            SimplicialCholesky<SparseMatrix<double>> s;
            s.analyzePattern(A);
            s.factorize(A);
            VectorXd x=s.solve(b);
            if(crcVec(x)!=gc) fail++;
        }
    } else {
        for(int k=0;k<iters;k++){
            SimplicialCholesky<SparseMatrix<double>> s;
            VectorXd x=s.compute(A).solve(b);
            if(crcVec(x)!=gc) fail++;
        }
    }
    const char*names[]={"compute(both)","numeric-only(reuse symbolic)","symbolic-redo-each"};
    fprintf(stderr,"RESULT %s: %d/%d fails\n",names[mode],fail,iters);
    printf("%d\n",fail);
    return 0;
}

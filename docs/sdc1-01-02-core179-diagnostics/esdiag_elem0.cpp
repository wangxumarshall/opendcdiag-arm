// Numeric-only Cholesky, check which element is corrupted (elem[0]? or random?)
#include <Eigen/Sparse>
#include <Eigen/Dense>
#include <vector>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <arm_acle.h>
using namespace Eigen;
constexpr int N=256;
static uint32_t rng_state;
static double frandom_scale(double s){ rng_state=rng_state*1103515245u+12345u; return s*((rng_state>>8)&0xffffff)/(double)0x1000000; }
int main(int argc,char**argv){
    int iters=argc>1?atoi(argv[1]):1500; rng_state=argc>2?(uint32_t)strtoul(argv[2],0,10):12345;
    std::vector<Triplet<double>> trip;
    for(int i=0;i<N;i++) for(int j=i+1;j<N;j++){ double x=frandom_scale(1.0); if(x<0.1){ trip.push_back({i,j,x}); trip.push_back({j,i,x}); } }
    for(int i=0;i<N;i++){ double x=abs(frandom_scale(1.0))+0.05; trip.push_back({i,i,x}); }
    SparseMatrix<double> A(N,N); A.setFromTriplets(trip.begin(),trip.end());
    VectorXd b=VectorXd::Random(N);
    SimplicialCholesky<SparseMatrix<double>> gs;
    VectorXd golden=gs.compute(A).solve(b);
    SimplicialCholesky<SparseMatrix<double>> s;
    s.analyzePattern(A);
    int fail=0, elem0_fail=0, other_elem_fail=0;
    for(int k=0;k<iters;k++){
        s.factorize(A);
        VectorXd x=s.solve(b);
        // check per-element
        int bad_idx=-1;
        for(int i=0;i<N;i++){ if(x(i)!=golden(i)){ bad_idx=i; break; } }
        if(bad_idx>=0){
            fail++;
            if(bad_idx==0) elem0_fail++;
            else other_elem_fail++;
            if(fail<=3){
                uint64_t ax; memcpy(&ax,&x(0),8); uint64_t ex; memcpy(&ex,&golden(0),8);
                fprintf(stderr,"k=%d bad_idx=%d elem0(%s) actual=%.6g bits=0x%016lx xor=0x%016lx pc=%d\n",k,bad_idx,
                    bad_idx==0?"BAD":"ok", x(0), ax, ax^ex, __builtin_popcountll(ax^ex));
            }
        }
    }
    fprintf(stderr,"RESULT numeric-only: total_fail=%d/%d  elem0_fail=%d  other_fail=%d\n",fail,iters,elem0_fail,other_elem_fail);
    return 0;
}

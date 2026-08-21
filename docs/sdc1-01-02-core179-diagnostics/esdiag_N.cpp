// Vary matrix size N to see if corruption stays at elem[0] or scales with N
#include <Eigen/Sparse>
#include <Eigen/Dense>
#include <vector>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <arm_acle.h>
using namespace Eigen;
static uint32_t rng_state;
static double frandom_scale(double s, int idx){ rng_state=rng_state*1103515245u+12345u; return s*((rng_state>>8)&0xffffff)/(double)0x1000000; }
int main(int argc,char**argv){
    int N=argc>1?atoi(argv[1]):256; int iters=argc>2?atoi(argv[2]):1000; rng_state=argc>3?(uint32_t)strtoul(argv[3],0,10):12345;
    std::vector<Triplet<double>> trip;
    for(int i=0;i<N;i++) for(int j=i+1;j<N;j++){ double x=frandom_scale(1.0,i); if(x<0.1){ trip.push_back({i,j,x}); trip.push_back({j,i,x}); } }
    for(int i=0;i<N;i++){ double x=abs(frandom_scale(1.0,i))+0.05; trip.push_back({i,i,x}); }
    SparseMatrix<double> A(N,N); A.setFromTriplets(trip.begin(),trip.end());
    VectorXd b=VectorXd::Random(N);
    SimplicialCholesky<SparseMatrix<double>> gs;
    VectorXd golden=gs.compute(A).solve(b);
    SimplicialCholesky<SparseMatrix<double>> s; s.analyzePattern(A);
    int fail=0, e0=0; int first_bad=-1;
    for(int k=0;k<iters;k++){
        s.factorize(A); VectorXd x=s.solve(b);
        for(int i=0;i<N;i++){ if(x(i)!=golden(i)){ if(fail==0) first_bad=i; fail++; if(i==0)e0++; break; } }
    }
    fprintf(stderr,"N=%d: fail=%d/%d elem0=%d first_bad_idx=%d\n",N,fail,iters,e0,first_bad);
    return 0;
}

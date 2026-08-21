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
int main(int argc,char**argv){
    int iters=(argc>1)?atoi(argv[1]):1000; rng_state=(argc>2)?(uint32_t)strtoul(argv[2],0,10):12345;
    std::vector<Triplet<double>> trip;
    for(int i=0;i<N;i++) for(int j=i+1;j<N;j++){ double x=frandom_scale(1.0); if(x<0.1){ trip.push_back({i,j,x}); trip.push_back({j,i,x}); } }
    for(int i=0;i<N;i++){ double x=fabs(frandom_scale(1.0))+0.05; trip.push_back({i,i,x}); }
    SparseMatrix<double> A(N,N); A.setFromTriplets(trip.begin(),trip.end());
    VectorXd b=VectorXd::Random(N);
    SimplicialCholesky<SparseMatrix<double>> solver;
    VectorXd golden=solver.compute(A).solve(b);
    fprintf(stderr,"elem[0] addr=%p golden[0]=%.17g golden[0] bits=0x%016lx\n",&golden,golden(0), *(uint64_t*)&golden);
    int fails=0;
    for(int k=0;k<iters;k++){
        SimplicialCholesky<SparseMatrix<double>> s2;
        VectorXd x=s2.compute(A).solve(b);
        if(x(0)!=golden(0)){
            fails++;
            uint64_t ax; memcpy(&ax,&x(0),8); uint64_t ex; memcpy(&ex,&golden(0),8);
            if(fails<=5) fprintf(stderr,"k=%d x[0]addr=%p actual=%.17g bits=0x%016lx xor=0x%016lx pc=%d\n",k,&x,x(0),ax,ax^ex,__builtin_popcountll(ax^ex));
        }
    }
    fprintf(stderr,"fails=%d/%d\n",fails,iters);
    return 0;
}

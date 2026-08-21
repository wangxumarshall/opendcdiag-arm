/* mru_eigenmc.c — Pure-C driver for the Eigen-machine-code MRU.
 *
 * NO library dependency except libc. This driver is pure C (no Eigen headers).
 * The Eigen factorize/solve MACHINE CODE lives in eigen_cabidrv.o (compiled
 * from Eigen 5.0 templates with -fno-exceptions; its only undefined symbols
 * are libc/libm: malloc/free/memcpy/memset/assert/sqrt/...). We link that .o
 * into this pure-C binary. The final `ldd` shows ONLY libc/libm.
 *
 * This is the strongest form of the "no library except libc" reproducer: the
 * exact triggering Eigen instruction stream (factorize_preordered rank-1
 * update: fmsub + indirect ldr/str + long-lived d4 accumulator + per-call
 * malloc/free workspace) runs verbatim, but via a pure-C driver.
 *
 * Mirrors esdiag_phase2 mode 1: analyzePattern ONCE, then factorize()+solve()
 * repeated, CRC of x (the corrupted output, elem[0]) compared each iter.
 *
 * Build: gcc -O2 -march=armv8.1-a+crc+crypto -std=gnu17 -ffp-contract=fast \
 *          mru_eigenmc.c eigen_cabidrv.o -o mrueig -lm
 * Run:   taskset -c <CORE> ./mrueig <iters> <seed>
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <arm_acle.h>

#define N 256
#define MAXNNZ (N*128)

/* C ABI to the Eigen machine-code wrapper (eigen_cabidrv.o) */
extern void* escnew_solver(void);
extern void  escfree_solver(void* s);
extern int   escanalyze(void* s, const int* Ap, const int* Ai, const double* Ax, int n, int nnz);
extern int   escfactorize(void* s, const int* Ap, const int* Ai, const double* Ax, int n, int nnz);
extern int   escsolve(void* s, const double* b, double* x, int n);

/* input A: CSC lower triangle, SPD (diag-dominant) */
static int    Ap[N+1], Ai[MAXNNZ];
static double Ax[MAXNNZ];
static double bvec[N];

static uint32_t rng = 12345;
static double frand(void){ rng = rng*1103515245u + 12345u; return ((rng>>8)&0xffffff)/(double)0x1000000; }

static void build_matrix(void){
    int nz = 0; Ap[0] = 0;
    for(int j=0;j<N;j++){
        Ai[nz] = j; Ax[nz] = 0; nz++;          /* diagonal placeholder */
        for(int i=j+1;i<N;i++){
            if(frand() < 0.1){ Ai[nz]=i; Ax[nz]=frand()-0.5; nz++; }
        }
        Ap[j+1] = nz;
    }
    for(int j=0;j<N;j++){
        double s=0;
        for(int p=Ap[j];p<Ap[j+1];p++){ if(Ai[p]!=j) s += fabs(Ax[p]); }
        Ax[Ap[j]] = s + 2.0;
    }
    rng = 99999; for(int i=0;i<N;i++) bvec[i]=frand()-0.5;
}

static uint32_t crc_x(const double*x){
    uint32_t c=0xffffffffu; const uint8_t*p=(const uint8_t*)x;
    for(int i=0;i<N*8;i++) c=__crc32cb(c,p[i]);
    return ~c;
}

int main(int argc,char**argv){
    int iters=argc>1?atoi(argv[1]):1500;
    uint32_t seed=argc>2?(uint32_t)strtoul(argv[2],0,10):12345;
    rng = seed;   /* matches esdiag_phase2 seed param (used for matrix build) */
    build_matrix();
    int nnz = Ap[N];

    void* s = escnew_solver();
    if(!s){ fprintf(stderr,"alloc solver fail\n"); return 2; }

    /* golden: analyze + factorize + solve once */
    if(escanalyze(s, Ap, Ai, Ax, N, nnz)){ fprintf(stderr,"analyze fail\n"); escfree_solver(s); return 2; }
    if(escfactorize(s, Ap, Ai, Ax, N, nnz)){ fprintf(stderr,"factorize fail (golden)\n"); escfree_solver(s); return 2; }
    static double golden[N];
    escsolve(s, bvec, golden, N);
    uint32_t gcrc = crc_x(golden);
    fprintf(stderr,"eigen-MC MRU: golden x-crc=0x%08x x[0]=%.17g nnz=%d\n",gcrc,golden[0],nnz);

    /* mode 1: analyzePattern done ONCE (above), repeat factorize()+solve() */
    int fail=0;
    for(int k=0;k<iters;k++){
        if(escfactorize(s, Ap, Ai, Ax, N, nnz)){ fail++; continue; }
        static double x[N];
        escsolve(s, bvec, x, N);
        uint32_t c=crc_x(x);
        if(c!=gcrc){
            fail++;
            if(fail<=8) fprintf(stderr,"k=%d x-crc mismatch (0x%08x vs 0x%08x) x[0]=%.6g (golden %.6g)\n",k,c,gcrc,x[0],golden[0]);
        }
    }
    escfree_solver(s);
    fprintf(stderr,"RESULT eigen-MC MRU: %d/%d fails\n",fail,iters);
    printf("%d\n",fail);
    return fail>0;
}

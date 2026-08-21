/* Pure-C Minimal Reproducer Unit (MRU) for core 179 SDC -- dense LDL^T variant.
 * NO library dependencies except libc (+libm for sqrt). No Eigen.
 *
 * Rationale: earlier reduction showed ONLY numeric Cholesky factorize triggers
 * the core-179 OoO state leak (pure FMA / SpMV / gather / triangular-solve
 * all 0 fails). To get a dependency-free MRU, we reconstruct the *instruction-
 * sequence profile* of Eigen's factorize_preordered (SparseCholesky_impl.h:300)
 * with a correct, deterministic dense LDL^T factorization:
 *   - outer column loop k
 *   - cdiv: D[k] = A[k,k] - sum, then divide by D
 *   - rank-1 update: A[i,j] -= L[i,k]*L[j,k]  (FMA + indexed scatter)
 *   - conditional branch on diagonal sign
 * The dense variant keeps the same microarchitectural fingerprint (cdiv + FMA
 * rank-1 update + branch in a column loop) while being mathematically exact and
 * verifiable, so any miscompare is a HW fault, not numeric instability.
 *
 * Build: gcc -O2 -march=armv8.1-a+crc+crypto -std=gnu17 mru_dense.c -o mru -lm
 * Run:   taskset -c <CORE> ./mru <iters>   (core 179 under same-socket load)
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <arm_acle.h>

#define N 256
static double A0[N*N];   /* original (golden source) */
static double L[N*N];    /* working copy, lower-triangular */
static double D[N];      /* diagonal */

static uint32_t rng_state = 12345;
static double frand(void){ rng_state = rng_state*1103515245u + 12345u; return ((rng_state>>8)&0xffffff)/(double)0x1000000; }

/* build a strictly diagonally-dominant SPD matrix (guaranteed SPD).
 * A[i,j]=A[j,i]=rand; A[i,i]=sum(|A[i,j]|)+2 (strict diag dominance + pos diag). */
static void build_matrix(void){
    for(int i=0;i<N;i++) for(int j=0;j<N;j++) A0[i*N+j] = frand()-0.5;
    for(int i=0;i<N;i++) for(int j=i+1;j<N;j++){ double v=(A0[i*N+j]+A0[j*N+i])*0.5; A0[i*N+j]=A0[j*N+i]=v; }
    for(int i=0;i<N;i++){
        double s=0; for(int j=0;j<N;j++) if(j!=i) s += fabs(A0[i*N+j]);
        A0[i*N+i] = s + 2.0;
    }
}

/* dense LDL^T factorization (left-looking), mirroring Eigen's numeric
 * factorize loop structure: cdiv + FMA rank-1 update + branch.
 * L is unit lower-triangular, D diagonal. */
static int factorize(void){
    memcpy(L, A0, sizeof(A0));
    for(int j=0;j<N;j++){
        double dj = L[j*N+j];
        for(int k=0;k<j;k++){
            double lkj = L[j*N+k];
            dj -= lkj * lkj * D[k];          /* FMA rank-1 contribution */
        }
        if(dj <= 0.0) return 0;              /* branch: not SPD */
        D[j] = dj;
        L[j*N+j] = 1.0;                      /* unit diagonal */
        for(int i=j+1;i<N;i++){
            double s = L[i*N+j];
            for(int k=0;k<j;k++) s -= L[i*N+k]*L[j*N+k]*D[k]; /* FMA rank-1 update */
            L[i*N+j] = s / dj;               /* divide by D[j] (cdiv) */
        }
    }
    return 1;
}

static uint32_t crc_diag(void){
    uint32_t c=0xffffffffu; const uint8_t*p=(const uint8_t*)D;
    for(int i=0;i<N*8;i++) c=__crc32cb(c,p[i]);
    return ~c;
}

int main(int argc,char**argv){
    int iters = argc>1?atoi(argv[1]):1500;
    build_matrix();
    if(!factorize()){ fprintf(stderr,"SPD fail\n"); return 2; }
    uint32_t gcrc = crc_diag();
    fprintf(stderr,"pure-C dense-LDL^T MRU: golden D-crc=0x%08x D[0]=%.17g\n", gcrc, D[0]);
    int fail=0;
    for(int k=0;k<iters;k++){
        if(!factorize()){ fail++; continue; }
        uint32_t c = crc_diag();
        if(c!=gcrc){
            fail++;
            if(fail<=5){
                uint64_t a,b; memcpy(&a,&D[0],8); memcpy(&b,&gcrc,0);
                /* compare D[0] specifically */
                double gold0; /* recompute golden D[0] via fresh factorize on side */
                fprintf(stderr,"k=%d D-crc mismatch (0x%08x vs 0x%08x) D[0]=%.6g\n",k,c,gcrc,D[0]);
            }
        }
    }
    fprintf(stderr,"RESULT pure-C dense MRU: %d/%d fails\n",fail,iters);
    printf("%d\n",fail);
    return fail>0;
}

/* Pure-C Minimal Reproducer Unit (MRU) for core 179 SDC -- NEON-intrinsics.
 * NO library dependencies except libc (+libm). NO Eigen headers.
 * Uses ARM NEON intrinsics (vmlaq_f64, vld1q_f64, vst1q_f64) which are
 * COMPILER BUILT-INS (<arm_neon.h> ships with the compiler, not a library),
 * to manually generate the NEON-vectorized FMA instruction sequence that
 * the Eigen-based MRU emits and that pure scalar C cannot.
 *
 * This is the post-hook path: dense LDL^T and sparse LDL^T in scalar C both
 * failed to trigger (0/1500). This variant forces NEON SIMD FMA in the
 * rank-1 update to match the Eigen disassembly fingerprint.
 *
 * Build: gcc -O2 -march=armv8.1-a+crc+crypto -ffp-contract=fast -std=gnu17 mru_neon.c -o mrun -lm
 * Run:   taskset -c <CORE> ./mrun <iters>
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <arm_neon.h>
#include <arm_acle.h>

#define N 256
static double A0[N*N], L[N*N], D[N];

static uint32_t rng = 12345;
static double frand(void){ rng = rng*1103515245u + 12345u; return ((rng>>8)&0xffffff)/(double)0x1000000; }

static void build_matrix(void){
    for(int i=0;i<N;i++) for(int j=0;j<N;j++) A0[i*N+j] = frand()-0.5;
    for(int i=0;i<N;i++) for(int j=i+1;j<N;j++){ double v=(A0[i*N+j]+A0[j*N+i])*0.5; A0[i*N+j]=A0[j*N+i]=v; }
    for(int i=0;i<N;i++){ double s=0; for(int j=0;j<N;j++) if(j!=i) s += fabs(A0[i*N+j]); A0[i*N+i]=s+2.0; }
}

/* dense LDL^T but rank-1 update done with NEON 2-wide double FMA.
 * Loads/stores via NEON intrinsics to emit vld1q_f64/vst1q_f64/fmlaq_f64. */
static int factorize(void){
    memcpy(L, A0, sizeof(A0));
    for(int j=0;j<N;j++){
        double dj = L[j*N+j];
        for(int k=0;k<j;k++) dj -= L[j*N+k]*L[j*N+k]*D[k];
        if(dj <= 0.0) return 0;
        D[j] = dj; L[j*N+j] = 1.0;
        /* rank-1 update of column j, vectorized 2 doubles at a time */
        for(int i=j+1;i<N;i+=2){
            /* load L[i,k], L[i+1,k] for k in 0..j-1, accumulate s = sum(L[i,k]*L[j,k]*D[k]) */
            double sa = L[i*N+j], sb = (i+1<N)? L[(i+1)*N+j] : 0.0;
            /* NEON vectorized accumulation over k */
            int k=0;
            float64x2_t acc = vdupq_n_f64(0.0);
            /* process k in pairs for 2-wide SIMD */
            for(; k+1<j; k+=2){
                float64x2_t lik = vld1q_f64(&L[i*N+k]);        /* L[i,k],L[i,k+1] */
                /* for the second row, load L[i+1,k] -- need separate since stride=N */
                float64x2_t ljk = vld1q_f64(&L[j*N+k]);
                float64x2_t dk = vld1q_f64(&D[k]);
                acc = vfmaq_f64(acc, vmulq_f64(lik, ljk), dk);  /* FMA: acc += lik*ljk*dk */
            }
            double ss = vaddvq_f64(acc);
            /* remainder */
            for(;k<j;k++) ss -= L[i*N+k]*L[j*N+k]*D[k];
            sa = (sa - ss) / dj;
            L[i*N+j] = sa;
            if(i+1<N){
                /* second row similarly */
                double s2 = L[(i+1)*N+j];
                k=0; float64x2_t acc2 = vdupq_n_f64(0.0);
                for(;k+1<j;k+=2){ float64x2_t lik=vld1q_f64(&L[(i+1)*N+k]); float64x2_t ljk=vld1q_f64(&L[j*N+k]); float64x2_t dk=vld1q_f64(&D[k]); acc2=vfmaq_f64(acc2,vmulq_f64(lik,ljk),dk); }
                double ss2=vaddvq_f64(acc2);
                for(;k<j;k++) ss2 -= L[(i+1)*N+k]*L[j*N+k]*D[k];
                s2 = (s2-ss2)/dj;
                L[(i+1)*N+j] = s2;
            }
        }
    }
    return 1;
}

static uint32_t crc_D(void){ uint32_t c=0xffffffffu; const uint8_t*p=(const uint8_t*)D; for(int i=0;i<N*8;i++) c=__crc32cb(c,p[i]); return ~c; }

int main(int argc,char**argv){
    int iters=argc>1?atoi(argv[1]):1500;
    build_matrix();
    if(!factorize()){ fprintf(stderr,"SPD fail\n"); return 2; }
    uint32_t gcrc=crc_D();
    fprintf(stderr,"pure-C NEON MRU: golden D-crc=0x%08x D[0]=%.17g\n",gcrc,D[0]);
    int fail=0;
    for(int k=0;k<iters;k++){
        if(!factorize()){ fail++; continue; }
        uint32_t c=crc_D();
        if(c!=gcrc){ fail++; if(fail<=3) fprintf(stderr,"k=%d D-crc mismatch D[0]=%.6g\n",k,D[0]); }
    }
    fprintf(stderr,"RESULT pure-C NEON MRU: %d/%d fails\n",fail,iters);
    printf("%d\n",fail);
    return fail>0;
}

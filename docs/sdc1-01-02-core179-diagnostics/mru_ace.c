/* Pure-C Minimal Reproducer Unit (MRU) for core 179 SDC.
 * NO library dependencies except libc (+libm). NO Eigen. NO external headers
 * (arm_neon.h / arm_acle.h ship with the compiler).
 *
 * Designed per the repository's OWN SDC theory (microprobe/passes/sdc_analysis):
 *   - ACE score (Architectural Correction Execution / register exposure): keep
 *     values ALIVE in registers across many instructions (long def-to-last-use).
 *   - IBR score (Information Bit Richness): use WIDE 128-bit SIMD registers.
 *   - Memory pressure: use paired memory access (LDP/STP-style) + cache churn.
 *
 * The prior pure-C Cholesky variants failed because values died immediately
 * (low ACE) and used scalar registers (low IBR). This variant deliberately
 * keeps 8 live 128-bit FMA accumulators across a long unrolled loop with
 * heavy paired memory traffic, maximizing all three SDC-exposure scores.
 *
 * Build: gcc -O2 -march=armv8.1-a+crc+crypto -ffp-contract=fast -std=gnu17 mru_ace.c -o mrua -lm
 * Run:   taskset -c <CORE> ./mrua <iters>
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <arm_neon.h>
#include <arm_acle.h>

#define N 256
#define BLK 8   /* number of live 128-bit accumulators (high ACE + IBR) */

/* working buffers, cache-churn friendly */
static double A0[N*N] __attribute__((aligned(64)));
static double B0[N*N] __attribute__((aligned(64)));
static double C0[N*N] __attribute__((aligned(64)));
static double golden[BLK] __attribute__((aligned(64)));

static uint32_t rng = 12345;
static double frand(void){ rng = rng*1103515245u + 12345u; return ((rng>>8)&0xffffff)/(double)0x1000000; }

static void build(void){
    for(int i=0;i<N*N;i++){ A0[i]=frand()-0.5; B0[i]=frand()-0.5; }
}

/* A long, unrolled FMA chain with 8 live 128-bit accumulators (high ACE+IBR)
 * + paired memory access (vld1q_x2 / vst1q_x2 emulate LDP/STP pressure).
 * The accumulators stay live across the whole inner loop (long exposure). */
static void compute(double out[BLK]){
    float64x2_t acc[BLK];
    for(int b=0;b<BLK;b++) acc[b] = vdupq_n_f64(0.0);
    /* walk matrix rows in stride-2 (paired load) to churn cache + keep
     * accumulators live across many iterations */
    for(int i=0;i<N;i+=2){
        /* paired load of two rows (cache pressure / LDP-like) */
        float64x2_t a0 = vld1q_f64(&A0[i*N]);
        float64x2_t a1 = vld1q_f64(&A0[(i+1)*N]);
        float64x2_t b0 = vld1q_f64(&B0[i*N]);
        float64x2_t b1 = vld1q_f64(&B0[(i+1)*N]);
        for(int b=0;b<BLK;b++){
            /* FMA into long-lived accumulators: acc[b] += a*b*k */
            float64x2_t k = vdupq_n_f64((double)(b+1)*0.1);
            acc[b] = vfmaq_f64(acc[b], vfmaq_f64(a0, b0, k), a1);
            /* store back intermediate to create store-pressure + reload
             * (keeps accumulators "touched" across iterations = long ACE) */
            double tmp[2]; vst1q_f64(tmp, acc[b]);
            /* immediate reload keeps the value live in a different reg */
            acc[b] = vld1q_f64(tmp);
        }
    }
    for(int b=0;b<BLK;b++){ double t[2]; vst1q_f64(t, acc[b]); out[b]=t[0]+t[1]; }
}

static uint32_t crc_out(const double o[BLK]){
    uint32_t c=0xffffffffu; const uint8_t*p=(const uint8_t*)o;
    for(int i=0;i<BLK*8;i++) c=__crc32cb(c,p[i]);
    return ~c;
}

int main(int argc,char**argv){
    int iters=argc>1?atoi(argv[1]):1500;
    build();
    double out[BLK];
    compute(out);
    memcpy(golden, out, sizeof(golden));
    uint32_t gcrc = crc_out(golden);
    fprintf(stderr,"pure-C ACE-MRU: golden crc=0x%08x out[0]=%.17g\n",gcrc,golden[0]);
    int fail=0;
    for(int k=0;k<iters;k++){
        compute(out);
        uint32_t c = crc_out(out);
        if(c!=gcrc){
            fail++;
            if(fail<=5){
                uint64_t a,b; memcpy(&a,&out[0],8); memcpy(&b,&golden[0],8);
                fprintf(stderr,"k=%d crc mismatch out[0]=%.6g xor=0x%016lx pc=%d\n",k,out[0],a^b,__builtin_popcountll(a^b));
            }
        }
    }
    fprintf(stderr,"RESULT pure-C ACE-MRU: %d/%d fails\n",fail,iters);
    printf("%d\n",fail);
    return fail>0;
}

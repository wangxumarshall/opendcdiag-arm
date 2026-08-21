/* Pure-C Minimal Reproducer Unit (MRU) for core 179 SDC -- branch-miss variant.
 * NO library dependencies except libc (+libm). NO Eigen.
 *
 * Insight from PMU profiling: Eigen's esphase2 has 2.23% branch-miss rate
 * (data-dependent branches on sparse patterns) + 0.03 stalled-cycles/insn
 * (full pipeline). The prior ACE-MRU had only 0.79% branch-miss + 0.72 stall
 * (too predictable, too stalled). The trigger appears to need:
 *   HIGH branch-miss (pipeline flushes from unpredictable data-dependent
 *   branches) COMBINED WITH a full/saturated OoO pipeline (low stall).
 *
 * This variant: 8 live 128-bit FMA accumulators (high ACE+IBR, full pipeline)
 * + data-dependent branches whose outcome depends on computed values
 * (unpredictable, ~mimicking Eigen's sparse-pattern branches).
 *
 * Build: gcc -O2 -march=armv8.1-a+crc+crypto -ffp-contract=fast -std=gnu17 mru_branch.c -o mrub -lm
 * Run:   taskset -c <CORE> ./mrub <iters>
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <arm_neon.h>
#include <arm_acle.h>

#define N 256
#define BLK 8

static double A0[N*N] __attribute__((aligned(64)));
static double B0[N*N] __attribute__((aligned(64)));
static double golden[BLK] __attribute__((aligned(64)));

static uint32_t rng = 12345;
static double frand(void){ rng = rng*1103515245u + 12345u; return ((rng>>8)&0xffffff)/(double)0x1000000; }
static void build(void){ for(int i=0;i<N*N;i++){ A0[i]=frand()-0.5; B0[i]=frand()-0.5; } }

/* 8 live 128-bit FMA accumulators + data-dependent branches.
 * Branch outcome depends on a computed FMA value (unpredictable to the
 * branch predictor, mimicking Eigen's sparse-pattern-dependent control flow). */
static void compute(double out[BLK]){
    float64x2_t acc[BLK];
    for(int b=0;b<BLK;b++) acc[b] = vdupq_n_f64(0.0);
    for(int i=0;i<N;i++){
        float64x2_t a = vld1q_f64(&A0[i*N]);
        float64x2_t bvec = vld1q_f64(&B0[i*N]);
        for(int b=0;b<BLK;b++){
            float64x2_t k = vdupq_n_f64((double)(b+1)*0.1);
            acc[b] = vfmaq_f64(acc[b], a, vmulq_f64(bvec, k));
            /* data-dependent branch on a computed value -- unpredictable */
            double tmp[2]; vst1q_f64(tmp, acc[b]);
            if(tmp[0] > tmp[1]){       /* branch on live FMA result */
                acc[b] = vaddq_f64(acc[b], vld1q_f64(&A0[(i+1<N? i+1:i)*N]));
            } else {
                acc[b] = vsubq_f64(acc[b], vld1q_f64(&B0[(i+1<N? i+1:i)*N]));
            }
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
    fprintf(stderr,"pure-C branch-MRU: golden crc=0x%08x out[0]=%.17g\n",gcrc,golden[0]);
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
    fprintf(stderr,"RESULT pure-C branch-MRU: %d/%d fails\n",fail,iters);
    printf("%d\n",fail);
    return fail>0;
}

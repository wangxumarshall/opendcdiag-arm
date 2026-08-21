/* Pure-C Minimal Reproducer Unit (MRU) for core 179 SDC -- instruction-exact.
 * NO library dependencies except libc (+libm). NO Eigen. NO external headers
 * (arm_neon.h/arm_acle.h ship with compiler).
 *
 * This MRU does NOT try to reproduce Eigen's algorithm. It reproduces the
 * EXACT instruction sequence of Eigen's factorize_preordered rank-1 update
 * hot loop (extracted via objdump from esdiag_phase2):
 *
 *   loop:
 *     ldrsw x1, [x22, x0, lsl #2]    ; Li[p] = index (indirect, shifted)
 *     ldr  d2, [x20, x0, lsl #3]     ; Lx[p] = value (indirect, shifted)
 *     add  x0, x0, #1
 *     ldr  d1, [x19, x1, lsl #3]     ; y[Li[p]] (indirect via index)
 *     fmsub d1, d2, d3, d1           ; y[idx] -= Lx[p] * yi  (SCALAR FMA)
 *     str  d1, [x19, x1, lsl #3]     ; store y[Li[p]]
 *     cmp  x2, x0
 *     b.ne loop
 *
 * The trigger factor (per disasm): SCALAR FMA (fmsub d) + indirect indexed
 * load/store (lsl #2/#3 shifted register addressing) in a tight loop. This is
 * wrapped with the data-dependent branches + pattern chase that Eigen has.
 *
 * Build: gcc -O2 -march=armv8.1-a+crc+crypto -ffp-contract=fast -std=gnu17 mru_asm.c -o mruasm -lm
 * Run:   taskset -c <CORE> ./mruasm <iters>
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <arm_acle.h>

#define N 256
#define MAXNNZ (N*16)

/* CSC sparse arrays mimicking Eigen layout */
static int    Li[MAXNNZ] __attribute__((aligned(64)));
static double Lx[MAXNNZ] __attribute__((aligned(64)));
static double y_work[N] __attribute__((aligned(64)));
static double D[N] __attribute__((aligned(64)));
static int    pattern[N] __attribute__((aligned(64)));
static int    tags[N] __attribute__((aligned(64)));
static int    nonZerosPerCol[N] __attribute__((aligned(64)));

static uint32_t rng = 12345;
static double frand(void){ rng = rng*1103515245u + 12345u; return ((rng>>8)&0xffffff)/(double)0x1000000; }

static void init_arrays(void){
    rng = 12345;  /* reset to fixed seed each call -> deterministic */
    for(int i=0;i<MAXNNZ;i++){ Li[i]=i%N; Lx[i]=frand()-0.5; }
    for(int i=0;i<N;i++){ y_work[i]=frand()-0.5; D[i]=frand()+1.0; pattern[i]=i; tags[i]=-1; nonZerosPerCol[i]=0; }
}

/* The exact Eigen hot-loop instruction sequence, via inline asm.
 * Performs: for p in [Lp[i]..Lp[i]+count): y[Li[p]] -= Lx[p] * yi
 * using the EXACT fmsub + indirect ldr/str pattern. */
static inline void rank1_update_eigenstyle(int *lip, double *lxp, double *yp, int count, double yi){
    /* yp[Li[p]] -= Lx[p] * yi  for p=0..count-1 */
    /* Force the fmsub + indirect-indexed ldr/str via asm matching Eigen disasm.
     * x0=p (loop index), x1=Li[p], d2=Lx[p], d1=y[Li[p]], d3=yi
     */
    long p = 0;
    long cnt = count;
    __asm__ volatile (
        "1:\n"
        "ldrsw x1, [%[lip], %x[p], lsl #2]\n"     /* x1 = Li[p] (indirect index) */
        "ldr  d2, [%[lxp], %x[p], lsl #3]\n"      /* d2 = Lx[p] */
        "add  %x[p], %x[p], #1\n"
        "ldr  d1, [%[yp], x1, lsl #3]\n"          /* d1 = y[Li[p]] (indirect via x1) */
        "fmsub d1, d2, %d[yi], d1\n"              /* d1 = d1 - d2*yi  (FMA, Eigen-exact) */
        "str  d1, [%[yp], x1, lsl #3]\n"          /* y[Li[p]] = d1 */
        "cmp  %x[cnt], %x[p]\n"
        "b.ne 1b\n"
        : [p] "+r"(p)
        : [lip] "r"(lip), [lxp] "r"(lxp), [yp] "r"(yp), [cnt] "r"(cnt), [yi] "w"(yi)
        : "x1", "d0", "d1", "d2", "memory", "cc"
    );
}

/* A numeric-factorize-like driver that exercises the exact hot loop
 * repeatedly with data-dependent branch patterns + pattern chase, to build
 * sufficient execution-window pressure like Eigen's factorize_preordered. */
static void compute(int iters){
    for(int k=0;k<iters;k++){
        for(int col=0; col<N; col++){
            /* data-dependent pattern: which indices to update */
            int base = (col * 7) % MAXNNZ;
            int cnt = (col % 31) + 5;   /* variable count, unpredictable branch */
            if(cnt + base >= MAXNNZ) cnt = MAXNNZ - base - 1;
            double yi = y_work[col];
            /* the exact Eigen-style rank-1 update hot loop */
            rank1_update_eigenstyle(&Li[base], &Lx[base], y_work, cnt, yi);
            /* cdiv + accumulate diagonal (mimics Eigen cdiv path) */
            double d = y_work[col];
            int top = N;
            /* pattern chase with tags (mimics Eigen symbolic) */
            for(int p=base; p<base+cnt; p++){
                int idx = Li[p];
                if(idx<N && tags[idx]!=col){
                    pattern[--top]=idx;
                    tags[idx]=col;
                    /* branch on computed value (unpredictable) */
                    if(d > y_work[idx]*0.5) d -= y_work[idx];
                    else d += y_work[idx]*0.3;
                }
            }
            if(d != 0.0 && col<N) D[col]=d;
        }
    }
}

static uint32_t crc_D(void){
    uint32_t c=0xffffffffu; const uint8_t*p=(const uint8_t*)D;
    for(int i=0;i<N*8;i++) c=__crc32cb(c,p[i]);
    return ~c;
}

int main(int argc,char**argv){
    int iters=argc>1?atoi(argv[1]):1500;
    init_arrays();
    compute(1);
    uint32_t gcrc=crc_D();
    fprintf(stderr,"pure-C asm-MRU: golden D-crc=0x%08x D[0]=%.17g\n",gcrc,D[0]);
    int fail=0;
    for(int k=0;k<iters;k++){
        init_arrays();  /* reset to same state for determinism */
        compute(1);
        uint32_t c=crc_D();
        if(c!=gcrc){
            fail++;
            if(fail<=5) fprintf(stderr,"k=%d crc mismatch D[0]=%.6g\n",k,D[0]);
        }
    }
    fprintf(stderr,"RESULT pure-C asm-MRU: %d/%d fails\n",fail,iters);
    printf("%d\n",fail);
    return fail>0;
}

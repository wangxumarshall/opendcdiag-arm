/* Pure-C Minimal Reproducer Unit (MRU) for core 179 SDC.
 * NO library dependencies except libc (no Eigen, no math lib beyond sqrt from libm).
 * Reconstructs the instruction-sequence profile of Eigen's numeric Cholesky
 * factorize_preordered() (SparseCholesky_impl.h:300):
 *   - outer loop over columns k
 *   - symbolic pattern via indirect pointer-chase (tags/parent)
 *   - sparse triangular solve: gather y[i], cdiv yi/diag
 *   - rank-1 update: y[Li[p]] -= Lx[p] * yi  (FMA + indirect scatter)
 *   - conditional branches on sparsity pattern (if i<=k, etc.)
 * This reproduces the *microarchitectural instruction sequence* that triggers
 * the OoO-engine state leak on core 179, not the math equivalence.
 *
 * Build: gcc -O2 -march=armv8.1-a+crc+crypto -std=gnu17 mru_pure_c.c -o mru -lm
 * Run:   taskset -c <CORE> ./mru <iters>   (under same-socket full load on core 179)
 * Expected on core 179 under load: elem[0] corruption, multi-bit aliasing.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <arm_acle.h>

#define N 256

/* sparse symmetric matrix in CSC-like lower triangle (mimics Eigen's input).
 * We build a fixed sparsity pattern + values so the run is deterministic
 * modulo the HW fault. */
static int    Lp[N+1];     /* column pointers (outerIndexPtr) */
static int    Li[N*16];     /* row indices (innerIndexPtr) */
static double Lx[N*16];     /* values (valuePtr) */
static int    m_parent[N]; /* elimination tree */
static double m_diag[N];   /* LDL diagonal */
static double A_vals[N*16]; /* input A values */
static int    A_Lp[N+1], A_Li[N*16];

static uint32_t rng_state = 12345;
static double frand(void){ rng_state = rng_state*1103515245u + 12345u;
    return ((rng_state>>8)&0xffffff) / (double)0x1000000; }

/* Build a sparse symmetric matrix (lower triangle CSC) with random sparsity,
 * mimicking opendcdiag eigen_sparse's A (n=256, ~10% density). */
static void build_matrix(void){
    int nnz = 0;
    A_Lp[0] = 0;
    for(int j=0;j<N;j++){
        A_Li[nnz] = j; A_vals[nnz] = fabs(frand())+0.05; nnz++; /* diagonal */
        for(int i=j+1;i<N;i++){
            if(frand() < 0.1){ /* ~10% off-diagonal */
                A_Li[nnz] = i; A_vals[nnz] = frand(); nnz++;
            }
        }
        A_Lp[j+1] = nnz;
    }
    /* build elimination tree (simplified: parent[i]=i+1) */
    for(int i=0;i<N;i++) m_parent[i] = (i+1<N)? i+1 : i;
}

/* Symbolic analysis: compute the column structure of L (pattern).
 * Mimics analyzePattern_preordered -- sets up Lp/Li layout. Done ONCE. */
static int  pattern_count[N];
static void analyze_pattern(void){
    int nnz = 0;
    Lp[0] = 0;
    for(int j=0;j<N;j++){
        pattern_count[j] = 0;
        for(int p=A_Lp[j]; p<A_Lp[j+1]; p++){
            int i = A_Li[p];
            if(i >= j){ Li[nnz] = i; nnz++; pattern_count[j]++; }
        }
        Lp[j+1] = nnz;
        /* initialize m_diag from A's diagonal so the first numeric factorize
         * has valid divisors (mimics Eigen storing D(k,k) before solve). */
        m_diag[j] = A_vals[A_Lp[j]];  /* A_Lp[j] points at the diagonal entry */
    }
}

/* The numeric factorize: the ACTUAL trigger. Reconstructs the instruction
 * sequence of factorize_preordered(). */
static double y_work[N];
static int    tags[N];
static int    pattern_list[N];   /* pattern of L(:,k) */
static void numeric_factorize(void){
    int nonZerosPerCol[N];
    for(int i=0;i<N;i++){ nonZerosPerCol[i]=0; }
    for(int k=0; k<N; k++){
        /* symbolic pattern: collect rows i>=k from A(:,k) -- simplified
         * pattern (the pointer-chase variant lives in the Eigen original;
         * here we reproduce the same scatter + branch density). */
        y_work[k] = 0.0;
        tags[k] = k;
        int npat = 0;
        for(int p=A_Lp[k]; p<A_Lp[k+1]; p++){
            int i = A_Li[p];
            if(i >= k){
                y_work[i] += A_vals[p];        /* scatter A(i,k) */
                if(tags[i] != k){ pattern_list[npat++] = i; tags[i] = k; }
            }
        }
        /* sparse triangular solve + rank-1 update over pattern */
        double d = y_work[k];                  /* shift identity */
        y_work[k] = 0.0;
        for(int t=0; t<npat; t++){
            int i = pattern_list[t];
            if(i==k) continue;
            double yi = y_work[i];
            y_work[i] = 0.0;
            double l_ki = (m_diag[i] != 0.0) ? yi / m_diag[i] : 0.0;  /* cdiv */
            /* rank-1 update: y[Li[p]] -= Lx[p] * yi (FMA + indirect scatter) */
            int p2 = Lp[i] + nonZerosPerCol[i];
            for(int p = Lp[i]; p < p2 && p < (int)(sizeof(Li)/sizeof(Li[0])); p++){
                int idx = Li[p];
                if(idx>=0 && idx<N) y_work[idx] -= Lx[p] * yi;
            }
            d -= l_ki * yi;
            if(p2 >= 0 && p2 < (int)(sizeof(Li)/sizeof(Li[0]))){
                Li[p2] = k; Lx[p2] = l_ki;
                nonZerosPerCol[i]++;
            }
        }
        m_diag[k] = d;
    }
}

static uint32_t crc_x0(double v){
    uint32_t c = 0xffffffffu;
    uint8_t *p = (uint8_t*)&v;
    for(int i=0;i<8;i++) c = __crc32cb(c, p[i]);
    return ~c;
}

int main(int argc, char**argv){
    int iters = argc>1 ? atoi(argv[1]) : 1500;
    build_matrix();
    analyze_pattern();
    /* golden: run numeric factorize once to get the baseline elem[0] (diag[0]) */
    numeric_factorize();
    double golden0 = m_diag[0];
    uint32_t gcrc = crc_x0(golden0);
    fprintf(stderr, "pure-C MRU: golden m_diag[0]=%.17g crc=0x%08x\n", golden0, gcrc);
    int fail = 0;
    for(int k=0; k<iters; k++){
        numeric_factorize();      /* the trigger */
        uint32_t c = crc_x0(m_diag[0]);
        if(c != gcrc){
            fail++;
            if(fail<=5){
                uint64_t a,b; memcpy(&a,&m_diag[0],8); memcpy(&b,&golden0,8);
                fprintf(stderr,"k=%d diag[0]=%.6g bits=0x%016lx xor=0x%016lx pc=%d\n",
                    k, m_diag[0], a, a^b, __builtin_popcountll(a^b));
            }
        }
    }
    fprintf(stderr, "RESULT pure-C MRU: %d/%d fails (elem[0]=m_diag[0])\n", fail, iters);
    printf("%d\n", fail);
    return fail>0;
}

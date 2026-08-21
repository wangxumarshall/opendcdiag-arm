/* Pure-C Minimal Reproducer Unit (MRU) for core 179 SDC -- sparse LDL^T.
 * NO library dependencies except libc (+libm). No Eigen.
 *
 * Reconstructs the instruction-sequence profile of Eigen's numeric
 * factorize_preordered() for SPARSE Cholesky (SimplicialCholesky_impl.h:300):
 *   - CSC sparse format (outerIndexPtr/innerIndexPtr/valuePtr)
 *   - elimination-tree symbolic pattern with pointer-chase (tags/parent)
 *   - sparse triangular solve via gather of y[i]
 *   - cdiv: L(k,i) = yi / D[i]
 *   - sparse rank-1 update: y[Li[p]] -= Lx[p] * yi  (FMA + indirect scatter)
 *   - branch on sparsity pattern (i<=k, tags[i]!=k)
 *
 * The dense LDL^T (mru_dense.c) does NOT trigger the fault; the sparse
 * pattern's indirect addressing combined with cdiv+FMA is required.
 *
 * Build: gcc -O2 -march=armv8.1-a+crc+crypto -std=gnu17 mru_sparse.c -o mrus -lm
 * Run:   taskset -c <CORE> ./mrus <iters>
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <arm_acle.h>

#define N 256
#define MAXNNZ (N*16)

/* CSC sparse lower-triangular storage (mimics Eigen's m_matrix) */
static int    Lp[N+1], Li[MAXNNZ];
static double Lx[MAXNNZ];
/* input A (CSC, full symmetric lower) */
static int    Ap[N+1], Ai[MAXNNZ];
static double Ax[MAXNNZ];
/* elimination tree + workspace */
static int    parent[N], nonZerosPerCol[N], pattern[N], tags[N];
static double y[N], D[N];

static uint32_t rng = 12345;
static double frand(void){ rng = rng*1103515245u + 12345u; return ((rng>>8)&0xffffff)/(double)0x1000000; }

/* Build a sparse symmetric SPD matrix in CSC (lower triangle).
 * Strictly diagonally dominant => SPD. ~10% off-diagonal density like eigen_sparse. */
static void build_matrix(void){
    int nz = 0;
    Ap[0] = 0;
    for(int j=0;j<N;j++){
        Ai[nz] = j; Ax[nz] = 0; nz++;  /* diagonal placeholder */
        for(int i=j+1;i<N;i++){
            if(frand() < 0.1){ Ai[nz]=i; Ax[nz]=frand()-0.5; nz++; }
        }
        Ap[j+1] = nz;
    }
    /* set diagonals for strict diagonal dominance */
    for(int j=0;j<N;j++){
        double s=0;
        for(int p=Ap[j];p<Ap[j+1];p++){ if(Ai[p]!=j) s += fabs(Ax[p]); }
        Ax[Ap[j]] = s + 2.0;  /* diagonal at Ap[j] */
    }
}

/* symbolic analysis: build elimination tree + L's column pointers. ONCE. */
static void analyze_pattern(void){
    /* simplified elimination tree: parent[j] = smallest i>j with A(i,j) nonzero, else j */
    for(int j=0;j<N;j++) parent[j]=j;
    for(int j=0;j<N;j++){
        for(int p=Ap[j];p<Ap[j+1];p++){
            int i=Ai[p];
            if(i>j){ if(parent[j]==j || i<parent[j]) parent[j]=i; }
        }
    }
    /* L has same sparsity as A's lower triangle (simplified; real Eigen does
     * fill-in via the tree, but for diag-dominant sparse this is close) */
    int nz=0; Lp[0]=0;
    for(int j=0;j<N;j++){
        for(int p=Ap[j];p<Ap[j+1];p++){ Li[nz]=Ai[p]; Lx[nz]=0; nz++; }
        Lp[j+1]=nz;
    }
}

/* numeric factorize: the trigger. Mirrors factorize_preordered(). */
static int factorize(void){
    for(int i=0;i<N;i++){ nonZerosPerCol[i]=0; y[i]=0; tags[i]=-1; }
    for(int k=0;k<N;k++){
        /* symbolic pattern of column k (topological, via parent chase) */
        y[k]=0; tags[k]=k; int top=N; int npat=0;
        for(int p=Ap[k];p<Ap[k+1];p++){
            int i=Ai[p];
            if(i<=k){ /* scatter A(i,k) into y */
                y[i] += Ax[p];
                int len=0;
                while(tags[i]!=k){ pattern[top-1-len]=i; /* mimic Eigen stack push */
                    i=parent[i]; tags[i]=k; len++; }
                /* shift pattern to top of stack */
                for(int t=0;t<len;t++){ pattern[--top]=pattern[top-len+t]; }
                (void)npat;
            }
        }
        /* sparse triangular solve + rank-1 update over pattern[top..N-1] */
        double d = y[k]; y[k]=0;
        for(int t=top;t<N;t++){
            int i=pattern[t];
            double yi = y[i]; y[i]=0;
            double l_ki = (D[i]!=0.0) ? yi/D[i] : 0.0;  /* cdiv */
            int p2 = Lp[i] + nonZerosPerCol[i];
            for(int p=Lp[i];p<p2;p++){ /* rank-1 update: FMA + indirect scatter */
                int idx=Li[p];
                y[idx] -= Lx[p]*yi;
            }
            d -= l_ki*yi;
            if(p2<MAXNNZ){ Li[p2]=k; Lx[p2]=l_ki; nonZerosPerCol[i]++; }
        }
        if(d<=0) return 0;  /* not SPD (shouldn't happen) */
        D[k]=d;
    }
    return 1;
}

static uint32_t crc_D(void){
    uint32_t c=0xffffffffu; const uint8_t*p=(const uint8_t*)D;
    for(int i=0;i<N*8;i++) c=__crc32cb(c,p[i]);
    return ~c;
}

int main(int argc,char**argv){
    int iters = argc>1?atoi(argv[1]):1500;
    build_matrix();
    analyze_pattern();
    if(!factorize()){ fprintf(stderr,"SPD fail\n"); return 2; }
    uint32_t gcrc = crc_D();
    fprintf(stderr,"pure-C sparse LDL^T MRU: golden D-crc=0x%08x D[0]=%.17g\n", gcrc, D[0]);
    int fail=0;
    for(int k=0;k<iters;k++){
        if(!factorize()){ fail++; continue; }
        uint32_t c=crc_D();
        if(c!=gcrc){
            fail++;
            if(fail<=5) fprintf(stderr,"k=%d D-crc mismatch (0x%08x vs 0x%08x) D[0]=%.6g\n",k,c,gcrc,D[0]);
        }
    }
    fprintf(stderr,"RESULT pure-C sparse MRU: %d/%d fails\n",fail,iters);
    printf("%d\n",fail);
    return fail>0;
}

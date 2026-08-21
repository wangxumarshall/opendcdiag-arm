/* Pure-C Minimal Reproducer Unit (MRU) for core 179 SDC -- cold-L1D-malloc variant.
 * NO library dependencies except libc (+libm). NO Eigen. NO external headers
 * (arm_neon.h/arm_acle.h ship with compiler, not used here to stay pure-C).
 *
 * HYPOTHESIS (from 2026-08-07 differential perf): the differentiator between
 * triggering Eigen (esphase2: 12/3000 fail, 4.05x l1d_cache_refill/insn) and
 * non-triggering pure-C (mru_full_sparse: 0/3000 fail) is that Eigen
 * MALLOC/FREEs a fresh y[] workspace EACH factorize iteration (confirmed in
 * disasm: factorize_preordered calls free@plt then malloc@plt, with the y
 * pointer in x19 / sub sp,sp,x0 VLA). A freshly malloc'd buffer is
 * L1D-cold each iteration -> the 4x l1d_cache_refill rate. Prior pure-C MRUs
 * all reused a static y[] array (L1D-hot) and thus never reproduced.
 *
 * This probe matches Eigen's signature: per-iteration malloc of y[], scattered
 * indirect gather/scatter y[Li[p]] -= Lx[p]*yi via scalar fmsub, same N=256,
 * same ~10% sparsity, self-checked via CRC of D (diagonal).
 *
 * Build: gcc -O2 -march=armv8.1-a+crc+crypto -ffp-contract=fast -std=gnu17 mru_coldl1d.c -o mrucold -lm
 * Run:   taskset -c <CORE> ./mrucold <iters>
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <arm_acle.h>

#define N 256
#define MAXNNZ (N*128)
#define kEmpty (-1)

/* ===== input A: CSC lower triangle (mimics Eigen SparseMatrix) ===== */
static int    Ap[N+1], Ai[MAXNNZ];
static double Ax[MAXNNZ];

/* ===== L factor: CSC ===== */
static int    Lp[N+1], Li[MAXNNZ];
static double Lx[MAXNNZ];
static int    Lnz[N];   /* nonzeros per column of L */

/* ===== elimination tree & symbolic workspace ===== */
static int    parent[N], firstChild[N], firstSibling[N], post[N], dfs_buf[N];
static int    ancestor_uf[N];           /* union-find ancestor */
static int    tags[N];
static int    pattern[N];               /* pattern stack (top..N-1) */
static double D[N];                      /* diagonal of LDL^T */

static uint32_t rng = 12345;
static double frand(void){ rng = rng*1103515245u + 12345u; return ((rng>>8)&0xffffff)/(double)0x1000000; }

/* ===== build sparse symmetric SPD matrix (strictly diag-dominant) ===== */
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
}

/* ===== union-find for elimination tree (Eigen's DisjointSet) ===== */
static int uf_find(int i){
    int root = i;
    while(ancestor_uf[root] != kEmpty && ancestor_uf[root] != root) root = ancestor_uf[root];
    int x = i;
    while(ancestor_uf[x] != kEmpty && ancestor_uf[x] != root){ int nx = ancestor_uf[x]; ancestor_uf[x] = root; x = nx; }
    return root;
}
static void uf_union(int i, int j){
    int r = uf_find(i);
    if(r != j) ancestor_uf[r] = j;
}

static void calc_etree(void){
    for(int i=0;i<N;i++){ parent[i]=kEmpty; ancestor_uf[i]=kEmpty; }
    for(int j=1;j<N;j++){
        for(int p=Ap[j];p<Ap[j+1];p++){
            int i=Ai[p];
            if(i<j){
                int r=uf_find(i);
                if(r!=j) parent[r]=j;
                uf_union(i,j);
            }
        }
    }
}

static void calc_lineage(void){
    for(int i=0;i<N;i++){ firstChild[i]=kEmpty; firstSibling[i]=kEmpty; }
    for(int j=0;j<N;j++){
        int p=parent[j];
        if(p==kEmpty) continue;
        int c=firstChild[p];
        if(c==kEmpty) firstChild[p]=j;
        else{ while(firstSibling[c]!=kEmpty) c=firstSibling[c]; firstSibling[c]=j; }
    }
}
static void calc_post(void){
    int pp=0, ds=0;
    for(int j=0;j<N;j++){
        if(parent[j]!=kEmpty) continue;
        ds=0; dfs_buf[ds++]=j;
        while(ds>0){
            int i=dfs_buf[ds-1]; int c=firstChild[i];
            if(c==kEmpty){ post[pp++]=i; ds--; }
            else{ dfs_buf[ds++]=c; firstChild[i]=firstSibling[c]; }
        }
    }
}

static int  Lcol_count[N];
static void analyze_pattern(void){
    int nz=0; Lp[0]=0;
    for(int j=0;j<N;j++) Lcol_count[j]=0;
    for(int k=0;k<N;k++){
        for(int i=0;i<N;i++) tags[i]=kEmpty;
        tags[k]=k;
        int top=N;
        for(int p=Ap[k];p<Ap[k+1];p++){
            int i=Ai[p];
            if(i<=k){
                while(tags[i]!=k){ pattern[--top]=i; tags[i]=k; i=parent[i]; if(i==kEmpty) break; }
            }
        }
        Lcol_count[k] = (N-top);
        for(int t=top;t<N;t++){ Li[nz]=pattern[t]; Lx[nz]=0; nz++; }
        Lp[k+1]=nz;
    }
}

/* ===== numeric factorize with FRESH MALLOC'd y[] each call (Eigen-like) ===== */
/* The hot inner loop matches Eigen's rank-1 update (fmsub + indirect ldr/str):
 *   y[Li[p]] -= Lx[p] * yi
 * Compiled with -ffp-contract=fast -> fmsub d, the exact Eigen instruction. */
static int factorize_cold(void){
    /* FRESH workspace each call -- this is the hypothesized trigger:
     * a freshly malloc'd buffer is L1D-cold, matching Eigen's 4x refill rate. */
    double *y = (double*)malloc(sizeof(double)*N);
    if(!y) return 0;
    for(int i=0;i<N;i++){ Lnz[i]=0; y[i]=0; tags[i]=-1; }
    for(int k=0;k<N;k++){
        y[k]=0; tags[k]=k; int top=N;
        for(int p=Ap[k];p<Ap[k+1];p++){
            int i=Ai[p];
            if(i<=k){
                y[i] += Ax[p];                    /* scatter A(i,k) */
                while(tags[i]!=k){
                    pattern[--top]=i;
                    tags[i]=k;
                    i=parent[i];
                    if(i==kEmpty) break;
                }
            }
        }
        /* sparse triangular solve + rank-1 update (the Eigen hot loop) */
        double d = y[k]; y[k]=0;
        for(int t=top;t<N;t++){
            int i=pattern[t];
            double yi = y[i]; y[i]=0;
            double l_ki = (D[i]!=0.0) ? yi/D[i] : 0.0;   /* cdiv (fdiv) */
            int p2 = Lp[i] + Lnz[i];
            /* rank-1 update: y[Li[p]] -= Lx[p]*yi  (FMA + indirect gather/scatter) */
            for(int p=Lp[i];p<p2;p++){
                int idx=Li[p];
                y[idx] -= Lx[p]*yi;
            }
            d -= l_ki*yi;
            if(p2<MAXNNZ){ Li[p2]=k; Lx[p2]=l_ki; Lnz[i]++; }
        }
        if(d<=0.0){ free(y); return 0; }
        D[k]=d;
    }
    free(y);
    return 1;
}

static uint32_t crc_D(void){ uint32_t c=0xffffffffu; const uint8_t*p=(const uint8_t*)D; for(int i=0;i<N*8;i++) c=__crc32cb(c,p[i]); return ~c; }

int main(int argc,char**argv){
    int iters=argc>1?atoi(argv[1]):1500;
    build_matrix();
    calc_etree();
    calc_lineage();
    calc_post();
    analyze_pattern();
    if(!factorize_cold()){ fprintf(stderr,"SPD fail (initial)\n"); return 2; }
    uint32_t gcrc=crc_D();
    fprintf(stderr,"pure-C cold-L1D MRU: golden D-crc=0x%08x D[0]=%.17g\n",gcrc,D[0]);
    int fail=0;
    for(int k=0;k<iters;k++){
        if(!factorize_cold()){ fail++; continue; }
        uint32_t c=crc_D();
        if(c!=gcrc){ fail++; if(fail<=5) fprintf(stderr,"k=%d D-crc mismatch (0x%08x vs 0x%08x) D[0]=%.6g\n",k,c,gcrc,D[0]); }
    }
    fprintf(stderr,"RESULT pure-C cold-L1D MRU: %d/%d fails\n",fail,iters);
    printf("%d\n",fail);
    return fail>0;
}

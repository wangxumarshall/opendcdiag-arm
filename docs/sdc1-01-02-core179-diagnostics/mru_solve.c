/* Pure-C MRU for core 179 SDC -- WITH solve phase, checks x (the actual corrupted output).
 * NO library dependencies except libc (+libm). NO Eigen.
 *
 * PRIOR FAILURE ANALYSIS (2026-08-07): all prior pure-C MRUs checked only D (the
 * factorize diagonal). But the actual corruption (per esdiag3/esdiag_phase2) is in
 * x = solve(b) -- the triangular-solve OUTPUT, elem[0]. esdiag_phase2 mode 1 calls
 * s.factorize(A) THEN s.solve(b) and CRCs x. My prior probes never ran solve().
 * This probe mirrors esdiag_phase2 mode 1 exactly: factorize + solve, check x.
 *
 * Also matches Eigen's per-iteration malloc of workspace (cold L1D) and the
 * long-lived-d4-accumulator-across-indirect-loop structure.
 *
 * Build: gcc -O2 -march=armv8.1-a+crc+crypto -ffp-contract=fast -std=gnu17 mru_solve.c -o mrusolv -lm
 * Run:   taskset -c <CORE> ./mrusolv <iters>
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <arm_acle.h>

#define N 256
#define MAXNNZ (N*128)
#define kEmpty (-1)

/* ===== input A: CSC lower triangle ===== */
static int    Ap[N+1], Ai[MAXNNZ];
static double Ax[MAXNNZ];

/* ===== L factor: CSC ===== */
static int    Lp[N+1], Li[MAXNNZ];
static double Lx[MAXNNZ];
static int    Lnz[N];

/* ===== etree & symbolic ===== */
static int    parent[N], firstChild[N], firstSibling[N], post[N], dfs_buf[N];
static int    ancestor_uf[N];
static int    tags[N];
static int    pattern[N];
static double D[N];

static uint32_t rng = 12345;
static double frand(void){ rng = rng*1103515245u + 12345u; return ((rng>>8)&0xffffff)/(double)0x1000000; }

static void build_matrix(void){
    int nz = 0; Ap[0] = 0;
    for(int j=0;j<N;j++){
        Ai[nz] = j; Ax[nz] = 0; nz++;
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

static int uf_find(int i){
    int root = i;
    while(ancestor_uf[root] != kEmpty && ancestor_uf[root] != root) root = ancestor_uf[root];
    int x = i;
    while(ancestor_uf[x] != kEmpty && ancestor_uf[x] != root){ int nx = ancestor_uf[x]; ancestor_uf[x] = root; x = nx; }
    return root;
}
static void uf_union(int i, int j){ int r = uf_find(i); if(r != j) ancestor_uf[r] = j; }

static void calc_etree(void){
    for(int i=0;i<N;i++){ parent[i]=kEmpty; ancestor_uf[i]=kEmpty; }
    for(int j=1;j<N;j++){
        for(int p=Ap[j];p<Ap[j+1];p++){
            int i=Ai[p];
            if(i<j){ int r=uf_find(i); if(r!=j) parent[r]=j; uf_union(i,j); }
        }
    }
}
static void calc_lineage(void){
    for(int i=0;i<N;i++){ firstChild[i]=kEmpty; firstSibling[i]=kEmpty; }
    for(int j=0;j<N;j++){
        int p=parent[j]; if(p==kEmpty) continue;
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
        tags[k]=k; int top=N;
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
static int factorize_cold(void){
    double *y = (double*)malloc(sizeof(double)*N);
    if(!y) return 0;
    for(int i=0;i<N;i++){ Lnz[i]=0; y[i]=0; tags[i]=-1; }
    for(int k=0;k<N;k++){
        y[k]=0; tags[k]=k; int top=N;
        for(int p=Ap[k];p<Ap[k+1];p++){
            int i=Ai[p];
            if(i<=k){
                y[i] += Ax[p];
                while(tags[i]!=k){ pattern[--top]=i; tags[i]=k; i=parent[i]; if(i==kEmpty) break; }
            }
        }
        double d = y[k]; y[k]=0;
        for(int t=top;t<N;t++){
            int i=pattern[t];
            double yi = y[i]; y[i]=0;
            double l_ki = (D[i]!=0.0) ? yi/D[i] : 0.0;
            int p2 = Lp[i] + Lnz[i];
            for(int p=Lp[i];p<p2;p++){
                int idx=Li[p];
                y[idx] -= Lx[p]*yi;   /* rank-1 update: FMA + indirect */
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

/* ===== solve: x = D^{-1} L^{-1} b  (LDL^T: L lower-unit, D diagonal) ===== */
/* Forward: y = L^{-1} b  -> y[k] = b[k] - sum_{j<k, L[k,j]} L[k,j]*y[j]... but L is CSC
 * (columns). Eigen stores L lower-triangular column-wise. Forward sub accesses L by
 * column. We do: for k=0..N-1: y[k]=b[k]; then for each k, for j in pattern: y[k]-=L*.
 * Simpler: use the column-wise L. Forward sub (L y = b, L unit-lower):
 *   for k=0..N-1: y[k] = b[k]; for p in L col k: y[Li[p]] -= Lx[p]*y[k]
 * Diagonal: y[k] /= D[k]
 * Backward (L^T x = y): for k=N-1..0: x[k]=y[k]; for p in L col k: x[k] -= Lx[p]*x[Li[p]]? no.
 * Correct backward for unit-lower L^T: x = L^{-T} y:
 *   for k=N-1..0: for p in col k (rows>k): x[k] -= Lx[p]*x[Li[p]]; then x[k] += y[k]
 * We'll do the standard dense-friendly version using the CSC structure.
static void solve(const double *b, double *x){
 */
static void solve(const double *b, double *x){
    double *y = (double*)malloc(sizeof(double)*N);  /* fresh workspace like Eigen */
    if(!y) return;
    /* forward sub: L y = b, L unit-lower (CSC, column k updates rows Li[p]) */
    for(int k=0;k<N;k++) y[k]=b[k];
    for(int k=0;k<N;k++){
        double yk = y[k];
        int p2 = Lp[k]+Lnz[k];
        for(int p=Lp[k];p<p2;p++){
            y[Li[p]] -= Lx[p]*yk;
        }
    }
    /* diagonal */
    for(int k=0;k<N;k++) y[k] /= D[k];
    /* backward sub: L^T x = y. L^T is unit-upper. x[k] = y[k] - sum_{j>k} L[j,k]*x[j]
     * In CSC, L col j contains rows Li[p]>j with values Lx[p]. So L[j,k] for k<j
     * is found by scanning col j. To go k=N-1..0: x[k]=y[k] - sum_{j>k} L[j,k]*x[j].
     * That needs L by row -> transpose access. Eigen does this with a transposed solve.
     * Simpler & correct: build nothing, do x[k]=y[k] then for j=k+1..N-1: if k in col j pattern, x[k]-=L[j,k]*x[j].
     * We iterate cols j and scatter: for j=0..N-1: for p in col j (rows>j): x[Li[p]] contributes back later.
     * Cleanest backward in CSC: x=y copy; for k=N-1..0: x[k] stays; then subtract contributions
     * to x[k] from columns j>k where k is in pattern(j). We do forward accumulation:
     */
    for(int k=0;k<N;k++) x[k]=y[k];
    /* backward: for k from N-1 down to 0, the contributions to x[k] come from
     * columns j (j>k) whose row index is k. Scan all cols j>k? O(nnz) total if we
     * iterate col j and update x[rows] in reverse. Standard CSC lower-tri back-solve:
     *   for j=N-1..0: for p in col j: x[Li[p]] -= Lx[p]*x[j]  (but only rows already finalized)
     * Actually since L^T x = y and L^T is upper, we process j=N-1..0: x[j] finalized = y[j] - sum_{p, Li[p]<j}... no.
     * Use the transpose: x[j] = y[j] - sum_{i>j, i in pattern(j as row)} L[i,j]*x[i].
     * In CSC L (col = first index), entry L[i,j] with i>j is at col j, row i=Li[p].
     * So for backward, process j=N-1 down to 0: x[j] -= sum over p in col j of Lx[p]*x[Li[p]]?
     * That uses x[Li[p]] where Li[p]>j which are already computed. YES that's it but
     * the subtraction sign for L^T solve: x = y; for j=N-1..0: for p in col j: x[Li[p]] is
     * the ROW, we want x[j] = y[j] - sum L[i,j] x[i]. So we need col-of-L^T = rows of L.
     * We have L as CSC (cols). L^T as CSC = L as CSR. Do: build L^T indices once? expensive.
     * PRAGMATIC: just do the O(N^2) dense backward using the fact Lx is sparse but
     * accumulate via a row-major pass. Simplest correct: for j=N-1..0, x[j] stays; then
     * for p in col j (rows i=Li[p]>j): x[i] gets reduced? No.
     * Let me just do: x = y; then for j = N-1 downto 0:
     *   sx = x[j]; for p in col j: sx -= Lx[p]*x[Li[p]]   -- WRONG (x[j] is being computed, depends on x[i>i]).
     * Correct (unit-upper solve, U=L^T): for i=N-1..0: x[i] = y[i]; for j>i: x[i]-=U[i,j]x[j].
     * U[i,j]=L[j,i]. For i=N-1..0: x[i]=y[i]; for each col j of L with j>i and row...
     * Build a row-index of L once is cleanest. Do it: */
    /* (handled by the transpose-built loop below) */
    static int Rnext[N], Rcol[MAXNNZ]; static double Rx[MAXNNZ];
    static int Rhead[N];
    for(int i=0;i<N;i++) Rhead[i]=-1;
    int rn=0;
    /* Build CSR (row-major) from CSC L: for each col j, for each entry row i=Li[p]: append to row i */
    /* We need per-row lists. Use Rhead linked list. */
    for(int j=0;j<N;j++){
        int p2=Lp[j]+Lnz[j];
        for(int p=Lp[j];p<p2;p++){
            int i=Li[p];
            Rcol[rn]=j; Rx[rn]=Lx[p]; Rnext[rn]=Rhead[i]; Rhead[i]=rn; rn++;
        }
    }
    /* backward: x = L^{-T} y, L^T = U unit-upper. for i=N-1..0: x[i]=y[i]; for j>i (row i entries): x[i]-=L[j,i]*x[j] */
    for(int i=N-1;i>=0;i--){
        double s=y[i];
        for(int r=Rhead[i];r>=0;r=Rnext[r]){
            int j=Rcol[r];
            s -= Rx[r]*x[j];
        }
        x[i]=s;
    }
    free(y);
}

static uint32_t crc_x(const double*x){ uint32_t c=0xffffffffu; const uint8_t*p=(const uint8_t*)x; for(int i=0;i<N*8;i++) c=__crc32cb(c,p[i]); return ~c; }

int main(int argc,char**argv){
    int iters=argc>1?atoi(argv[1]):1500;
    build_matrix();
    calc_etree();
    calc_lineage();
    calc_post();
    analyze_pattern();
    /* golden: factorize + solve once */
    static double b[N], golden[N];
    rng=99999; for(int i=0;i<N;i++) b[i]=frand()-0.5;
    if(!factorize_cold()){ fprintf(stderr,"SPD fail\n"); return 2; }
    solve(b, golden);
    uint32_t gcrc=crc_x(golden);
    fprintf(stderr,"pure-C solve-MRU: golden x-crc=0x%08x x[0]=%.17g\n",gcrc,golden[0]);
    int fail=0;
    for(int k=0;k<iters;k++){
        if(!factorize_cold()){ fail++; continue; }
        static double x[N];
        solve(b, x);
        uint32_t c=crc_x(x);
        if(c!=gcrc){ fail++; if(fail<=8) fprintf(stderr,"k=%d x-crc mismatch (0x%08x vs 0x%08x) x[0]=%.6g (golden %.6g)\n",k,c,gcrc,x[0],golden[0]); }
    }
    fprintf(stderr,"RESULT pure-C solve-MRU: %d/%d fails\n",fail,iters);
    printf("%d\n",fail);
    return fail>0;
}

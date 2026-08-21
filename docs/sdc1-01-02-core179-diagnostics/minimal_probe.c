/* Minimal indirect-addressing corruption probe (no Eigen dependency).
 * Tests whether sparse-matrix-vector-multiply (SpMV) with indirect indexing
 * ALONE can trigger the same elem[0] aliasing corruption as Eigen Cholesky.
 * If yes -> MRU doesn't need Cholesky; if no -> Cholesky-specific path required.
 *
 * Three variants in one binary, selectable via arg1:
 *   1 = dense dot product (no indirect addressing) - control
 *   2 = SpMV via CSR indirect indexing (indirect addressing, double)
 *   3 = gather/scatter with pointer chase (heavy indirect)
 *
 * Build: gcc -O2 -march=armv8.1-a+crc+crypto -std=gnu17 minimal_probe.c -o mprobe
 * Run:   taskset -c <CORE> ./mprobe <variant> <iters>
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <arm_acle.h>
#define N 256

/* variant 1: dense dot product (baseline, no indirect) */
static int v_dense(int iters){
    static double a[N], b[N];
    for(int i=0;i<N;i++){ a[i]=(double)(i+1)*0.5; b[i]=(double)(i+1)*1.5; }
    double gold=0; for(int i=0;i<N;i++) gold+=a[i]*b[i];
    uint32_t gc=0xffffffffu; uint8_t*p=(uint8_t*)&gold; for(int i=0;i<8;i++) gc=__crc32cb(gc,p[i]); gc=~gc;
    int fail=0;
    for(int k=0;k<iters;k++){
        double s=0; for(int i=0;i<N;i++) s+=a[i]*b[i];
        if(s!=gold) fail++;
    }
    fprintf(stderr,"dense: %d/%d fails\n",fail,iters);
    return fail>0;
}

/* variant 2: SpMV via CSR (indirect indexing) */
static int v_spmv(int iters){
    static double val[N], x[N], y[N];
    static int col[N];
    for(int i=0;i<N;i++){ val[i]=(double)(i+1)*0.7; x[i]=(double)(i+1)*0.3; col[i]=(i*7+3)%N; y[i]=0; }
    /* golden y computed once */
    for(int i=0;i<N;i++) y[i]=val[i]*x[col[i]];
    uint32_t gc=0xffffffffu; uint8_t*p=(uint8_t*)y; for(int i=0;i<N*8;i++) gc=__crc32cb(gc,p[i]); gc=~gc;
    int fail=0;
    for(int k=0;k<iters;k++){
        for(int i=0;i<N;i++) y[i]=val[i]*x[col[i]];
        uint32_t c=0xffffffffu; p=(uint8_t*)y; for(int i=0;i<N*8;i++) c=__crc32cb(c,p[i]); c=~c;
        if(c!=gc) fail++;
    }
    fprintf(stderr,"spmv: %d/%d fails\n",fail,iters);
    return fail>0;
}

/* variant 3: pointer-chase gather (heavy indirect, mimics Cholesky elimination tree walk) */
static int v_gather(int iters){
    static double src[N], dst[N];
    static int next[N];
    for(int i=0;i<N;i++){ src[i]=(double)(i+1)*1.1; next[i]=(i*11+5)%N; }
    /* golden: walk linked structure, accumulate into dst[0] */
    double gold=0; int idx=0; for(int i=0;i<N;i++){ gold+=src[idx]; idx=next[idx]; }
    uint8_t*p=(uint8_t*)&gold; uint32_t gc=0xffffffffu; for(int i=0;i<8;i++) gc=__crc32cb(gc,p[i]); gc=~gc;
    int fail=0;
    for(int k=0;k<iters;k++){
        double s=0; int idx2=0; for(int i=0;i<N;i++){ s+=src[idx2]; idx2=next[idx2]; }
        uint8_t*q=(uint8_t*)&s; uint32_t c=0xffffffffu; for(int i=0;i<8;i++) c=__crc32cb(c,q[i]); c=~c;
        if(c!=gc) fail++;
    }
    fprintf(stderr,"gather: %d/%d fails\n",fail,iters);
    return fail>0;
}

int main(int argc,char**argv){
    int v=argc>1?atoi(argv[1]):2;
    int it=argc>2?atoi(argv[2]):5000;
    switch(v){
        case 1: return v_dense(it);
        case 2: return v_spmv(it);
        case 3: return v_gather(it);
        default: fprintf(stderr,"variant 1-3\n"); return 1;
    }
}

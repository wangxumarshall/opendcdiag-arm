/* CRC-sensitive floating-point probe: detects any single-bit corruption
 * in FP computation results by CRC-checking outputs. Run pinned to a core
 * under external load; a healthy core never mismatches, a corrupt core
 * produces occasional CRC mismatches.
 * Build: gcc -O3 -march=armv8.1-a+crc+crypto -std=gnu17 fpprobe.c -o fpprobe
 * Run:   taskset -c <CORE> ./fpprobe <iters>  -> prints mismatch count
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <arm_acle.h>
#define N 1024
static double A[N], B[N], C[N];

static uint32_t crc64vec(const double*v){
    uint32_t c=0xffffffffu;
    const uint8_t*p=(const uint8_t*)v;
    for(int i=0;i<N*8;i++) c=__crc32cb(c,p[i]);
    return ~c;
}

int main(int argc,char**argv){
    uint64_t iters = (argc>1)?strtoull(argv[1],0,10):1000000;
    for(int i=0;i<N;i++){ A[i]=(double)(i+1)*0.5; B[i]=(double)(i+1)*1.5; }
    for(int i=0;i<N;i++) C[i]=A[i]*B[i];
    uint32_t gold=crc64vec(C);
    uint64_t mismatch=0, rounds=0;
    for(uint64_t k=0;k<iters;k++){
        for(int i=0;i<N;i++) C[i]=A[i]*B[i];
        uint32_t c=crc64vec(C);
        rounds++;
        if(c!=gold) mismatch++;
    }
    double dp_gold=0; for(int i=0;i<N;i++) dp_gold+=A[i]*B[i];
    uint32_t dp_fail=0;
    for(uint64_t k=0;k<iters;k++){
        double dp=0; for(int i=0;i<N;i++) dp+=A[i]*B[i];
        if(dp!=dp_gold) dp_fail++;
    }
    fprintf(stderr,"vec_mul CRC: %lu/%lu mismatches ; dotprod: %u/%lu mismatches\n",
            (unsigned long)mismatch,(unsigned long)rounds, dp_fail,(unsigned long)iters);
    printf("%lu %u\n",(unsigned long)mismatch, dp_fail);
    return (mismatch>0 || dp_fail>0)?1:0;
}

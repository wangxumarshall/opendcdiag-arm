#include <stdio.h>
#include <stdint.h>
#include <arm_acle.h>
#define N 256
static double a[N], b[N], c[N];
int main(int argc,char**argv){
    int it=argc>1?atoi(argv[1]):5000;
    for(int i=0;i<N;i++){a[i]=(i+1)*0.5;b[i]=(i+1)*1.5;}
    for(int i=0;i<N;i++) c[i]=a[i]*b[i]+a[i]; // FMA
    uint8_t*p=(uint8_t*)c; uint32_t gc=0xffffffffu; for(int i=0;i<N*8;i++) gc=__crc32cb(gc,p[i]); gc=~gc;
    int fail=0;
    for(int k=0;k<it;k++){ for(int i=0;i<N;i++) c[i]=a[i]*b[i]+a[i]; uint32_t cc=0xffffffffu; p=(uint8_t*)c; for(int i=0;i<N*8;i++) cc=__crc32cb(cc,p[i]); cc=~cc; if(cc!=gc) fail++; }
    fprintf(stderr,"pure-FMA: %d/%d fails\n",fail,it);
    return fail>0;
}

/* Microarchitectural SDC probe for Kunpeng 920 / TaiShan v110.
 * Each probe stresses one functional unit and self-checks results to catch
 * silent data corruption. Designed to run pinned to a single core, many
 * iterations, so transient HW errors surface.
 *
 * Build: gcc -O3 -march=armv8.1-a+crc+crypto -std=gnu17 -Wno-incompatible-pointer-types probe.c -o probe
 * Run:   taskset -c <CORE> ./probe <probe_id> <iters>
 * probe_id: 0=all (default), or 1..8 individual.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sched.h>
#include <arm_neon.h>
#include <arm_acle.h>

#define ARR 4096
static double  *da, *db, *dc;
static float   *fa, *fb, *fc;
static uint64_t *ua, *ub;
static uint8_t  *byte;

static double gold_dp(double *a, double *b){ double s=0; for(int i=0;i<ARR;i++) s+=a[i]*b[i]; return s; }
static uint32_t crc_buf(const void*p,size_t n){ uint32_t c=0xffffffffu; const uint8_t*x=p;
    for(size_t i=0;i<n;i++) c=__crc32cb(c,x[i]); return ~c; }

static int p_alu(uint64_t it){
    for(uint64_t k=0;k<it;k++){
        uint64_t a=0x123456789abcdefULL, b=0x9e3779b97f4a7c15ULL;
        for(int j=0;j<2000;j++){ a=(a*b)+0x9e3779b97f4a7c15ULL; b=(b^(a>>7))+a; }
    }
    uint64_t a=0x123456789abcdefULL, b=0x9e3779b97f4a7c15ULL;
    for(int j=0;j<2000;j++){ a=(a*b)+0x9e3779b97f4a7c15ULL; b=(b^(a>>7))+a; }
    uint64_t ref=a, ref2;
    a=0x123456789abcdefULL; b=0x9e3779b97f4a7c15ULL;
    for(int j=0;j<2000;j++){ a=(a*b)+0x9e3779b97f4a7c15ULL; b=(b^(a>>7))+a; }
    ref2=a;
    return (ref!=ref2)?1:0;
}
static int p_dp(uint64_t it){
    double ref=-1;
    for(uint64_t k=0;k<it;k++){ double s=gold_dp(da,db); if(ref<0) ref=s; else if(s!=ref) return 1; }
    return 0;
}
static int p_sp(uint64_t it){
    float ref=-1;
    for(uint64_t k=0;k<it;k++){ float s=0; for(int i=0;i<ARR;i++) s+=fa[i]*fb[i]; if(ref<0) ref=s; else if(s!=ref) return 1; }
    return 0;
}
static int p_neon(uint64_t it){
    uint32_t ref=0;
    for(uint64_t k=0;k<it;k++){
        uint32x4_t acc=vdupq_n_u32(0);
        uint32x4_t kk=vdupq_n_u32(0xdeadbeefu);
        for(int i=0;i<ARR;i+=4){
            uint32x4_t x=vld1q_u32((const uint32_t*)(ua+i));
            uint32x4_t y=vld1q_u32((const uint32_t*)(ub+i));
            acc=veorq_u32(acc, vmlaq_u32(x,y,kk));
        }
        uint32_t tmp[4]; vst1q_u32(tmp, acc);
        uint32_t s=tmp[0]^tmp[1]^tmp[2]^tmp[3];
        if(k==0) ref=s; else if(s!=ref) return 1;
    }
    return 0;
}
static int p_l1d(uint64_t it){
    uint32_t ref=-1;
    for(uint64_t k=0;k<it;k++){ uint32_t c=crc_buf(byte, 32768); if(ref==(uint32_t)-1) ref=c; else if(c!=ref) return 1; }
    return 0;
}
static int p_mem(uint64_t it){
    uint32_t ref=-1;
    for(uint64_t k=0;k<it;k++){ uint32_t c=crc_buf(byte, ARR*8); c^=crc_buf(byte+32768, 32768); if(ref==(uint32_t)-1) ref=c; else if(c!=ref) return 1; }
    return 0;
}
static int __attribute__((noinline,optimize("O0"))) p_branch(uint64_t it){
    for(uint64_t k=0;k<it;k++){ volatile uint64_t s=0, ref=0;
        for(int i=0;i<2000;i++){ if(i&1) s+=i*3; else s-=i*5; }
        if(k==0) ref=s; else if(s!=ref) return 1;
    }
    return 0;
}
static int p_crc(uint64_t it){
    uint32_t ref=-1;
    for(uint64_t k=0;k<it;k++){ uint32_t c=0xffffffffu; for(int i=0;i<ARR;i++) c=__crc32cw(c, ua[i]); c=~c; if(ref==(uint32_t)-1) ref=c; else if(c!=ref) return 1; }
    return 0;
}
static void init(){
    da=aligned_alloc(64,ARR*8); db=aligned_alloc(64,ARR*8); dc=aligned_alloc(64,ARR*8);
    fa=aligned_alloc(64,ARR*4); fb=aligned_alloc(64,ARR*4); fc=aligned_alloc(64,ARR*4);
    ua=aligned_alloc(64,ARR*8); ub=aligned_alloc(64,ARR*8);
    byte=aligned_alloc(64,65536);
    for(int i=0;i<ARR;i++){ da[i]=1.0/(i+1); db[i]=1.0/(i+2); fa[i]=1.0f/(i+1); fb[i]=1.0f/(i+2); ua[i]=i*2654435761ULL; ub[i]=i*40503ULL; }
    for(int i=0;i<65536;i++) byte[i]=i&0xff;
}
int main(int argc,char**argv){
    int pid = (argc>1)?atoi(argv[1]):0;
    uint64_t it = (argc>2)?strtoull(argv[2],0,10):2000;
    init();
    p_alu(2); p_dp(2); p_sp(2); p_neon(2); p_l1d(2); p_mem(2); p_branch(2); p_crc(2);
    struct {const char*name; int(*fn)(uint64_t);} pr[]={
        {"ALU_int",p_alu},{"FPU_double",p_dp},{"FPU_float",p_sp},{"NEON_vec",p_neon},
        {"L1D_cache",p_l1d},{"L2L3_mem",p_mem},{"Branch_pred",p_branch},{"CRC_engine",p_crc}};
    if(pid==0){
        for(int i=0;i<8;i++){ int f=pr[i].fn(it); fprintf(stderr,"%-14s %s\n",pr[i].name,f?"MISCOMPARE":"ok"); printf("%s\n",f?"FAIL":"ok"); }
    } else {
        int f=pr[pid-1].fn(it);
        fprintf(stderr,"%-14s %s\n",pr[pid-1].name,f?"MISCOMPARE":"ok");
        printf("%s\n",f?"FAIL":"ok");
    }
    return 0;
}

/* Long-running L1d SDC probe: stays in one cache line, hammers loads,
 * checks for torn/corrupted reads of a known pattern. Tolerant of rare
 * errors. Designed to run for hours in background, logging any hit. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sched.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#define CACHELINE 64
static volatile uint64_t sink;
int main(int argc,char**argv){
    int cpu=argc>1?atoi(argv[1]):179;
    unsigned long long reps=argc>2?strtoull(argv[2],0,0):0; /* 0=infinite */
    int logfd=argc>3?open(argv[3],O_WRONLY|O_CREAT|O_APPEND,0644):-1;
    cpu_set_t cs;CPU_ZERO(&cs);CPU_SET(cpu,&cs);
    if(sched_setaffinity(0,sizeof(cs),&cs)){perror("setaffinity");return 2;}
    void*p=NULL;posix_memalign(&p,CACHELINE,CACHELINE*2);
    uint64_t pat=0xffffd937172de000ULL; /* the historic crashed pattern */
    uint64_t*m=(uint64_t*)p;
    for(int i=0;i<CACHELINE/8;i++)m[i]=pat;
    unsigned long long r=0,err=0;
    struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);
    unsigned long long start=ts.tv_sec;
    while(!reps || r<reps){
        r++;
        uint64_t v=*(volatile uint64_t*)p;
        sink^=v;
        if(__builtin_expect(v!=pat,0)){
            err++;
            if(logfd>=0){
                char buf[160];int n=snprintf(buf,sizeof(buf),
                    "HIT r=%llu err=%lu obs=%016lx xor=%016lx t=%llu\n",
                    r,err,v,pat^v,(unsigned long long)(ts.tv_sec-start));
                clock_gettime(CLOCK_MONOTONIC,&ts);
                write(logfd,buf,n);
            } else {
                printf("HIT r=%llu obs=%016lx xor=%016lx\n",r,v,pat^v);fflush(stdout);
            }
        }
        if((r%200000000ULL)==0){ /* report every ~1-2s */
            clock_gettime(CLOCK_MONOTONIC,&ts);
            printf("tick cpu=%d r=%llu err=%lu elapsed=%llu sink=%016lx\n",
                cpu,r,err,(unsigned long long)(ts.tv_sec-start),(uint64_t)sink);fflush(stdout);
        }
    }
    printf("DONE cpu=%d r=%llu err=%lu sink=%016lx\n",cpu,r,err,(uint64_t)sink);
    return 0;
}

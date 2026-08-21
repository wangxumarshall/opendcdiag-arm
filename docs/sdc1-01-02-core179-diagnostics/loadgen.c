/* CPU load generator: spawn N workers that burn CPU to create full-system load.
 * Usage: ./loadgen <nworkers> <seconds>
 * Each worker does pointless FP+int work. Designed to create the pressure
 * conditions under which core 179's SDC manifests, without depending on opendcdiag.
 * Build: gcc -O2 loadgen.c -o loadgen
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <sched.h>
#include <signal.h>
#include <sys/wait.h>
#include <stdatomic.h>
static volatile sig_atomic_t stop=0;
static void h(int s){ stop=1; }
static void worker(int id){
    double a=1.0001, b=0.9999, s=0; uint64_t z=0;
    while(!stop){ for(int i=0;i<1000000;i++){ s=s*a+b; a*=1.0000001; z+=i*2654435761ULL; } }
    _exit(0);
}
int main(int argc,char**argv){
    int n=atoi(argv[1]), secs=atoi(argv[2]);
    signal(SIGALRM,h); signal(SIGTERM,h); alarm(secs);
    for(int i=0;i<n;i++){ if(fork()==0) worker(i); }
    while(wait(NULL)>0);
    return 0;
}

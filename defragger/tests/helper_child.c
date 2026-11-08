// SPDX-License-Identifier: GPL-3.0-or-later
// Controlled, test-only native child for privileged helper lifecycle tests.
// No filesystem or block device is opened; markers live in a private test dir.
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stopped=0;
static void stop_handler(int signal_number){
    (void)signal_number;
    stopped=1;
}
static int sleep_milliseconds(long milliseconds){
    struct timespec delay={milliseconds/1000,(milliseconds%1000)*1000000L};
    while(nanosleep(&delay,&delay)<0){
        if(errno!=EINTR)return -1;
    }
    return 0;
}
static int create_marker(const char *base,const char *suffix,const char *text){
    char path[4096];
    const int length=snprintf(path,sizeof(path),"%s%s",base,suffix);
    if(length<0||(size_t)length>=sizeof(path))return -1;
    FILE *file=fopen(path,"wb");
    if(file==NULL)return -1;
    const int ok=fputs(text,file)>=0;
    if(fclose(file)!=0)return -1;
    return ok?0:-1;
}
static double elapsed(const struct timespec *start,const struct timespec *now){
    return (double)(now->tv_sec-start->tv_sec)+
           (double)(now->tv_nsec-start->tv_nsec)/1000000000.0;
}
int main(int argc,char **argv){
    const char *marker=getenv("LD_HELPER_TEST_MARKER");
    if(marker==NULL||!*marker||argc!=2){
        fputs("Usage: helper-child {defrag|mapper} (with LD_HELPER_TEST_MARKER)\n",stderr);
        return 2;
    }
    const int writer=strcmp(argv[1],"defrag")==0;
    const char *ignore=getenv("LD_HELPER_TEST_IGNORE_SIGINT");
    struct sigaction handler;
    memset(&handler,0,sizeof(handler));
    sigemptyset(&handler.sa_mask);
    if(ignore!=NULL&&strcmp(ignore,"1")==0)handler.sa_handler=SIG_IGN;
    else handler.sa_handler=stop_handler;
    if(sigaction(SIGINT,&handler,NULL)<0)return 2;

    // The helper must not leave its control stdin readable by the child.
    char ch;
    ssize_t read_result;
    do{read_result=read(STDIN_FILENO,&ch,1);}
    while(read_result<0&&errno==EINTR);
    if(read_result!=0)return 2;

    char pid[64];
    if(snprintf(pid,sizeof(pid),"%ld",(long)getpid())<0 ||
       create_marker(marker,".ready",pid)<0)return 2;
    if(writer){
        // STOP_READY only follows the deliberate startup transition;
        // a signal delivered earlier must remain queued cooperatively.
        if(sleep_milliseconds(250)<0)return 2;
        puts("@@STOP_READY");
        puts("worker initialised");
        fflush(stdout);
    }

    struct timespec started,now;
    if(clock_gettime(CLOCK_MONOTONIC,&started)!=0)return 2;
    for(;;){
        if(stopped)break;
        if(clock_gettime(CLOCK_MONOTONIC,&now)!=0)return 2;
        if(elapsed(&started,&now)>=10.0)return 3;
        if(writer){
            puts("worker checkpoint");
            fflush(stdout);
        }
        if(sleep_milliseconds(20)<0)return 2;
    }
    // Model cooperative cancellation completing only at a safe writer boundary.
    if(sleep_milliseconds(150)<0)return 2;
    if(create_marker(marker,".stopped","safe boundary reached")<0)return 2;
    return 130;
}

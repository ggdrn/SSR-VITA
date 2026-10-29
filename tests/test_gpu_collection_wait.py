#!/usr/bin/env python3
"""Real emergency wait must await queued GPU-memory GC, preserving permits."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2];src=(root/'vitaGL/source/utils/gpu_utils.c').read_text()
a=src.index('static void gpu_wait_emergency_collection(void)');b=src.index('\n#endif',a)
h=r'''
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>
#define FRAME_PURGE_FREQ 4
static int gc_mutex[2]={0,1},permits[2]={0,4},started,completed,allowed,quit;
static int fail_first,fail_submit,fail_ack;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond=PTHREAD_COND_INITIALIZER;
static int sceKernelWaitSema(int sem,int n,void *timeout){
 pthread_mutex_lock(&lock);
 if((sem==1 && n==4 && fail_first) || (sem==1 && n==1 && fail_ack)) {pthread_mutex_unlock(&lock);return -1;}
 while(permits[sem]<n)pthread_cond_wait(&cond,&lock);
 permits[sem]-=n;pthread_cond_broadcast(&cond);pthread_mutex_unlock(&lock);return 0;
}
static int sceKernelSignalSema(int sem,int n){
 pthread_mutex_lock(&lock);
 if(sem==0 && fail_submit){pthread_mutex_unlock(&lock);return -1;}
 assert(permits[sem]+n<=4);permits[sem]+=n;pthread_cond_broadcast(&cond);pthread_mutex_unlock(&lock);return 0;
}
'''
h+=src[a:b]
h+=r'''
static void *collector(void *arg){
 for(;;){
  pthread_mutex_lock(&lock);while(!permits[0] && !quit)pthread_cond_wait(&cond,&lock);
  if(quit){pthread_mutex_unlock(&lock);return NULL;}
  permits[0]--;started++;pthread_cond_broadcast(&cond);
  while(!allowed)pthread_cond_wait(&cond,&lock);
  completed++;assert(permits[1]<4);permits[1]++;pthread_cond_broadcast(&cond);pthread_mutex_unlock(&lock);
 }
}
static int returned;
static void *emergency(void *arg){gpu_wait_emergency_collection();__atomic_store_n(&returned,1,__ATOMIC_RELEASE);return NULL;}
static void delay(void){struct timespec t={0,10000000};nanosleep(&t,NULL);}
int main(void){
 pthread_t gc,waiter;assert(!pthread_create(&gc,NULL,collector,NULL));
 // Two outstanding normal frame jobs must complete before the emergency job.
 sceKernelWaitSema(1,2,NULL);sceKernelSignalSema(0,2);
 pthread_create(&waiter,NULL,emergency,NULL);delay();
 assert(!__atomic_load_n(&returned,__ATOMIC_ACQUIRE));
 pthread_mutex_lock(&lock);assert(completed==0);allowed=1;pthread_cond_broadcast(&cond);pthread_mutex_unlock(&lock);
 pthread_join(waiter,NULL);assert(completed==3 && permits[0]==0 && permits[1]==4);
 for(int i=0;i<100;i++)gpu_wait_emergency_collection();
 pthread_mutex_lock(&lock);assert(completed==103 && permits[0]==0 && permits[1]==4);pthread_mutex_unlock(&lock);
 fail_first=1;gpu_wait_emergency_collection();assert(permits[1]==4);fail_first=0;
 fail_submit=1;gpu_wait_emergency_collection();assert(permits[1]==4);fail_submit=0;
 fail_ack=1;gpu_wait_emergency_collection();
 pthread_mutex_lock(&lock);while(completed<104)pthread_cond_wait(&cond,&lock);assert(permits[1]==4);quit=1;pthread_cond_broadcast(&cond);pthread_mutex_unlock(&lock);
 pthread_join(gc,NULL);
 puts("PASS: drain pending jobs, await actual emergency completion, restore all permits, repeated cycles and API failure paths");
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.c').write_text(h)
 subprocess.run(['cc','-pthread','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
# Verify both actual CPU/GPU recovery paths use the tested helper.
a=src.index('int unsafe_allocator_counter');b=src.index('void *gpu_alloc_mapped_aligned_for_cpu(',a)
assert src[a:b].count('gpu_wait_emergency_collection();')==2
assert 'sceKernelDelayThread(1000000)' not in src[a:b]

#!/usr/bin/env python3
"""Real bridge lifecycle with native host pthreads and deterministic failure injection."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
src=(root/'src/bridges/pthread_bridge.c').read_text()
src=src[src.index('typedef struct {',src.index('// --- threads')):src.index('int pthread_getschedparam_bridge')]
harness=r'''
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <stdatomic.h>
#include <unistd.h>
#include <errno.h>
typedef int SceUID;
typedef unsigned SceUInt;
typedef struct {size_t size;void *stack;size_t stackSize;} SceKernelThreadInfo;
#define B_EINVAL 22
#define SLICE_NS 5000000
#define SCE_KERNEL_ERROR_WAIT_TIMEOUT (-999)
#define L_ERROR(...) ((void)0)
#define L_INFO(...) ((void)0)
#define WAIT_BEGIN(...) ((void)0)
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static void global_lock(void){assert(!pthread_mutex_lock(&lock));}
static void global_unlock(void){assert(!pthread_mutex_unlock(&lock));}
static _Atomic int next_uid=1,forgotten;
static _Thread_local int uid;
static SceUID sceKernelGetThreadId(void){if(!uid)uid=atomic_fetch_add(&next_uid,1);return uid;}
static void sceKernelChangeThreadCpuAffinityMask(int u,int m){}
static int thread_cpu_mask(void){return 15;}
static void signal_forget_thread(int u){assert(u>0);atomic_fetch_add(&forgotten,1);}
static void signal_poll(void){}
static void thread_wait_end(void){}
static void sceKernelDelayThread(int us){usleep(us);}
static int sceKernelWaitThreadEnd(int u,void *p,SceUInt *t){return 0;}
static int sceKernelGetThreadInfo(int u,SceKernelThreadInfo *i){return -1;}
static int fail_create,fail_join,fail_detach;
static int native_create(pthread_t *t,const pthread_attr_t *a,void *(*fn)(void *),void *p){
 if(fail_create){fail_create=0;return EAGAIN;}return pthread_create(t,a,fn,p);
}
static int native_join(pthread_t t,void **p){if(fail_join){fail_join=0;return EINVAL;}return pthread_join(t,p);}
static int native_detach(pthread_t t){if(fail_detach){fail_detach=0;return EINVAL;}return pthread_detach(t);}
#define pthread_create native_create
#define pthread_join native_join
#define pthread_detach native_detach
'''+src+r'''
#undef pthread_create
#undef pthread_join
#undef pthread_detach
static void *quick(void *p){assert(pthread_self_bridge()==pthread_self());return p;}
static void *exit_now(void *p){pthread_exit_bridge(p);return NULL;}
static void await_empty(void){
 for(int i=0;i<10000;i++){global_lock();unsigned n=threads_tracked;global_unlock();if(!n)return;usleep(1000);}
 assert(!"metadata not retired");
}
static void await_exited(pthread_t t){
 for(int i=0;i<10000;i++){global_lock();thread_slot *s=slot_find(t);int done=s&&s->exited;global_unlock();if(done)return;usleep(1000);}
 assert(!"thread did not exit");
}
static void *driver(void *p){
 for(int i=0;i<100;i++){pthread_t t;void *ret=NULL;
  assert(!pthread_create_bridge(&t,NULL,quick,p));assert(!pthread_join_bridge(t,&ret));assert(ret==p);
 }return NULL;
}
static _Atomic int go,started;
static void *blocked(void *p){atomic_fetch_add(&started,1);while(!atomic_load(&go))usleep(1000);return p;}
int main(void){
 pthread_t t;void *ret=NULL;unsigned completed=0;
 for(int i=0;i<300;i++){
  assert(!pthread_create_bridge(&t,NULL,i%2?exit_now:quick,(void *)(uintptr_t)(i+1)));
  assert(t);assert(!pthread_join_bridge(t,&ret));assert(ret==(void *)(uintptr_t)(i+1));completed++;
 }
 assert(!threads_tracked);
 bionic_attr attr;pthread_attr_init_bridge(&attr);pthread_attr_setdetachstate_bridge(&attr,1);
 for(int i=0;i<300;i++){
  assert(!pthread_create_bridge(&t,&attr,i%2?exit_now:quick,NULL));assert(t);await_empty();completed++;
 }
 for(int i=0;i<160;i++){
  assert(!pthread_create_bridge(&t,NULL,quick,NULL));await_exited(t);
  assert(!pthread_detach_bridge(t));await_empty();completed++;
 }
 for(int i=0;i<160;i++){
  assert(!pthread_create_bridge(&t,NULL,quick,NULL));assert(!pthread_detach_bridge(t));await_empty();completed++;
 }
 unsigned before=threads_tracked;
 fail_create=1;assert(pthread_create_bridge(&t,NULL,quick,NULL)==EAGAIN);assert(threads_tracked==before);
 assert(!pthread_create_bridge(&t,NULL,quick,(void *)123));await_exited(t);
 fail_join=1;assert(pthread_join_bridge(t,&ret)==EINVAL);assert(threads_tracked==1);
 fail_detach=1;assert(pthread_detach_bridge(t)==EINVAL);assert(threads_tracked==1);
 assert(!pthread_join_bridge(t,&ret)&&ret==(void *)123);completed++;
 pthread_t drivers[6];
 for(int i=0;i<6;i++)assert(!pthread_create(&drivers[i],NULL,driver,(void *)(uintptr_t)(i+1)));
 for(int i=0;i<6;i++)assert(!pthread_join(drivers[i],NULL));
 completed+=600;await_empty();
 // Outstanding joinable threads still honor the bound; never reclaim a live slot.
 pthread_t full[MAX_THREADS];
 for(int i=0;i<MAX_THREADS;i++)assert(!pthread_create_bridge(&full[i],NULL,blocked,NULL));
 assert(pthread_create_bridge(&t,NULL,quick,NULL)==11);
 assert(threads_tracked==MAX_THREADS);atomic_store(&go,1);
 for(int i=0;i<MAX_THREADS;i++)assert(!pthread_join_bridge(full[i],NULL));
 completed+=MAX_THREADS;await_empty();
 assert(threads_created==completed&&atomic_load(&forgotten)==completed);
 assert(threads_released==completed+1); // includes the failed reservation
 for(int i=0;i<MAX_SELF_MAP;i++)assert(!self_map[i].thread);
 printf("PASS: %u bridge threads: return/exit, joins, detached completion, late detach, failures, concurrent creators, 128 live bound\n",completed);
}
'''
with tempfile.TemporaryDirectory() as d:
    p=Path(d);(p/'test.c').write_text(harness)
    subprocess.run(['cc','-pthread','-fsanitize=address,undefined','-Wno-int-to-pointer-cast','-Wno-pointer-to-int-cast',str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True,timeout=60)

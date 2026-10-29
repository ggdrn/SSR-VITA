#!/usr/bin/env python3
"""Real WWW worker/join code: callbacks must finish before request destruction."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1];src=(root/'src/jni/java_impl.c').read_text()
a=src.index('#define WWW_CLASS');b=src.index('// --- java.lang / java.util helpers',a)
assert 'jni_register_method(NULL, "join", "()V", www_join)' in src
h=r'''
#define _POSIX_C_SOURCE 200809L
#include "jni/jni.h"
#include <pthread.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
typedef int SceUID;
typedef unsigned SceSize;
typedef struct {const char *name,*value;} http_header;
typedef struct {void *user; int (*on_status)(void *,int,const char *);int (*on_header)(void *,const char *,const char *);int (*on_data)(void *,const uint8_t *,size_t,size_t,size_t);} http_callbacks;
#define MAP_MAX 64
typedef struct {int n;jobj *keys[MAP_MAX],*values[MAP_MAX];} hash_map;
static hash_map *map_of(jobj *self){return self->native;}
#define L_INFO(...) ((void)0)
#define L_WARN(...) ((void)0)
#define L_ERROR(...) ((void)0)
#define RET_VOID ((jvalue){.j=0})
static _Thread_local int tid;
static int next_tid,polls;
static int sceKernelGetThreadId(void){if(!tid)tid=__atomic_add_fetch(&next_tid,1,__ATOMIC_RELAXED);return tid;}
static uint64_t sceKernelGetProcessTimeWide(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000+t.tv_nsec/1000;}
static int sceKernelDelayThread(unsigned us){struct timespec t={us/1000000,(us%1000000)*1000};nanosleep(&t,NULL);return 0;}
static int sceKernelExitDeleteThread(int status){return 0;}
static int signal_poll(void){__atomic_add_fetch(&polls,1,__ATOMIC_RELAXED);return 0;}
static pthread_mutex_t obj_lock=PTHREAD_MUTEX_INITIALIZER;
static jobj *objects[8192];static int nobjects;
static jobj *object(jobj_kind k,const char *cls){jobj *o=calloc(1,sizeof(*o));assert(o);o->kind=k;o->magic=JOBJ_MAGIC;o->cls=cls;pthread_mutex_lock(&obj_lock);objects[nobjects++]=o;assert(nobjects<8192);pthread_mutex_unlock(&obj_lock);return o;}
jobj *jni_new_string(const char *s){jobj *o=object(JO_STRING,"java/lang/String");o->str.utf8=strdup(s);return o;}
const char *jni_string_utf8(jobj *s){return s?s->str.utf8:"";}
jobj *jni_new_array(const char *cls,int len,int size){jobj *o=object(JO_ARRAY,cls);o->arr.len=len;o->arr.data=calloc(len,size);return o;}
void *jni_get_env(void){return (void *)1;}
jobj *jni_find_class(const char *s){static jobj c={.magic=JOBJ_MAGIC,.cls="com/unity3d/player/WWW"};return &c;}
static int net_start_thread(const char *,int (*)(SceSize,void *),SceSize,void *);
static int http_request(const char *,const char *,const http_header *,int,const void *,size_t,const http_callbacks *,char *,size_t);
static jvalue www_join(jobj *,const jvalue *);
static struct {jobj *self;int alive,mode,headers,reads,done,errors;} target[16];
static int check(int id){assert(id>=0 && id<16 && __atomic_load_n(&target[id].alive,__ATOMIC_ACQUIRE));return id;}
static uint8_t on_header(void *env,jobj *c,int id,jobj *line){check(id);target[id].headers++;if(target[id].mode==5)www_join(target[id].self,NULL);return target[id].mode==1;}
static uint8_t on_read(void *env,jobj *c,int id,jobj *data,int n){check(id);target[id].reads++;assert(n==4 && !memcmp(data->arr.data,"data",4));return target[id].mode==2;}
static void on_error(void *env,jobj *c,int id,jobj *message){check(id);target[id].errors++;if(target[id].mode==6)www_join(target[id].self,NULL);}
static void on_done(void *env,jobj *c,int id){check(id);target[id].done++;}
void *jni_find_native(const char *cls,const char *name){
 if(!strcmp(name,"headerCallback"))return on_header;
 if(!strcmp(name,"readCallback"))return on_read;
 if(!strcmp(name,"errorCallback"))return on_error;
 assert(!strcmp(name,"doneCallback"));return on_done;
}
'''
h+=r'''
static int fail_alloc,alloc_index;
static void *checked_calloc(size_t n,size_t size){if(fail_alloc && ++alloc_index==fail_alloc)return NULL;return calloc(n,size);}
#define calloc checked_calloc
'''
h+=src[a:b]
h+='\n#undef calloc\n'
h+=r'''
static pthread_mutex_t gate_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cond=PTHREAD_COND_INITIALIZER;
static int allowed,started,start_fail,joined,entered;
static pthread_t workers[64];static int nworkers;
static int http_request(const char *method,const char *url,const http_header *h,int nh,const void *body,size_t bl,const http_callbacks *cb,char *err,size_t es){
 pthread_mutex_lock(&gate_lock);started++;pthread_cond_broadcast(&gate_cond);while(!allowed)pthread_cond_wait(&gate_cond,&gate_lock);pthread_mutex_unlock(&gate_lock);
 www_request *r=cb->user;int mode=target[r->id].mode;
 if(mode==4){snprintf(err,es,"network error");return -1;}
 int status=mode==3?404:200;
 if(!cb->on_status(cb->user,status,"HTTP/1.1 200 OK"))return status;
 if(!cb->on_header(cb->user,"Content-Length","4"))return status;
 cb->on_data(cb->user,(const uint8_t *)"data",4,4,4);return status;
}
struct start_arg {int (*entry)(SceSize,void *);www_request *request;};
static void *worker_entry(void *arg){struct start_arg *s=arg;int (*entry)(SceSize,void *)=s->entry;www_request *r=s->request;free(s);entry(sizeof(r),&r);return NULL;}
static int net_start_thread(const char *name,int (*entry)(SceSize,void *),SceSize n,void *arg){
 if(start_fail)return -1;
 struct start_arg *s=malloc(sizeof(*s));s->entry=entry;s->request=*(www_request **)arg;
 assert(!pthread_create(&workers[nworkers++],NULL,worker_entry,s));return 0;
}
static void start(jobj *self,int id,int mode){
 target[id].self=self;target[id].mode=mode;target[id].alive=1;
 self->cls=WWW_CLASS;self->native=NULL;
 jvalue a[4]={{.i=id},{.l=jni_new_string("https://test.invalid/asset")},{.l=NULL},{.l=NULL}};www_init(self,a);
}
static void *joiner(void *arg){__atomic_add_fetch(&entered,1,__ATOMIC_RELEASE);www_join(arg,NULL);__atomic_add_fetch(&joined,1,__ATOMIC_RELEASE);return NULL;}
static void wait_started(int n){pthread_mutex_lock(&gate_lock);while(started<n)pthread_cond_wait(&gate_cond,&gate_lock);pthread_mutex_unlock(&gate_lock);}
static void release_gate(void){pthread_mutex_lock(&gate_lock);allowed=1;pthread_cond_broadcast(&gate_cond);pthread_mutex_unlock(&gate_lock);}
static void finish_workers(void){for(int i=0;i<nworkers;i++)pthread_join(workers[i],NULL);nworkers=0;}
static void reset(void){memset(target,0,sizeof(target));allowed=started=joined=entered=0;}
int main(void){
 jobj self={0};pthread_t a,b;
 // Two simultaneous joiners cannot return while the worker is paused before callbacks.
 reset();start(&self,0,0);wait_started(1);
 pthread_create(&a,NULL,joiner,&self);pthread_create(&b,NULL,joiner,&self);
 while(__atomic_load_n(&entered,__ATOMIC_ACQUIRE)<2)sceKernelDelayThread(1000);
 sceKernelDelayThread(20000);assert(!__atomic_load_n(&joined,__ATOMIC_ACQUIRE) && target[0].done==0);
 release_gate();pthread_join(a,NULL);pthread_join(b,NULL);target[0].alive=0;finish_workers();
 assert(!self.native && target[0].done==1 && target[0].errors==0 && polls>0);www_join(&self,NULL);
 // Abort/error paths must never send doneCallback after errorCallback.
 for(int mode=1;mode<=5;mode++){
  reset();start(&self,0,mode);wait_started(1);release_gate();www_join(&self,NULL);target[0].alive=0;finish_workers();
  assert(!self.native && !target[0].done);
  assert(target[0].errors==(mode==5?0:1));
  if(mode==5)assert(target[0].headers==1 && !target[0].reads);
 }
 // Failed thread creation also publishes completion, so disposal never hangs.
 for(int mode=0;mode<=6;mode+=6){reset();start_fail=1;start(&self,0,mode);www_join(&self,NULL);assert(!self.native && target[0].errors==1);start_fail=0;}
 for(int which=1;which<=2;which++){reset();fail_alloc=which;alloc_index=0;start(&self,0,6);www_join(&self,NULL);assert(!self.native && target[0].errors==1);fail_alloc=0;}
 // Independent requests can join concurrently without sharing completion state.
 reset();jobj many[8]={{0}};pthread_t joins[8];
 for(int i=0;i<8;i++)start(&many[i],i,0);wait_started(8);
 for(int i=0;i<8;i++)pthread_create(&joins[i],NULL,joiner,&many[i]);release_gate();
 for(int i=0;i<8;i++){pthread_join(joins[i],NULL);target[i].alive=0;assert(!many[i].native && target[i].done==1 && !target[i].errors);}
 finish_workers();
 for(int i=0;i<nobjects;i++){jobj *o=objects[i];if(o->kind==JO_STRING)free(o->str.utf8);else if(o->kind==JO_ARRAY)free(o->arr.data);free(o);}
 puts("PASS: blocking/repeated/concurrent join, callback lifetime, success/error/abort/self-disposal, thread failure, 8 independent requests");
}
'''
with tempfile.TemporaryDirectory() as d:
    p=Path(d);(p/'test.c').write_text(h)
    subprocess.run(['cc','-I',str(root/'src'),'-pthread','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)

#!/usr/bin/env python3
"""Reuse the real bounded signal registry across many finished workers."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
src=(root/'src/bridges/signal_bridge.c').read_text()
src=src[src.index('typedef struct {\n  uint32_t thread;'):src.index('static uint32_t self_id(')]
harness=r'''
#include <stdint.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
typedef int SceUID;
typedef int SceKernelLwMutexWork;
static unsigned created,deleted;
static void sceKernelLockLwMutex(int *p,int n,void *t){}
static void sceKernelUnlockLwMutex(int *p,int n){}
static int sceKernelCreateSema(const char *s,int a,int b,int c,void *p){return ++created;}
static void sceKernelDeleteSema(int s){assert(s>0);deleted++;}
'''+src+r'''
int main(void){
 for(int round=0;round<20;round++){
  for(int i=0;i<MAX_SIG_THREADS;i++){
   unsigned uid=round*MAX_SIG_THREADS+i+1;
   sig_slot *s=slot_for(uid,1);assert(s);s->pending=(1u<<24)|(1u<<30);total_pending+=2;
  }
  assert(!slot_for(999999,1));
  for(int i=0;i<MAX_SIG_THREADS;i++){
   unsigned uid=round*MAX_SIG_THREADS+i+1;signal_forget_thread(uid);
   assert(!slot_for(uid,0));signal_forget_thread(uid);
  }
  assert(total_pending==0);
 }
 assert(created==2560&&created==deleted);
 puts("PASS: 2560 signal registrations retired, pending count reset, semaphores deleted, repeated retirement safe");
}
'''
with tempfile.TemporaryDirectory() as d:
    p=Path(d);(p/'test.c').write_text(harness)
    subprocess.run(['cc','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)

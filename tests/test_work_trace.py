#!/usr/bin/env python3
"""Render-thread attribution must exclude asynchronous worker activity."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
def run(source,extra=(),args=((),)):
 with tempfile.TemporaryDirectory() as d:
  p=Path(d);(p/'test.c').write_text(source)
  subprocess.run(['cc','-I',str(root/'src'),'-pthread','-fsanitize=address,undefined',str(p/'test.c'),*extra,'-o',str(p/'test')],check=True)
  for a in args:subprocess.run([str(p/'test'),*a],check=True)

src=(root/'src/util/work_trace.c').read_text()
src=src.replace('#include "work_trace.h"', '#include "util/work_trace.h"')
src='\n'.join(l for l in src.splitlines() if not l.startswith('#include <psp2/'))
h=r'''#include <assert.h>
#include <pthread.h>
#include <stdio.h>
static _Thread_local int uid=1;
static unsigned long long now=100;
static int sceKernelGetThreadId(void){return uid;}
static unsigned long long sceKernelGetProcessTimeWide(void){return now;}
'''+src+r'''
static void *worker(void *unused){uid=2;assert(work_trace_begin()==0);work_trace_end(WORK_IO,0,500,99);return NULL;}
int main(void){
 assert(work_trace_begin()==0);work_trace_frame_begin();
 uint64_t t=work_trace_begin();now=140;work_trace_end(WORK_IO,t,10,123);
 t=work_trace_begin();now=150;work_trace_end(WORK_IO,t,5,456);
 t=work_trace_begin();now=200;work_trace_end(WORK_WAIT,t,0,789);
 pthread_t p;assert(!pthread_create(&p,NULL,worker,NULL));pthread_join(p,NULL);
 work_sample s;work_trace_take(&s);
 assert(s.counter[WORK_IO].us==50 && s.counter[WORK_IO].calls==2 && s.counter[WORK_IO].bytes==15);
 assert(s.counter[WORK_IO].longest_us==40 && s.counter[WORK_IO].caller==123);
 assert(s.counter[WORK_WAIT].us==50 && s.counter[WORK_WAIT].caller==789);
 work_trace_frame_begin();work_trace_take(&s);assert(!s.counter[WORK_IO].us && !s.counter[WORK_WAIT].calls);
 t=work_trace_begin();now=1;work_trace_end(WORK_IO,t,0,1);work_trace_take(&s);assert(s.counter[WORK_IO].us==0);
 puts("PASS: per-frame I/O/wait bytes/time/caller, workers excluded, reset and monotonic guard");
}
'''
run(h)

#!/usr/bin/env python3
"""Production telemetry: bounded queues, aggregation, timestamps, JSON and contention."""
from pathlib import Path
import subprocess,tempfile,json
root=Path(__file__).resolve().parents[1]
src=(root/'src/util/observe.c').read_text().replace('#include "observe.h"','#include "util/observe.h"')
src='\n'.join(l for l in src.splitlines() if not l.startswith('#include <psp2/') and l!='#include "log.h"')
h=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <pthread.h>
static uint64_t clock_us=1000;
static _Thread_local int uid=1;
static uint64_t sceKernelGetProcessTimeWide(void){return clock_us;}
static int sceKernelGetThreadId(void){return uid;}
static int sceKernelCreateThread(const char *n,int (*fn)(unsigned,void *),int p,unsigned stack,int a,int mask,void *opt){return -1;}
static int sceKernelStartThread(int id,unsigned args,void *p){return -1;}
static int sceKernelDeleteThread(int id){return 0;}
static int sceKernelDelayThread(unsigned us){return 0;}
static void record(const char *fmt,...);
#define L_INFO(...) record(__VA_ARGS__)
#define L_WARN(...) record(__VA_ARGS__)
'''+src+r'''
static unsigned records;
static void record(const char *fmt,...){
 assert(!gate);char out[1024];va_list ap;va_start(ap,fmt);int n=vsnprintf(out,sizeof(out),fmt,ap);va_end(ap);
 assert(n>0 && n<(int)sizeof(out));puts(out);records++;
}
static void *producer(void *arg){uid=(int)(uintptr_t)arg;for(unsigned i=0;i<2000;i++)observe_span(OBS_FILE,"thread-file",1000,8,0,4);return NULL;}
int main(void){
 observe_span(OBS_RESOURCE,"disabled",1,0,0,0);assert(!head);
 observe_init();assert(head==1);observe_begin_frame(7);
 char file_label[128];
 observe_file_register(4,"asset-A");observe_file_label(4,file_label,sizeof(file_label));assert(!strcmp(file_label,"asset-A"));
 observe_file_register(4,"asset-B");observe_file_label(4,file_label,sizeof(file_label));assert(!strcmp(file_label,"asset-B"));
 observe_file_register(516,"collision");observe_file_forget(4);observe_file_label(4,file_label,sizeof(file_label));assert(strstr(file_label,"handle="));
 observe_file_register(516,"collision-new");observe_file_label(516,file_label,sizeof(file_label));assert(!strcmp(file_label,"collision-new"));
 gate=1;observe_file_forget(516);gate=0;observe_file_label(516,file_label,sizeof(file_label));assert(strstr(file_label,"handle="));
 observe_file_register(8,"known-again");observe_file_label(8,file_label,sizeof(file_label));assert(!strcmp(file_label,"known-again"));

 clock_us=3000;observe_span(OBS_RESOURCE,"res\"\\with\ncontrol",1000,4,1,77);
 for(unsigned i=0;i<50;i++)observe_span(OBS_RESOURCE,"res\"\\with\ncontrol",2900,2,1,77);
 assert(head==2); // first 2ms operation only; tiny repeats aggregate
 asset *found=NULL;for(unsigned i=0;i<ASSETS;i++)if(assets[i].value.count)found=&assets[i];
 assert(found && found->value.count==51 && found->value.total==7000 && found->value.maximum==2000 && found->value.bytes==104);
 assert(queue[1].at==0 && queue[1].duration==2000 && queue[1].frame==7 && queue[1].object==77);
 drain(QUEUE);aggregates(ASSETS);
 observe_span(OBS_CLONE,"prefab",clock_us,0,1,0x89001234u);
 assert(queue[(head-1)%QUEUE].object==0x09001234u && queue[(head-1)%QUEUE].hash<0x40000000u);
 observe_span_at(OBS_GC,"mark_us=123",2000,2400,0,0,0);assert(queue[(head-1)%QUEUE].duration==400);
 observe_phase("MainMenu",0);unsigned epoch=phase;observe_phase("MainMenu",0);assert(phase==epoch);
 for(unsigned i=0;i<10;i++){clock_us+=500000;observe_frame(10000,16000, i==0);}
 frame_flush();assert(!frames);
 drain(QUEUE);
 unsigned d=dropped;gate=1;observe_mark("contended");gate=0;assert(dropped==d+1);
 for(unsigned i=0;i<QUEUE+10;i++)observe_mark("burst");assert(head-tail==QUEUE && dropped>=11);
 drain(QUEUE);
 pthread_t t[8];for(unsigned i=0;i<8;i++)assert(!pthread_create(&t[i],NULL,producer,(void *)(uintptr_t)(i+2)));
 for(unsigned i=0;i<8;i++)pthread_join(t[i],NULL);
 assert(head-tail<=QUEUE);drain(QUEUE);
 char long_name[4096];for(unsigned i=0;i<sizeof(long_name)-1;i++)long_name[i]='x';long_name[4095]=0;
 observe_span(OBS_RESOURCE,long_name,clock_us,0,-1,1);drain(QUEUE);
 for(unsigned i=0;i<ASSETS*2;i++){char label[40];snprintf(label,sizeof(label),"asset-%u",i);observe_span(OBS_RESOURCE,label,clock_us,0,1,i);}
 assert(overflow>0 && head-tail<=QUEUE);
 drain(QUEUE);
 for(unsigned i=0;i<ASSETS;i++)assets[i].dirty=0;
 observe_phase("s_playingstage",1);unsigned before=overflow;
 observe_span(OBS_RESOURCE,"new-phase",clock_us,0,1,1);assert(overflow==before);
 observe_start_writer();assert(!running); // thread API failure doesn't block producers
 drain(QUEUE);
 assert(records>10);
 fprintf(stderr,"PASS: bounded queue/asset table, nonblocking contention/overflow, concurrent producers, per-name totals, phase/frame histogram, JSON escaping, writer-start failure\n");
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.c').write_text(h)
 subprocess.run(['cc','-I',str(root/'src'),'-pthread','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
 result=subprocess.run([str(p/'test')],capture_output=True,text=True,check=True)
 rows=[json.loads(l.split('OBS ',1)[1]) for l in result.stdout.splitlines() if l.startswith('OBS {')]
 assert any(r['type']=='frames' and 'p95_ms=16' in r['name'] for r in rows)
 assert any(r['kind']=='gc' and r['duration_us']==400 for r in rows)
 assert any(r['type']=='asset' and r['count']==51 and r['total_us']==7000 for r in rows)
 assert any('res"\\with?control'==r['name'] for r in rows)
 print(result.stderr.strip())

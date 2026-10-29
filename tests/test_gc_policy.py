#!/usr/bin/env python3
"""Actual APK fingerprint plus bounded, collector-lock-only GC tuning."""
from pathlib import Path
import zipfile,struct,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
with zipfile.ZipFile(root.parent/'srr224.apk') as z:binary=z.read('lib/armeabi-v7a/libmono.so')
ph=struct.unpack_from('<I',binary,28)[0];ents,num=struct.unpack_from('<HH',binary,42)
segments=[struct.unpack_from('<8I',binary,ph+i*ents) for i in range(num)]
def at(va,n):
 for typ,off,addr,phys,sz,memsz,flags,align in segments:
  if typ==1 and addr<=va and va+n<=addr+sz:return binary[off+va-addr:off+va-addr+n]
 raise AssertionError(hex(va))
fp=at(0x2bcb30,0x114)
assert struct.unpack('<I',at(0x3ada10,4))[0]==0x3b22e0
assert struct.unpack('<I',at(0x3b22e0,4))[0]==3
src=(root/'src/mono_probe.c').read_text();a=src.index('static void install_gc_policy(');b=src.index('static int gc_playing_scene',a)
h=r'''
#include "util/gc_policy.h"
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#define MB (1024ULL*1024)
typedef struct {uintptr_t base,end;} so_module;
static int bad_symbol,warns,infos;
static uintptr_t so_symbol(so_module *m,const char *name){assert(!strcmp(name,"mono_gc_collect"));return bad_symbol?0:m->base+0x13f0f4;}
#define L_WARN(...) (warns++)
#define L_INFO(...) (infos++)
'''
h+=src[a:b]
h+='static const unsigned char fingerprint[]={'+','.join(str(x) for x in fp)+'};\n'
h+=r'''
static void *requests(void *arg){for(unsigned i=0;i<100000;i++)gc_policy_request(1,i&1,55*MB,25*MB,64*MB);return NULL;}
int main(void){
 assert(gc_policy_request(1,0,55*MB,25*MB,64*MB)==3);gc_policy_event(0);
 unsigned char *image=calloc(1,0x400000);assert(image);
 so_module m={(uintptr_t)image,(uintptr_t)image+0x400000};
 memcpy(image+0x2bcb30,fingerprint,sizeof(fingerprint));
 uint32_t *setting=(void *)(image+0x3b22e0);*setting=3;
 *(uintptr_t *)(image+0x3ada10)=(uintptr_t)setting;
 uintptr_t end=m.end;m.end=m.base+0x1000;install_gc_policy(&m);assert(!gc_policy_bound());m.end=end;
 *setting=4;install_gc_policy(&m);assert(!gc_policy_bound());*setting=3;
 *(uintptr_t *)(image+0x3ada10)+=4;install_gc_policy(&m);assert(!gc_policy_bound());*(uintptr_t *)(image+0x3ada10)-=4;
 image[0x2bcb60]^=1;install_gc_policy(&m);assert(!gc_policy_bound());image[0x2bcb60]^=1;
 bad_symbol=1;install_gc_policy(&m);assert(!gc_policy_bound());bad_symbol=0;
 install_gc_policy(&m);assert(gc_policy_bound() && warns==5 && infos==1 && *setting==3);
 assert(gc_policy_request(1,0,55*MB,25*MB,64*MB)==2 && *setting==3);
 gc_policy_event(1);assert(*setting==3);gc_policy_event(0);assert(*setting==2 && gc_policy_applied()==2);
 assert(gc_policy_request(1,0,55*MB,25*MB,40*MB)==2);
 assert(gc_policy_request(1,0,55*MB,25*MB,31*MB)==3 && *setting==2);
 gc_policy_event(0);assert(*setting==3);
 assert(gc_policy_request(1,0,55*MB,25*MB,40*MB)==3); // Hysteresis: do not re-enable too early.
 assert(gc_policy_request(1,0,55*MB,25*MB,48*MB)==2);
 assert(gc_policy_request(1,0,55*MB,48*MB,64*MB)==3);
 assert(gc_policy_request(1,0,55*MB,45*MB,64*MB)==3);
 assert(gc_policy_request(1,0,55*MB,43*MB,64*MB)==2);
 assert(gc_policy_request(1,0,97*MB,25*MB,64*MB)==3);
 assert(gc_policy_request(1,1,55*MB,25*MB,64*MB)==3);
 assert(gc_policy_request(0,0,55*MB,25*MB,64*MB)==3);
 assert(gc_policy_request(1,0,55*MB,56*MB,64*MB)==3); // No unsigned underflow.
 pthread_t t;assert(!pthread_create(&t,NULL,requests,NULL));
 for(int i=0;i<100000;i++){gc_policy_event(0);assert(*setting==2 || *setting==3);}
 pthread_join(t,NULL);
 gc_policy_request(0,0,55*MB,25*MB,64*MB);gc_policy_event(0);assert(*setting==3);
 *setting=4;gc_policy_event(0);assert(gc_policy_conflict() && *setting==4 && gc_policy_applied()==4);
 assert(gc_policy_request(1,0,55*MB,25*MB,64*MB)==3);gc_policy_event(0);assert(*setting==4);
 free(image);
 puts("PASS: full APK fingerprint/GOT/default guards, no writes outside GC_START, pressure/loading/heap bounds, hysteresis, concurrent requests, external changes respected");
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.c').write_text(h)
 subprocess.run(['cc','-I',str(root/'src'),'-pthread','-fsanitize=address,undefined',str(p/'test.c'),str(root/'src/util/gc_policy.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)

#!/usr/bin/env python3
"""Count only validated native reserve, keep conservative fallback and bounds."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
def run(source,extra=(),args=((),)):
 with tempfile.TemporaryDirectory() as d:
  p=Path(d);(p/'test.c').write_text(source)
  subprocess.run(['cc','-I',str(root/'src'),'-pthread','-fsanitize=address,undefined',str(p/'test.c'),*extra,'-o',str(p/'test')],check=True)
  for a in args:subprocess.run([str(p/'test'),*a],check=True)


src=(root/'src/util/native_heap.c').read_text().replace('#include "native_heap.h"','#include "util/native_heap.h"')
src='\n'.join(l for l in src.splitlines() if l not in ('#include <malloc.h>','#include <unistd.h>','#include <psp2/kernel/sysmem.h>'))
h=r'''#include "util/gc_policy.h"
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#include <stdio.h>
#define MB (1024ULL*1024)
struct mallinfo{int fordblks;};
static int free_chunks=14*MB,bad;
static uintptr_t heap_base=0x10000000,brk=0x10000000+120*MB;
static unsigned size=220*MB,reported=220*MB;
char *__malloc_sbrk_base=(char *)0x10000000;
static struct mallinfo mallinfo(void){return (struct mallinfo){free_chunks};}
static unsigned _get_vita_heap_size(void){return size;}
static void *sbrk(ptrdiff_t delta){assert(!delta);return (void *)brk;}
typedef struct{unsigned size;void *mappedBase;unsigned mappedSize;} SceKernelMemBlockInfo;
static int sceKernelGetMemBlockInfoByAddr(void *p,SceKernelMemBlockInfo *info){info->mappedBase=(void *)heap_base;info->mappedSize=reported;return bad?-1:0;}
'''+src+r'''
static void fallback(void){native_heap_sample s=native_heap_available();assert(s.total_free==(free_chunks>0?(unsigned)free_chunks:0)&&!s.unused_reserve);}
int main(void){
 native_heap_sample s=native_heap_available();assert(s.chunks_free==14*MB && s.unused_reserve==100*MB-4096 && s.total_free==114*MB-4096);
 uint32_t divisor=3;assert(gc_policy_attach(&divisor));
 assert(gc_policy_request(1,0,55*MB,25*MB,s.total_free)==2);gc_policy_event(0);assert(divisor==2);
 bad=1;fallback();s=native_heap_available();
 assert(gc_policy_request(1,0,55*MB,25*MB,s.total_free)==3);gc_policy_event(0);assert(divisor==3);bad=0;
 reported=219*MB;fallback();reported=220*MB;
 brk=heap_base-1;fallback();brk=heap_base+reported;fallback();brk=(uintptr_t)-1;fallback();
 brk=heap_base+reported-2048;s=native_heap_available();assert(!s.unused_reserve&&s.total_free==14*MB);
 __malloc_sbrk_base=(void *)-1;fallback();__malloc_sbrk_base=0;fallback();
 __malloc_sbrk_base=(void *)(heap_base+reported+1);fallback();
 __malloc_sbrk_base=(void *)heap_base;brk=heap_base+120*MB;free_chunks=-1;s=native_heap_available();assert(!s.chunks_free&&s.total_free==100*MB-4096);
 heap_base=UINTPTR_MAX-1024;__malloc_sbrk_base=(void *)heap_base;fallback();
 puts("PASS: reserved native tail accounted, unrelated blocks rejected, one-page margin, invalid ranges/errors/overflow safely fall back");
}
'''
h=h.replace('static unsigned _get_vita_heap_size','unsigned _get_vita_heap_size').replace('static void *sbrk','void *sbrk')
run(h,extra=(str(root/'src/util/gc_policy.c'),))

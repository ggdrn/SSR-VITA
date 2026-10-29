#!/usr/bin/env python3
"""Exercise real Unity allocation recovery with distinct memory ownership."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1];src=(root/'src/bridges/format_bridge.c').read_text()
a=src.index('#define UNITY_SPILL_SLOTS');b=src.index('static int bad_free_reports;',a)
c=src.index('void free_bridge(void *ptr)');d=src.index('void log_heap_usage',c)
h=r'''
#define _POSIX_C_SOURCE 200112L
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
typedef enum { VGL_MEM_PHYCONT,VGL_MEM_RAM,VGL_MEM_VRAM } vglMemType;
typedef struct {const char *name;} so_module;
static int fail_native,allow_pool=1,is_unity=1,errors,native_frees,pool_frees,pool_attempts;
static struct {void *raw,*base;size_t size;} blocks[128];
static void *vglAlloc(uint32_t size,vglMemType pool){
 pool_attempts++;if(!allow_pool || pool!=VGL_MEM_PHYCONT)return NULL;
 for(int i=0;i<128;i++)if(!blocks[i].raw){
  void *raw=malloc(size+3);assert(raw);blocks[i].raw=raw;blocks[i].base=(char *)raw+3;blocks[i].size=size;
  return blocks[i].base;
 }
 assert(0);return NULL;
}
static void vglFree(void *base){
 for(int i=0;i<128;i++)if(blocks[i].base==base){assert(blocks[i].raw);free(blocks[i].raw);memset(&blocks[i],0,sizeof(blocks[i]));pool_frees++;return;}
 assert(!"wrong pool free");
}
static void *native_memalign(size_t align,size_t size){
 if(fail_native)return NULL;void *p=NULL;return posix_memalign(&p,align,size)?NULL:p;
}
static void native_free(void *p){
 for(int i=0;i<128;i++)assert(!blocks[i].raw || (uintptr_t)p<(uintptr_t)blocks[i].base || (uintptr_t)p>=(uintptr_t)blocks[i].base+blocks[i].size);
 native_frees++;free(p);
}
static int reject_free(const char *what,void *p,void *caller){return 0;}
static void alloc_failed(const char *what,size_t size,size_t align,void *caller){errors++;}
static so_module *so_module_at(uintptr_t caller){static so_module unity={"libunity.so"},other={"libmono.so"};return is_unity?&unity:&other;}
#define L_WARN(...) ((void)0)
#define free native_free
#define memalign native_memalign
'''
h+=src[a:b]+src[c:d]
h+=r'''
static void *worker(void *arg){
 for(int n=0;n<40;n++){
  unsigned char *p=memalign_bridge(64,1048576);assert(p && !((uintptr_t)p&63));p[0]=n;
  assert(p[0]==n);free_bridge(p);
 }return NULL;
}
int main(void){
 void *p=memalign_bridge(64,1048576);assert(p && !unity_spill_live);free_bridge(p);assert(native_frees==1);
 fail_native=1;unsigned char *s=memalign_bridge(4096,1048576);assert(s && !((uintptr_t)s&4095));
 assert(unity_spill_live==1);memset(s,0x6b,1048576);
 s=realloc_bridge(s,2097152);assert(s && unity_spill_live==1 && pool_frees==1);
 for(int i=0;i<1048576;i++)assert(s[i]==0x6b);
 assert(realloc_bridge(s,524288)==s);
 allow_pool=0;assert(!realloc_bridge(s,3145728) && unity_spill_live==1 && s[0]==0x6b);
 allow_pool=1;fail_native=0;
 unsigned char *normal=realloc_bridge(s,3145728);assert(normal && !unity_spill_live && pool_frees==2);
 assert(normal[0]==0x6b);free_bridge(normal);
 fail_native=1;p=memalign_bridge(16,1048576);assert(p);assert(!realloc_bridge(p,0) && !unity_spill_live);
 int before=pool_attempts;is_unity=0;assert(!memalign_bridge(16,1048576) && pool_attempts==before);is_unity=1;
 assert(!memalign_bridge(3,1048576) && !memalign_bridge(16,SIZE_MAX) && pool_attempts==before);
 void *many[32];for(int i=0;i<32;i++){many[i]=memalign_bridge(16,1048576);assert(many[i]);}
 before=pool_attempts;assert(!memalign_bridge(16,1048576) && pool_attempts==before);
 for(int i=0;i<32;i++)free_bridge(many[i]);assert(!unity_spill_live);
 pthread_t threads[8];for(int i=0;i<8;i++)assert(!pthread_create(&threads[i],NULL,worker,NULL));
 for(int i=0;i<8;i++)pthread_join(threads[i],NULL);
 assert(!unity_spill_live);for(int i=0;i<128;i++)assert(!blocks[i].raw);
 puts("PASS: Unity-only recovery, alignment, correct free ownership, realloc preservation/failure/zero, table bounds, 8 concurrent users");
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.c').write_text(h)
 subprocess.run(['cc','-pthread','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)

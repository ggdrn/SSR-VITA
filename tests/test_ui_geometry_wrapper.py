#!/usr/bin/env python3
"""Exercise the real geometry wrapper's guards, counts and original fallback."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
src=(root/'src/util/ui_geometry.c').read_text();src=src[src.index('typedef void (*geometry_fn)'):src.index('void ui_geometry_install')]
harness=r'''
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
#include <sys/types.h>
#include "util/ui_geometry_copy.h"
struct list { unsigned char header[8]; void *buffer; int used; } source_list[3],target_list[3];
static void *classes[3]={(void *)1,(void *)2,(void *)3};
static unsigned forwarded;
static void *obj_class(void *list) {
 for(unsigned i=0;i<3;i++)if(list==&source_list[i]||list==&target_list[i])return classes[i];
 return NULL;
}
static void fallback(void *self,void *v,void *u,void *c,void *n,void *t) {
 assert(v==&target_list[0] && u==&target_list[1] && c==&target_list[2]);forwarded++;
}
'''+src+r'''
int main(void) {
 original=fallback;object_class=obj_class;void *geometry[3];int stride[3]={12,8,4};
 for(unsigned i=0;i<3;i++) {
  geo_offset[i]=sizeof(void *)*i;buffer_offset[i]=offsetof(struct list,buffer);size_offset[i]=offsetof(struct list,used);list_class[i]=classes[i];
  source_list[i].buffer=calloc(1,16+16*stride[i]);target_list[i].buffer=calloc(1,16+16*stride[i]);
  *(int32_t *)((char *)source_list[i].buffer+12)=16;*(int32_t *)((char *)target_list[i].buffer+12)=16;
  source_list[i].used=4;target_list[i].used=3;geometry[i]=&source_list[i];
  memset((char *)source_list[i].buffer+16,0x10+i,4*stride[i]);
 }
 geometry_copy(geometry,&target_list[0],&target_list[1],&target_list[2],NULL,NULL);
 assert(fast_calls==1 && !forwarded);
 for(int i=0;i<3;i++){assert(target_list[i].used==7);assert(!memcmp((char *)target_list[i].buffer+16+3*stride[i],(char *)source_list[i].buffer+16,4*stride[i]));}
 geometry_copy(geometry,&target_list[0],&target_list[1],&target_list[2],(void *)1,(void *)2);assert(forwarded==1);
 *(int32_t *)((char *)target_list[2].buffer+12)=7;
 geometry_copy(geometry,&target_list[0],&target_list[1],&target_list[2],NULL,NULL);assert(forwarded==2);
 for(int i=0;i<3;i++)assert(target_list[i].used==7);
 source_list[0].used=0;geometry_copy(geometry,&target_list[0],&target_list[1],&target_list[2],NULL,NULL);assert(fast_calls==2 && forwarded==2);
 assert(safe_prologue(0xe1a0c00d));assert(safe_prologue(0xe92d59f0));assert(safe_prologue(0xe92d48f0));assert(safe_prologue(0xe24dd080));assert(safe_prologue(0xe58de004));
 assert(!safe_prologue(0xe59f0010));assert(!safe_prologue(0xea000001));assert(!safe_prologue(0xe92d8000));assert(!safe_prologue(0xe58df004));
 for(int i=0;i<3;i++){free(source_list[i].buffer);free(target_list[i].buffer);}
 puts("PASS: real UI geometry wrapper: exact append/counts, capacity and normals fallback, empty geometry, conservative ARM trampoline guard");
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.c').write_text(harness)
 subprocess.run(['cc','-fsanitize=address,undefined','-I',str(root/'src'),str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)

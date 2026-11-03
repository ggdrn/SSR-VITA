#!/usr/bin/env python3
"""Validate preflight of paired parser IL edits and original-path fallback."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
src='\n'.join(l for l in (root/'src/util/stage_parse.c').read_text().splitlines() if not l.startswith('#include'))
harness=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "util/stage_parse_il.h"
#define DATA_PATH "/not-a-real-directory"
#define L_INFO(...) ((void)0)
#define L_WARN(...) ((void)0)
typedef void so_module;
static int mode,writes,flushes,releases;
static uint8_t objects[sizeof(stage_objects_il)],layer[sizeof(stage_layer_il)];
static FILE *marker_open(const char *n,const char *m){return mode==1?tmpfile():NULL;}
#define fopen marker_open
static so_module *so_find_module(const char *n){return mode==12?NULL:(void *)1;}
static const char *get_guid(void *im){return mode==2?"wrong":STAGE_PARSE_MVID;}
static void *get_method(void *im,uint32_t token,void *klass){
 assert(im==(void *)99&&!klass);
 if(token==0x06003c18)return mode==10?NULL:(void *)1;
 assert(token==0x06004c80);return (void *)2;
}
static void *get_header(void *m){return mode==11&&m==(void *)2?NULL:m;}
static const uint8_t *get_code(void *h,uint32_t *n,uint32_t *s){
 int i=h==(void *)1?0:1;*n=i?sizeof(layer):sizeof(objects);*s=i?18:77;
 if((mode==6&&!i)||(mode==7&&i))(*n)--;
 if((mode==8&&!i)||(mode==9&&i))*s=7;
 return i?layer:objects;
}
static void free_header(void *h){assert(h);releases++;}
static uintptr_t so_symbol(so_module *m,const char *n){
 if(mode==3&&!strcmp(n,"mono_method_get_header"))return 0;
 #define API_ENTRY(name,fn) if(!strcmp(n,name))return (uintptr_t)fn
 API_ENTRY("mono_image_get_guid",get_guid);API_ENTRY("mono_get_method",get_method);
 API_ENTRY("mono_method_get_header",get_header);API_ENTRY("mono_method_header_get_code",get_code);
 API_ENTRY("mono_metadata_free_mh",free_header);assert(0);return 0;
}
static void kuKernelCpuUnrestrictedMemcpy(void *dst,const void *src,unsigned n){
 if(!writes){assert(dst==objects+STAGE_PARSE_LOOP_AT&&n==STAGE_PARSE_LOOP_SIZE);for(unsigned i=0;i<n;i++)assert(!((const uint8_t *)src)[i]);}
 else assert(writes==1&&dst==layer&&n==sizeof(stage_layer_batched_il));
 writes++;memcpy(dst,src,n);
}
static void kuKernelFlushCaches(void *dst,unsigned n){assert(writes==2);flushes++;}
'''
tests=r'''
int main(void){
 for(int m=0;m<=12;m++){
  mode=m;attempted=writes=flushes=releases=0;
  memcpy(objects,stage_objects_il,sizeof(objects));memcpy(layer,stage_layer_il,sizeof(layer));
  if(m==4)objects[25]^=1;if(m==5)layer[25]^=1;
  uint8_t before_objects[sizeof(objects)],before_layer[sizeof(layer)];memcpy(before_objects,objects,sizeof(objects));memcpy(before_layer,layer,sizeof(layer));
  stage_parse_install(NULL);assert(!attempted);
  stage_parse_install((void *)99);
  if(m==0){
   assert(writes==2&&flushes==2&&releases==2);
   for(unsigned i=0;i<sizeof(objects);i++)assert(objects[i]==(i>=0x298&&i<0x2a8?0:before_objects[i]));
   assert(!memcmp(layer,stage_layer_batched_il,sizeof(layer)));
   stage_parse_install((void *)99);assert(writes==2&&releases==2);
  }else{
   assert(!writes&&!flushes&&!memcmp(objects,before_objects,sizeof(objects))&&!memcmp(layer,before_layer,sizeof(layer)));
  }
 }
 puts("PASS: paired parser guards, rollback, both-method preflight, exact write spans, cache flush and idempotence (ASan/UBSan)");
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.c').write_text(harness+src+tests)
 subprocess.run(['cc','-fsanitize=address,undefined','-I',str(root/'src'),str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)

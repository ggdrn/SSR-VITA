#!/usr/bin/env python3
"""Memoized ZIP offsets must preserve reads, bounds and concurrent callers."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
def run(source,extra=(),args=((),)):
 with tempfile.TemporaryDirectory() as d:
  p=Path(d);(p/'test.c').write_text(source)
  subprocess.run(['cc','-I',str(root/'src'),'-pthread','-fsanitize=address,undefined',str(p/'test.c'),*extra,'-o',str(p/'test')],check=True)
  for a in args:subprocess.run([str(p/'test'),*a],check=True)

src=(root/'src/util/apk.c').read_text()
a=src.index('int64_t apk_data_offset(');b=src.index('void *apk_extract(',a)
c=src.index('int apk_read_stored(')
h=r'''#include "util/apk.h"
#define title_layout_apply(...) ((void)0)
#define splash_art_apply(...) ((void)0)
#include "util/observe.h"
#include <stdint.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
uint64_t observe_now(void){return 1;}
void observe_span(unsigned k,const char *n,uint64_t s,uint64_t b,int status,uintptr_t o){}
static unsigned char file[256];
static int headers,fail;
static uint16_t rd16(const uint8_t *p){return p[0]|p[1]<<8;}
static uint32_t rd32(const uint8_t *p){return p[0]|p[1]<<8|p[2]<<16|(uint32_t)p[3]<<24;}
static int pread_all(void *dst,uint32_t len,int64_t off){
 if(len==30){__atomic_fetch_add(&headers,1,__ATOMIC_RELAXED);if(fail)return -1;}
 if(off<0 || off+len>256)return -1;
 memcpy(dst,file+off,len);return 0;
}
'''+src[a:b]+src[c:]+r'''
static apk_entry e={.local_offset=16,.size=20,.method=0};
static void *worker(void *unused){for(int i=0;i<1000;i++){char b[20];assert(apk_read_stored(&e,0,b,20)==20);assert(!memcmp(b,file+53,20));}return NULL;}
int main(void){
 file[16]=0x50;file[17]=0x4b;file[18]=3;file[19]=4;file[42]=5;file[44]=2;
 for(int i=53;i<73;i++)file[i]=i;
 fail=1;assert(apk_data_offset(&e)==-1 && e.data_offset_cache==0);
 fail=0;file[16]=0;assert(apk_data_offset(&e)==-1 && e.data_offset_cache==0);file[16]=0x50;
 assert(apk_data_offset(&e)==53);int initial=headers;
 pthread_t t[4];for(int i=0;i<4;i++)pthread_create(&t[i],NULL,worker,NULL);
 for(int i=0;i<4;i++)pthread_join(t[i],NULL);
 assert(headers==initial);char b[32];assert(apk_read_stored(&e,19,b,UINT32_MAX)==1 && b[0]==72);
 assert(apk_read_stored(&e,21,b,1)==-1);e.method=8;assert(apk_read_stored(&e,0,b,1)==-1);
 apk_entry large={.local_offset=UINT32_MAX-5};assert(apk_data_offset(&large)==-1 && !large.data_offset_cache);
 puts("PASS: offset reused across 4000 concurrent reads, failed headers uncached, data and overflow bounds preserved");
}
'''
run(h)

#!/usr/bin/env python3
"""Check actual range selection and reading against every supplied bundle."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
harness=r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "util/log.h"
#undef L_INFO
#define L_INFO(...) ((void)0)
#include "util/bundle_mips.c"
int main(int argc,char **argv) {
 unsigned files=0,ranges=0;
 for(int arg=1;arg<argc;arg++) {
  FILE *f=fopen(argv[arg],"rb");assert(f);fseek(f,0,SEEK_END);long total=ftell(f);rewind(f);
  uint8_t *full=malloc(total),*actual=malloc(total),*expected=malloc(total);assert(full&&actual&&expected);
  assert(fread(full,1,total,f)==(size_t)total);rewind(f);
  mip_range *selected=NULL;unsigned count=parse_ranges(f,total,&selected);rewind(f);
  // Every selected range belongs to a named UI texture, not gameplay data.
  unsigned data=be32(full+12);uint8_t *meta=full+20,*p=memchr(meta,0,32);assert(p);p++;
  unsigned objects=le32(p+12);p+=16;
  for(unsigned i=0;i<count;i++) {
   int matched=0;
   for(unsigned j=0;j<objects;j++) {
    uint8_t *row=p+j*20;unsigned off=data+le32(row+4),sz=le32(row+8);
    if(selected[i].start>=off && selected[i].end<=off+sz) {
     unsigned n=le32(full+off);uint8_t *name=full+off+4;
     assert((n>=3&&!memcmp(name,"ui_",3))||(n>=9&&!memcmp(name,"UCGothic_",9)));matched=1;break;
    }
   }
   assert(matched);
  }
  bundle_mips_open(f,argv[arg]);bundle_mips_set_limit(1);
  size_t block_sizes[]={65536,1048576};
  for(unsigned b=0;b<2;b++) {
   rewind(f);
   for(uint32_t pos=0;pos<(unsigned)total;) {
    size_t n=block_sizes[b];if(n>(unsigned)total-pos)n=total-pos;memset(actual,0xcd,n);memcpy(expected,full+pos,n);
    size_t got=0;int split=bundle_mips_fread(actual,1,n,f,&got);
    if(!split)got=fread(actual,1,n,f);
    assert(got==n);
    if(split)for(unsigned i=0;i<count;i++) {
     unsigned at=selected[i].start>pos?selected[i].start:pos, end=selected[i].end<pos+n?selected[i].end:pos+n;
     if(end>at)memset(expected+at-pos,0,end-at);
    }
    assert(!memcmp(actual,expected,n));pos+=n;
   }
  }
  bundle_mips_set_limit(0);rewind(f);size_t got=0;assert(!bundle_mips_fread(actual,1,total,f,&got));assert(fread(actual,1,total,f)==(size_t)total);assert(!memcmp(full,actual,total));
  bundle_mips_close(f);fclose(f);files++;ranges+=count;free(selected);free(full);free(actual);free(expected);
 }
 printf("PASS: %u real bundles, %u UI mip ranges; all other bytes identical; skipped bytes zero; limit 0 preserves all data\n",files,ranges);
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.c').write_text(harness)
 subprocess.run(['cc','-O1','-fsanitize=address,undefined','-I',str(root/'src'),str(p/'test.c'),'-o',str(p/'test')],check=True)
 files=sorted((root.parent/'data do jogo/files/UnityCache').rglob('*.sharedAssets'))
 assert files,'Real cache required'
 subprocess.run([str(p/'test'),*map(str,files)],check=True)

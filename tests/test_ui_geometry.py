#!/usr/bin/env python3
"""Compare the bounded NGUI copies to per-element appends, including guards."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
harness=r'''
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include "util/ui_geometry_copy.h"
static uint32_t rng=123;
static uint32_t random_value(void) { rng=rng*1664525u+1013904223u; return rng; }
int main(void) {
 const int stride[3]={12,8,4};
 for (int round=0;round<500;round++) {
  unsigned n=random_value()%500, used=random_value()%40;
  ui_copy_list src[3],dst[3];unsigned char expected[3][7000],data[3][7000],source[3][7000];
  for (int k=0;k<3;k++) {
   for (int b=0;b<7000;b++) { source[k][b]=random_value();data[k][b]=random_value(); }
   memcpy(expected[k],data[k],7000);
   src[k]=(ui_copy_list){source[k],n,n};dst[k]=(ui_copy_list){data[k],used,550};
   for(unsigned i=0;i<n;i++)memcpy(expected[k]+(used+i)*stride[k],source[k]+i*stride[k],stride[k]);
  }
  assert(ui_copy_geometry(src,dst,n));
  for(int k=0;k<3;k++){assert(dst[k].used==(int)(used+n));assert(!memcmp(expected[k],data[k],7000));}
 }
 unsigned char s[3][128]={{0}},d[3][128]={{0}},before[3][128];
 ui_copy_list src[3]={{s[0],4,4},{s[1],4,4},{s[2],4,4}};
 ui_copy_list dst[3]={{d[0],0,4},{d[1],0,4},{d[2],0,3}};
 memcpy(before,d,sizeof(d));assert(!ui_copy_geometry(src,dst,4));assert(!memcmp(before,d,sizeof(d)));
 dst[2].capacity=4;src[1].capacity=3;assert(!ui_copy_geometry(src,dst,4));assert(!memcmp(before,d,sizeof(d)));
 src[1].capacity=4;dst[1].data=s[0];assert(!ui_copy_geometry(src,dst,4));assert(!memcmp(before,d,sizeof(d)));
 dst[1].data=d[1];dst[0].used=INT_MAX;assert(!ui_copy_geometry(src,dst,4));assert(!memcmp(before,d,sizeof(d)));
 assert(ui_copy_geometry(src,dst,0));assert(!ui_copy_geometry(src,dst,-1));
 puts("PASS: NGUI bulk copies byte-identical to scalar appends; capacity/source/alias/overflow guards make no partial writes");
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.c').write_text(harness)
 subprocess.run(['cc','-fsanitize=address,undefined','-I',str(root/'src'),str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)

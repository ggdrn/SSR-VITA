#!/usr/bin/env python3
"""Read-only explicit-request attribution: real ELF guard and mapped ranges."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
def run(source,extra=(),args=((),)):
 with tempfile.TemporaryDirectory() as d:
  p=Path(d);(p/'test.c').write_text(source)
  subprocess.run(['cc','-I',str(root/'src'),'-pthread','-fsanitize=address,undefined',str(p/'test.c'),*extra,'-o',str(p/'test')],check=True)
  for a in args:subprocess.run([str(p/'test'),*a],check=True)


import zipfile,struct
with zipfile.ZipFile(root.parent/'srr224.apk') as z:binary=z.read('lib/armeabi-v7a/libmono.so')
ph=struct.unpack_from('<I',binary,28)[0];ents,num=struct.unpack_from('<HH',binary,42)
def elf_at(va,n):
 for i in range(num):
  typ,off,addr,phys,sz,memsz,flags,align=struct.unpack_from('<8I',binary,ph+i*ents)
  if typ==1 and addr<=va and va+n<=addr+sz:return binary[off+va-addr:off+va-addr+n]
 raise AssertionError(hex(va))
fp=elf_at(0x13f0f4,56)
# PC-relative slot calculation must match the actual ARM instruction/literal.
assert 0x13f110+int.from_bytes(fp[-4:],'little')==0x3b4f70
src=(root/'src/mono_probe.c').read_text();a=src.index('static int gc_manual_requests(');b=src.index('// Native counters',a)
h=r'''#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
typedef struct{uintptr_t base,end;} so_module;
typedef struct{unsigned size;} SceKernelMemBlockInfo;
static so_module module;
static int bad_symbol,bad_range,queries;
static uintptr_t stats_expected;
static so_module *so_find_module(const char *n){return &module;}
static uintptr_t so_symbol(so_module *m,const char *n){return bad_symbol?0:m->base+0x13f0f4;}
static int sceKernelGetMemBlockInfoByRange(void *p,unsigned size,SceKernelMemBlockInfo *info){assert((uintptr_t)p==stats_expected && size==0x60);queries++;return bad_range?-1:0;}
#define L_INFO(...) ((void)0)
#define L_WARN(...) ((void)0)
'''+src[a:b]
h+='static const unsigned char fingerprint[]={'+','.join(str(x) for x in fp)+'};\n'
h+=r'''
int main(int argc,char **argv){
 int mode=argc>1?atoi(argv[1]):0;
 unsigned char *image=calloc(1,0x400000);assert(image);
 module=(so_module){(uintptr_t)image,(uintptr_t)image+0x400000};
 memcpy(image+0x13f0f4,fingerprint,56);
 uint32_t out=0;
 if(mode==1)bad_symbol=1;
 if(mode==2)image[0x13f100]^=1;
 if(mode==3)module.end=module.base+0x3b4f70;
 if(mode){assert(!gc_manual_requests(&out));free(image);return 0;}
 assert(!gc_manual_requests(&out));
 unsigned char stats[0x60]={0},stats2[0x60]={0};stats_expected=(uintptr_t)stats;
 *(uintptr_t *)(image+0x3b4f70)=stats_expected;
 uint32_t *counter=(void *)(stats+0x5c);*counter=42;
 bad_range=1;assert(!gc_manual_requests(&out));bad_range=0;
 assert(gc_manual_requests(&out)&&out==42);int q=queries;
 *counter=43;assert(gc_manual_requests(&out)&&out==43&&queries==q&&*counter==43);
 stats_expected=(uintptr_t)stats2;*(uintptr_t *)(image+0x3b4f70)=stats_expected;
 *(uint32_t *)(stats2+0x5c)=UINT32_MAX;assert(gc_manual_requests(&out)&&out==UINT32_MAX&&queries==q+1);
 assert(!memcmp(image+0x13f0f4,fingerprint,56));
 free(image);puts("PASS: real ARM fingerprint/slot, invalid layouts/ranges rejected, lazy pointer revalidation, read-only counter");
}
'''
run(h,args=((),('1',),('2',),('3',)))

#!/usr/bin/env python3
"""Execute exact IL edits; preserve control flow, gameplay and EH boundaries."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
def run(source,extra=(),args=((),)):
 with tempfile.TemporaryDirectory() as d:
  p=Path(d);(p/'test.c').write_text(source)
  subprocess.run(['cc','-I',str(root/'src'),'-pthread','-fsanitize=address,undefined',str(p/'test.c'),*extra,'-o',str(p/'test')],check=True)
  for a in args:subprocess.run([str(p/'test'),*a],check=True)

import json,zipfile,hashlib
fixture=json.loads((root/'tests/fixtures/spawn_il.json').read_text())
with zipfile.ZipFile(root.parent/'srr224.apk') as z:
 assert hashlib.sha256(z.read('assets/bin/Data/Managed/Assembly-CSharp.dll')).hexdigest()==fixture['assembly_sha256']
 core=z.read('assets/bin/Data/Managed/mscorlib.dll')
 # Dispose clears the boxed COPY's reference; List ctor only initializes that new list.
 assert bytes.fromhex('02147D5F01000A2A') in core
 assert bytes.fromhex('022801000006027E2001000A7D2101000A2A') in core
src=(root/'src/mono_probe.c').read_text();a=src.index('static void patch_spawn_il(');b=src.index('// Avoid per-effect',a)
h=r'''#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef void MonoClass,MonoMethod;typedef int so_module;
static int game_image,freed,copies;
'''
for i,name in enumerate(('CheckRangeIn','CheckRangeOut')):
 d=bytes.fromhex(fixture['methods'][name]);h+=f'static const uint8_t old{i}[]={{'+','.join(map(str,d))+'};\n'+f'static uint8_t code{i}[sizeof(old{i})];\n'
for i,spans in enumerate((((0xa6,11),(0xee,12)),((0x96,11),(0xde,12)))):
 d=bytes.fromhex(fixture['methods'][('CheckRangeIn','CheckRangeOut')[i]])
 for off,n in spans:
  assert d[off+n]==0xdc
  assert d[off+n-5:off+n]==bytes.fromhex('6FA200000A')
  assert bytes.fromhex('8CC401001B') in d[off:off+n]
h+=r'''
static MonoClass *klass(int img,const char *ns,const char *name){assert(!strcmp(name,"ObjectSpawnManager"));return (void *)1;}
static MonoMethod *method(MonoClass *c,const char *name,int n){assert(n==1);return !strcmp(name,"CheckRangeIn")?(void *)1:(void *)2;}
static struct {MonoClass *(*class_from_name)(int,const char *,const char *);MonoMethod *(*get_method)(MonoClass *,const char *,int);} api={klass,method};
static void *header(MonoMethod *m){return m;}
static const uint8_t *code(void *h,uint32_t *size,uint32_t *stack){*stack=8;*size=h==(void *)1?sizeof(code0):sizeof(code1);return h==(void *)1?code0:code1;}
static void release(void *h){freed++;}
static so_module *so_find_module(const char *s){static so_module m;return &m;}
static uintptr_t so_symbol(so_module *m,const char *s){if(!strcmp(s,"mono_method_get_header"))return (uintptr_t)header;if(!strcmp(s,"mono_method_header_get_code"))return (uintptr_t)code;return (uintptr_t)release;}
static void kuKernelCpuUnrestrictedMemcpy(void *d,const void *s,size_t n){copies++;memcpy(d,s,n);}
static void kuKernelFlushCaches(void *d,size_t n){}
#define L_INFO(...) ((void)0)
#define L_WARN(...) ((void)0)
'''+src[a:b]+r'''
static void reset(void){memcpy(code0,old0,sizeof(code0));memcpy(code1,old1,sizeof(code1));copies=freed=0;}
int main(void){
 reset();patch_spawn_il();assert(copies==6 && freed==2);
 for(unsigned i=0;i<sizeof(code0);i++)assert(code0[i]==((i>=0x6c&&i<0x72)||(i>=0xa6&&i<0xb1)||(i>=0xee&&i<0xfa)?0:old0[i]));
 for(unsigned i=0;i<sizeof(code1);i++)assert(code1[i]==((i>=0x6b&&i<0x71)||(i>=0x96&&i<0xa1)||(i>=0xde&&i<0xea)?0:old1[i]));
 reset();code0[10]^=1;code1[20]^=1;patch_spawn_il();assert(copies==0 && freed==2);
 puts("PASS: original APK guards, exact dead allocation edits, finally terminators/branches/gameplay bytes unchanged");
}
'''
run(h)

#!/usr/bin/env python3
"""Execute the real IL patch and validate original/patched effect playback."""
from pathlib import Path
import json, subprocess, tempfile, hashlib, zipfile
root=Path(__file__).resolve().parents[1]
src=(root/'src/mono_probe.c').read_text()
a=src.index('static void patch_perf_il(void)'); b=src.index('// Called between frames',a)
fixture=json.loads((root/'tests/fixtures/perf_il.json').read_text())
with zipfile.ZipFile(root.parent/'srr224.apk') as apk:
    assert hashlib.sha256(apk.read('assets/bin/Data/Managed/Assembly-CSharp.dll')).hexdigest()==fixture['assembly_sha256']
fixtures=fixture['methods']
names=['.SoundManager.SePlay','.GameModeStage.StatePause','.GameModeStage.StateChangeLevel']
data=[bytes.fromhex(fixtures[n]) for n in names]
h=r'''
#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>
typedef void MonoMethod,MonoClass;
typedef int so_module;
static int game_image, copies, freed;
'''
for i,d in enumerate(data):
    h+=f'static const uint8_t original{i}[]={{'+','.join(str(x) for x in d)+'};\n'
    h+=f'static uint8_t code{i}[sizeof(original{i})];\n'
h+=r'''
static MonoClass *klass(int image,const char *ns,const char *name){return (void *)name;}
static MonoMethod *method(MonoClass *cls,const char *name,int nargs){
 if(!strcmp(name,"SePlay")){assert(nargs==2);return (void *)1;}
 if(!strcmp(name,"StatePause")){assert(nargs==1);return (void *)2;}
 assert(!strcmp(name,"StateChangeLevel") && nargs==1);return (void *)3;
}
static struct { MonoClass *(*class_from_name)(int,const char *,const char *);
 MonoMethod *(*get_method)(MonoClass *,const char *,int); } api={klass,method};
static void *header(MonoMethod *m){return m;}
static const uint8_t *code(void *h,uint32_t *size,uint32_t *stack){
 *stack=8;
 if(h==(void *)1){*size=sizeof(code0);return code0;}
 if(h==(void *)2){*size=sizeof(code1);return code1;}
 *size=sizeof(code2);return code2;
}
static void release(void *h){freed++;}
static so_module module;
static so_module *so_find_module(const char *s){return &module;}
static uintptr_t so_symbol(so_module *m,const char *s){
 if(!strcmp(s,"mono_method_get_header"))return (uintptr_t)header;
 if(!strcmp(s,"mono_method_header_get_code"))return (uintptr_t)code;
 assert(!strcmp(s,"mono_metadata_free_mh"));return (uintptr_t)release;
}
static void kuKernelFlushCaches(void *p,size_t n){}
static void kuKernelCpuUnrestrictedMemcpy(void *p,const void *q,size_t n){memcpy(p,q,n);copies++;}
#define L_INFO(...) ((void)0)
#define L_WARN(...) ((void)0)
'''
h+=src[a:b]
h+=r'''
static void reset(void){memcpy(code0,original0,sizeof(code0));memcpy(code1,original1,sizeof(code1));memcpy(code2,original2,sizeof(code2));}
static void check(const uint8_t *now,const uint8_t *old,unsigned n,unsigned a,unsigned b){
 for(unsigned i=0;i<n;i++)assert(now[i]==(i>=a && i<b ? 0 : old[i]));
}
int main(int argc,char **argv){
 reset();patch_perf_il();assert(copies==1 && freed==1);
 check(code0,original0,sizeof(code0),0x12,0x42);
 check(code1,original1,sizeof(code1),0,0);
 check(code2,original2,sizeof(code2),0,0);
 FILE *out=fopen(argv[1],"wb");assert(out);fwrite(code0,1,sizeof(code0),out);fclose(out);
 reset();code0[1]^=1;code1[0x78]^=1;code2[0xc7]^=1;copies=0;
 patch_perf_il();assert(copies==0 && freed==2);
 puts("PASS: exact IL guards, only debug-building removed; all transition GC/unload bytes preserved");
}
'''
def execute(code, instance):
    stack=[]; pc=0; allocated=0; played=[]
    while pc<len(code):
        op=code[pc];pc+=1
        if op==0:continue
        if op==0x7e:pc+=4;stack.append(instance)
        elif op==0x14:stack.append(None)
        elif op==0x39:
            d=int.from_bytes(code[pc:pc+4],'little',signed=True);pc+=4
            if not stack.pop():pc+=d
        elif op==0x28:
            tok=int.from_bytes(code[pc:pc+4],'little');pc+=4
            if tok==0x0a000028:
                b=stack.pop();a=stack.pop();stack.append(a==b)
            elif tok==0x0a000044:
                arr=stack.pop();stack.append(str(arr));allocated+=1
            elif tok==0x06003b92:stack.pop()
            elif tok==0x06003b77:
                args=stack[-4:];del stack[-4:];played.append(args);stack.append(777)
            else:raise AssertionError(hex(tok))
        elif 0x16<=op<=0x1b:stack.append(op-0x16)
        elif op==0x8d:
            pc+=4;stack.append([None]*stack.pop());allocated+=1
        elif op==0x25:stack.append(stack[-1])
        elif op==0x72:
            stack.append(int.from_bytes(code[pc:pc+4],'little'));pc+=4
        elif op in (2,3):stack.append('cue' if op==2 else 'SE')
        elif op==0xa2:
            value=stack.pop();idx=stack.pop();arr=stack.pop();arr[idx]=value
        elif op==0x2a:
            assert len(stack)==1
            return stack[0],allocated,played
        else:raise AssertionError(hex(op))
    raise AssertionError('no return')
with tempfile.TemporaryDirectory() as d:
    p=Path(d);(p/'test.c').write_text(h)
    subprocess.run(['cc','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test'),str(p/'patched')],check=True)
    patched=(p/'patched').read_bytes()
    old=execute(data[0],True);new=execute(patched,True)
    assert old==(777,2,[[2,'cue','SE',0]]) and new==(777,0,old[2])
    assert execute(data[0],None)==execute(patched,None)==(0,0,[])
print('PASS: effect arguments/result preserved; debug array/string allocations drop from 2 to 0')

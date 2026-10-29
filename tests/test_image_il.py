#!/usr/bin/env python3
"""Run real image IL patches; simulate fresh Texture2D on every WWW access."""
from pathlib import Path
import json,hashlib,zipfile,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
fixture=json.loads((root/'tests/fixtures/image_il.json').read_text())
with zipfile.ZipFile(root.parent/'srr224.apk') as apk:
 assert hashlib.sha256(apk.read('assets/bin/Data/Managed/Assembly-CSharp.dll')).hexdigest()==fixture['assembly_sha256']
methods=fixture['methods']; original=[bytes.fromhex(m['original']) for m in methods]
expected=[]
for m,old in zip(methods,original):
 data=bytearray(old)
 for p in m['patches']:
  new=bytes.fromhex(p['replacement']);data[p['offset']:p['offset']+len(new)]=new
 expected.append(bytes(data))
src=(root/'src/mono_probe.c').read_text()
assert '  patch_image_il();' not in src, 'menu image optimization must remain disabled during regression recovery'
a=src.index('static void patch_image_il(void)');b=src.index('// Patches the original IL',a)
h=r'''
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
typedef void MonoClass,MonoMethod;
typedef int so_module;
static int game_image,copies,freed,wrong_size,missing;
'''
for i,old in enumerate(original):
 h+=f'static const uint8_t original{i}[]={{'+','.join(str(x) for x in old)+'};\n'
 h+=f'static uint8_t code{i}[sizeof(original{i})];\n'
h+=r'''
static MonoClass *klass(int image,const char *ns,const char *name){return (void *)name;}
static MonoClass *nested(MonoClass *cls,void **iter){
 uintptr_t i=(uintptr_t)*iter;*iter=(void *)(i+1);
 if(i==0)return "decoy";
 if(i>1 || missing)return NULL;
 if(!strcmp(cls,"HudLoadingCharaInfo"))return "<LoadWWW>c__Iterator37";
 if(!strcmp(cls,"InformationImageManager"))return "ImageData";
 assert(!strcmp(cls,"ImageData"));return "<DoFetchImages>c__Iterator3B";
}
static const char *name(MonoClass *cls){return cls;}
static MonoMethod *method(MonoClass *cls,const char *s,int n){
 assert(!strcmp(s,"MoveNext") && n==0);
 return !strcmp(cls,"<LoadWWW>c__Iterator37") ? (void *)1 : (void *)2;
}
static struct {MonoClass *(*class_from_name)(int,const char *,const char *);
 MonoMethod *(*get_method)(MonoClass *,const char *,int);} api={klass,method};
static void *header(MonoMethod *m){return m;}
static const uint8_t *code(void *h,uint32_t *size,uint32_t *stack){
 *stack=8;*size=h==(void *)1?sizeof(code0):sizeof(code1);
 if(wrong_size)(*size)--;
 return h==(void *)1?code0:code1;
}
static void release(void *h){freed++;}
static so_module module;
static so_module *so_find_module(const char *s){return &module;}
static uintptr_t so_symbol(so_module *m,const char *s){
 if(!strcmp(s,"mono_class_get_nested_types"))return (uintptr_t)nested;
 if(!strcmp(s,"mono_class_get_name"))return (uintptr_t)name;
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
static void reset(void){memcpy(code0,original0,sizeof(code0));memcpy(code1,original1,sizeof(code1));copies=0;}
int main(int argc,char **argv){
 reset();patch_image_il();assert(copies==3 && freed==2);
 FILE *out=fopen(argv[1],"wb");assert(out);
 fwrite(code0,1,sizeof(code0),out);fwrite(code1,1,sizeof(code1),out);fclose(out);
 reset();code0[0x442]^=1;code1[0x7e]^=1;patch_image_il();assert(!copies && freed==4);
 reset();wrong_size=1;patch_image_il();assert(!copies && freed==6);
 wrong_size=0;missing=1;patch_image_il();assert(!copies && freed==6);
 puts("PASS: nested metadata resolution, atomic guards for both HUD edits, wrong sizes/tokens/missing classes skipped");
}
'''
def run(code, hud, valid):
    stack=[];calls=0;record={};cache={};image={'tex':None};www=object();disposed=False
    if hud:
        self={0x04003d4a:record,0x04003d4e:www,0x04003d51:{0x0400158a:cache},0x04003d4c:'url'}
        pc=0x3fd;end=0x45b
    else:
        self={0x04003d6a:image,0x04003d6b:www};pc=0x7c;end=0xa8
    while pc<end:
        op=code[pc];pc+=1
        if op==0:continue
        if op==2:stack.append(self);continue
        if op==0x14:stack.append(None);continue
        if op in (0x7b,0x7c):
            tok=int.from_bytes(code[pc:pc+4],'little');pc+=4
            stack.append(stack.pop()[tok]);continue
        if op==0x7d:
            tok=int.from_bytes(code[pc:pc+4],'little');pc+=4
            val=stack.pop();obj=stack.pop();obj[tok]=val;continue
        if op in (0x28,0x6f):
            tok=int.from_bytes(code[pc:pc+4],'little');pc+=4
            if tok==0x0a000072:
                assert stack.pop() is www;calls+=1;stack.append(object() if valid else None)
            elif tok==0x0a000029:
                b=stack.pop();a=stack.pop();stack.append(a is not b)
            elif tok==0x0a000a56:
                val=stack.pop();key=stack.pop();d=stack.pop();d[key]=val
            elif tok==0x0a000074:
                assert stack.pop() is www;disposed=True
            elif tok==0x0600172a:
                val=stack.pop();d=stack.pop();d['tex']=val
            elif tok==0x06001729:stack.append(stack.pop()['tex'])
            else:raise AssertionError(hex(tok))
            continue
        if op in (0x38,0x39,0x3a):
            displacement=int.from_bytes(code[pc:pc+4],'little',signed=True);pc+=4
            take=op==0x38 or ((not stack.pop()) if op==0x39 else bool(stack.pop()))
            if take:pc+=displacement
            continue
        raise AssertionError((hex(pc-1),hex(op)))
    assert not stack
    return calls,record,cache,image,disposed,pc
with tempfile.TemporaryDirectory() as d:
    p=Path(d);(p/'test.c').write_text(h)
    subprocess.run(['cc','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test'),str(p/'patched')],check=True)
    patched=(p/'patched').read_bytes();assert patched==b''.join(expected)
    for valid in (True,False):
        old=run(original[0],True,valid);new=run(expected[0],True,valid)
        assert old[0]==(3 if valid else 2) and new[0]==1
        assert new[1][0x04001592] is new[2]['url'] and old[4] and new[4]
        assert (new[2]['url'] is not None)==valid
        old=run(original[1],False,valid);new=run(expected[1],False,valid)
        assert old[0]==(2 if valid else 1) and new[0]==1
        assert (old[3]['tex'] is not None)==(new[3]['tex'] is not None)==valid
        assert old[5]==new[5]==(0xa8 if valid else 0x15e)
print('PASS: successful image creation drops 3→1 / 2→1; identity shared, null paths, branch targets, stack and Dispose preserved')

#!/usr/bin/env python3
"""Exercise the real guarded patch and the APK's flag/stocking control flow."""
import hashlib,json,subprocess,tempfile,zipfile
from pathlib import Path
root=Path(__file__).resolve().parents[1]
f=json.loads((root/'tests/fixtures/pickup_effects_il.json').read_text())
with zipfile.ZipFile(root.parent/'srr224.apk') as z:
 assert hashlib.sha256(z.read('assets/bin/Data/Managed/Assembly-CSharp.dll')).hexdigest()==f['assembly_sha256']
s=(root/'src/mono_probe.c').read_text();a=s.index('static void patch_pickup_effects_il(');b=s.index('// BetterList<T>.Trim',a)
h=r'''
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
typedef void MonoClass,MonoMethod;typedef int so_module;
static int game_image,copies,freed,wrong_size,missing;
'''
h+='static const uint8_t original[]={'+','.join(str(x) for x in bytes.fromhex(f['original']))+'};\n'
h+=r'''
static uint8_t codebuf[sizeof(original)];
static MonoClass *klass(int i,const char *ns,const char *name){assert(!strcmp(name,"StageEffectManager"));return missing?NULL:(void *)1;}
static MonoMethod *method(MonoClass *k,const char *name,int n){assert(!strcmp(name,"StockStageEffect") && n==1);return (void *)1;}
static struct{MonoClass *(*class_from_name)(int,const char *,const char *);MonoMethod *(*get_method)(MonoClass *,const char *,int);}api={klass,method};
static void *header(MonoMethod *m){return m;}
static const uint8_t *code(void *h,uint32_t *size,uint32_t *stack){*size=sizeof(codebuf)-wrong_size;*stack=8;return codebuf;}
static void release(void *h){freed++;}
static so_module module;
static so_module *so_find_module(const char *n){return &module;}
static uintptr_t so_symbol(so_module *m,const char *n){
 if(!strcmp(n,"mono_method_get_header"))return (uintptr_t)header;
 if(!strcmp(n,"mono_method_header_get_code"))return (uintptr_t)code;
 return (uintptr_t)release;
}
static void kuKernelCpuUnrestrictedMemcpy(void *d,const void *s,size_t n){memcpy(d,s,n);copies++;}
static void kuKernelFlushCaches(void *p,size_t n){}
#define L_INFO(...) ((void)0)
#define L_WARN(...) ((void)0)
'''
h+=s[a:b]
h+=r'''
// Execute actual IL until the stocking loop, including its ResourceManager
// presence check. m_lightMode is also the flag used by PlayEffect/SleepEffect.
static int run(int save,int light,int resource,int *flag){
 int stack[8],sp=0;unsigned pc=0;*flag=0;
 while(pc<0x36){unsigned op=codebuf[pc++];int tok;
 switch(op){
 case 0:break;
 case 2:stack[sp++]=999;break;
 case 0x14:case 0x16:stack[sp++]=0;break;
 case 0x26:assert(sp);sp--;break;
 case 0x28:case 0x6f:
  memcpy(&tok,codebuf+pc,4);pc+=4;
  if(tok==0x06003317)stack[sp++]=save;
  else if(tok==0x060032b9){assert(sp && stack[sp-1]==save);stack[sp-1]=light;}
  else if(tok==0x06003081)stack[sp++]=resource;
  else {assert(tok==0x0a000029 && sp>=2);sp--;stack[sp-1]=stack[sp-1]!=stack[sp];}
  break;
 case 0x7d:pc+=4;assert(sp==2 && stack[0]==999);*flag=stack[1];sp=0;break;
 case 0x7b:pc+=4;assert(sp && stack[sp-1]==999);stack[sp-1]=*flag;break;
 case 0x39:case 0x3a:{int offset;memcpy(&offset,codebuf+pc,4);pc+=4;assert(sp);int v=stack[--sp];if((op==0x39 && !v)||(op==0x3a && v))pc+=offset;break;}
 case 0x2a:assert(!sp);return 0;
 default:assert(0);
 }}assert(!sp && pc==0x36);return 1;
}
int main(void){
 memcpy(codebuf,original,sizeof(original));int flag;
 assert(!run(1,1,1,&flag) && flag==1);assert(run(1,0,1,&flag) && !flag);
 patch_pickup_effects_il();assert(copies==1 && freed==1);
 for(int i=0;i<sizeof(original);i++)if(i<0x10 || i>=0x15)assert(codebuf[i]==original[i]);
 assert(run(1,1,1,&flag) && !flag);assert(run(1,0,1,&flag) && !flag);
 assert(!run(1,1,0,&flag));assert(!run(0,1,0,&flag));assert(run(0,1,1,&flag));
 patch_pickup_effects_il();assert(copies==1); // idempotent
 memcpy(codebuf,original,sizeof(original));wrong_size=1;patch_pickup_effects_il();assert(copies==1);
 wrong_size=0;codebuf[0x11]^=1;patch_pickup_effects_il();assert(copies==1);
 missing=1;patch_pickup_effects_il();assert(copies==1);
 puts("PASS: APK identity, light-mode stocking/play/sleep flag, resource guard, unchanged loop, divergent IL/missing class");
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.c').write_text(h)
 subprocess.run(['cc','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)

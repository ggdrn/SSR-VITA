#!/usr/bin/env python3
"""Runtime IL changes: exact target guards, pickup playback and stack balance."""
from pathlib import Path
import subprocess,tempfile
src=(Path(__file__).resolve().parents[1]/'src/mono_probe.c').read_text()
a=src.index('static void patch_stage_il(void) {');b=src.index('// The stage manager disables',a)
harness=r'''
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
typedef void MonoClass,MonoMethod;
typedef struct { int x; } so_module;
static so_module mono;
static int game_image, copies, freed;
static uint8_t block[0xfb], light[20];
static const uint8_t tail[]={0x6f,0x89,0,0,0x0a,0x28,0x89,0x08,0,0x0a,0x2a};
static const uint8_t original_light[]={0x28,0xee,0x28,0,6,0x39,2,0,0,0,0x16,0x2a,2,3,0x28,0xc0,0x28,0,6,0x2a};
static MonoClass *klass(int image,const char *ns,const char *name){return (void *)name;}
static MonoMethod *method(MonoClass *cls,const char *name,int nargs){
 if(strcmp(name,"ActivateBlock")==0){assert(nargs==-1);return (void *)1;}
 if(strcmp(name,"LightPlaySE")!=0)return NULL;
 assert(nargs==2);return (void *)2;
}
static struct{MonoClass *(*class_from_name)(int,const char *,const char *);
 MonoMethod *(*get_method)(MonoClass *,const char *,int);} api={klass,method};
static void *header(MonoMethod *m){return m;}
static const uint8_t *code(void *h,uint32_t *size,uint32_t *stack){
 *size=h==(void *)1?sizeof(block):sizeof(light);*stack=3;
 return h==(void *)1?block:light;
}
static void release(void *h){freed++;}
static so_module *so_find_module(const char *n){return &mono;}
static uintptr_t so_symbol(so_module *m,const char *n){
 if(strcmp(n,"mono_method_get_header")==0)return (uintptr_t)header;
 if(strcmp(n,"mono_method_header_get_code")==0)return (uintptr_t)code;
 assert(strcmp(n,"mono_metadata_free_mh")==0);return (uintptr_t)release;
}
static void kuKernelFlushCaches(void *p,size_t n) {}
static void kuKernelCpuUnrestrictedMemcpy(void *d,const void *s,size_t n){memcpy(d,s,n);copies++;}
#define L_INFO(...) ((void)0)
#define L_WARN(...) ((void)0)
'''
harness+=src[a:b]
harness+=r'''
// Execute the original method's small instruction set to verify its behavior
// in light mode, including branch displacement and evaluation-stack balance.
static int run_light(int low,int *played){
 int st[8],sp=0;unsigned pc=0;*played=0;
 while(pc<sizeof(light)){
  unsigned op=light[pc++];
  if(op==0)continue;
  if(op==0x16){st[sp++]=0;continue;}
  if(op==2 || op==3){st[sp++]=op==2?111:222;continue;}
  if(op==0x28){
   uint32_t tok=light[pc]|light[pc+1]<<8|light[pc+2]<<16|light[pc+3]<<24;pc+=4;
   if(tok==0x060028ee)st[sp++]=low;
   else {assert(tok==0x060028c0 && sp==2 && st[0]==111 && st[1]==222);sp=0;st[sp++]=123;(*played)++;}
   continue;
  }
  if(op==0x39){uint32_t d=light[pc];pc+=4;assert(sp>0);if(!st[--sp])pc+=d;continue;}
  assert(op==0x2a && sp==1);return st[0];
 }
 assert(0);return -1;
}
int main(void){
 memcpy(block+0xf0,tail,sizeof(tail));memcpy(light,original_light,sizeof(light));
 int played;assert(run_light(1,&played)==0 && played==0);
 assert(run_light(0,&played)==123 && played==1);
 uint8_t before[sizeof(block)];memcpy(before,block,sizeof(block));
 patch_stage_il();assert(copies==2 && freed==2);
 for(unsigned i=0;i<sizeof(block);i++)assert(block[i]==(i>=0xf5&&i<0xfa?0:before[i]));
 assert(run_light(1,&played)==123 && played==1);
 assert(run_light(0,&played)==123 && played==1);
 assert(memcmp(light+5,original_light+5,15)==0);
 // Different assembly tokens must be rejected rather than patched blindly.
 memcpy(block+0xf0,tail,sizeof(tail));block[0xf6]^=1;
 memcpy(light,original_light,sizeof(light));light[1]^=1;
 copies=0;patch_stage_il();assert(copies==0 && freed==4);
 puts("PASS: per-block GC call only, light-mode pickup playback, valid IL stack, mismatched APK guards");
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.c').write_text(harness)
 subprocess.run(['cc','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)

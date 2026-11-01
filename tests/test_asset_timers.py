#!/usr/bin/env python3
"""Native asset timers must forward arguments/returns once and guard registration."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
def run(source,extra=(),args=((),)):
 with tempfile.TemporaryDirectory() as d:
  p=Path(d);(p/'test.c').write_text(source)
  subprocess.run(['cc','-I',str(root/'src'),'-pthread','-fsanitize=address,undefined',str(p/'test.c'),*extra,'-o',str(p/'test')],check=True)
  for a in args:subprocess.run([str(p/'test'),*a],check=True)


import zipfile
with zipfile.ZipFile(root.parent/'srr224.apk') as z:
 native=z.read('lib/armeabi-v7a/libunity.so')
 for name in ('UnityEngine.Object::Internal_CloneSingle','UnityEngine.Object::INTERNAL_CALL_Internal_InstantiateSingle','UnityEngine.Resources::Load','UnityEngine.WWW::GetTexture','UnityEngine.Resources::UnloadUnusedAssets','UnityEngine.AssetBundle::Load','UnityEngine.AssetBundle::LoadAsync','UnityEngine.Application::LoadLevelAsync'):
  assert (name+'\0').encode() in native
src=(root/'src/mono_probe.c').read_text();a=src.index('// Profile asset creation');b=src.index('// Distinguish the engine',a)
snippet=src[a:src.index('// Compile ahead',a)]+src[src.index('static MonoObject *traced_bundle_load',a):b]
h=r'''#include "util/work_trace.h"
#include "util/observe.h"
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
typedef void MonoObject,MonoMethod,MonoClass;
typedef int so_module;static so_module mono,unity,foreign;
static int unity_image=1,bad,adds,originals,ended,input_race_scene;
static void input_set_race_controls(int on){}
static void race_jit_warmup(void){}
static void *mapping[8];
static void *arg1=(void *)101,*arg2=(void *)102,*arg3=(void *)103;
static void observe_mono_label(MonoObject *o,char *s,unsigned n){snprintf(s,n,"asset");}
uint64_t observe_now(void){return 100;}
void observe_span(unsigned k,const char *s,uint64_t t,uint64_t bytes,int status,uintptr_t obj){assert(t==100);}
static uint64_t work_trace_begin(void){return 100;}
static void work_trace_end(unsigned kind,uint64_t start,uint64_t bytes,uintptr_t caller){assert(start==100 && bytes==0);assert(kind==WORK_CLONE || kind==WORK_RESOURCE || kind==WORK_IMAGE || kind==WORK_UNLOAD);ended++;}
static MonoObject *clone(MonoObject *a){assert(a==arg1);originals++;return (void *)201;}
static MonoObject *instantiate(MonoObject *a,void *b,void *c){assert(a==arg1&&b==arg2&&c==arg3);originals++;return (void *)202;}
static MonoObject *load(MonoObject *a,MonoObject *b){assert(a==arg1&&b==arg2);originals++;return NULL;}
static MonoObject *texture(MonoObject *self,uint8_t nonread){assert(self==arg1 && nonread==1);originals++;return (void *)203;}
static MonoObject *unload(void){originals++;return (void *)204;}
static MonoObject *bundle(MonoObject *a,MonoObject *b,MonoObject *c){assert(a==arg1 && b==arg2 && c==arg3);originals++;return (void *)205;}
static MonoObject *bundle_async(MonoObject *a,MonoObject *b,MonoObject *c){assert(a==arg1 && b==arg2 && c==arg3);originals++;return NULL;}
static MonoObject *scene(MonoObject *a,int i,uint8_t add,uint8_t sync){assert(a==arg1 && i==-1 && add==1 && sync==0);originals++;return (void *)206;}
static MonoClass *klass(int image,const char *ns,const char *name){return (void *)name;}
static MonoMethod *method(MonoClass *c,const char *name,int nargs){if(!strcmp(name,"Internal_CloneSingle")){assert(nargs==1);return (void *)1;}if(!strcmp(name,"INTERNAL_CALL_Internal_InstantiateSingle")){assert(nargs==3);return (void *)2;}if(!strcmp(name,"Load")){assert(nargs==2);return !strcmp((char *)c,"AssetBundle")?(void *)6:(void *)3;}if(!strcmp(name,"GetTexture")){assert(nargs==1);return (void *)4;}if(!strcmp(name,"UnloadUnusedAssets")){assert(nargs==0);return (void *)5;}if(!strcmp(name,"LoadAsync")){assert(nargs==2);return (void *)7;}assert(!strcmp(name,"LoadLevelAsync")&&nargs==4);return (void *)8;}
static struct{MonoClass *(*class_from_name)(int,const char *,const char *);MonoMethod *(*get_method)(MonoClass *,const char *,int);}api={klass,method};
static so_module *so_find_module(const char *name){return !strcmp(name,"libmono.so")?&mono:&unity;}
static so_module *so_module_at(uintptr_t p){return bad?&foreign:&unity;}
static void *lookup(MonoMethod *m){return mapping[(uintptr_t)m-1];}
static void add(const char *name,const void *fn){adds++;if(strstr(name,"AssetBundle::LoadAsync"))mapping[6]=(void *)fn;else if(strstr(name,"AssetBundle::Load"))mapping[5]=(void *)fn;else if(strstr(name,"Application::LoadLevelAsync"))mapping[7]=(void *)fn;else if(strstr(name,"Internal_CloneSingle"))mapping[0]=(void *)fn;else if(strstr(name,"Internal_InstantiateSingle"))mapping[1]=(void *)fn;else if(strstr(name,"::Load"))mapping[2]=(void *)fn;else if(strstr(name,"::GetTexture"))mapping[3]=(void *)fn;else mapping[4]=(void *)fn;}
static uintptr_t so_symbol(so_module *m,const char *name){return !strcmp(name,"mono_lookup_internal_call")?(uintptr_t)lookup:(uintptr_t)add;}
#define L_INFO(...) ((void)0)
'''+snippet+r'''
int main(int argc,char **argv){
 mapping[0]=clone;mapping[1]=instantiate;mapping[2]=load;mapping[3]=texture;mapping[4]=unload;mapping[5]=bundle;mapping[6]=bundle_async;mapping[7]=scene;bad=argc>1;
 work_hooks_init();work_hooks_init();
 if(bad){assert(adds==0 && !originals && !ended);return 0;}
 assert(adds==8 && !originals);
 assert(traced_clone(arg1)==(void *)201);
 assert(traced_instantiate(arg1,arg2,arg3)==(void *)202);
 assert(traced_resource_load(arg1,arg2)==NULL);
 assert(traced_www_texture(arg1,1)==(void *)203);
 assert(traced_unload()==(void *)204);
 assert(traced_bundle_load(arg1,arg2,arg3)==(void *)205);
 assert(traced_bundle_async(arg1,arg2,arg3)==NULL);
 assert(traced_scene_load(arg1,-1,1,0)==(void *)206);
 assert(originals==8 && ended==5);
 puts("PASS: all native arguments/results including NULL preserved, once-only forwarding/install, foreign functions untouched");
}
'''
h=h.replace('static uint64_t work_trace_begin','uint64_t work_trace_begin').replace('static void work_trace_end','void work_trace_end')
run(h,args=((),('foreign',)))

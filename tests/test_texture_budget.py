#!/usr/bin/env python3
"""Test actual native quality-call bridge without changing selected quality."""
from pathlib import Path
import subprocess,tempfile,zipfile
root=Path(__file__).resolve().parents[1]
with zipfile.ZipFile(root.parent/'srr224.apk') as apk:
    unity=apk.read('lib/armeabi-v7a/libunity.so')
    for method in ('SetQualityLevel','get_masterTextureLimit','set_masterTextureLimit'):
        assert ('UnityEngine.QualitySettings::'+method+'\0').encode() in unity
src=(root/'src/mono_probe.c').read_text();a=src.index('// Texture budget:');b=src.index('// Each WWW.texture',a)
h=r'''
#include <stdint.h>
static void bundle_mips_set_limit(int level) {}
#include <stdio.h>
#include <string.h>
#include <assert.h>
typedef void MonoMethod,MonoClass,MonoImage;
typedef int so_module;
static so_module mono,unity_module,other;
static MonoImage *unity_image;
static int limit,selected=-1,expensive_arg=-1,sets,quality_calls,adds,registered=1,foreign,missing_api;
static int get_limit(void){return limit;}
static void set_limit(int value){limit=value;sets++;}
static void set_quality(int value,int expensive){selected=value;expensive_arg=expensive;quality_calls++;limit=0;}
static void (*mapping)(int,int)=set_quality;
static void *lookup(MonoMethod *m){
 if(!registered)return NULL;
 if(m==(void *)1)return get_limit;
 if(m==(void *)2)return set_limit;
 assert(m==(void *)3);return mapping;
}
static void add(const char *name,const void *fn){assert(!strcmp(name,"UnityEngine.QualitySettings::SetQualityLevel"));mapping=(void *)fn;adds++;}
static so_module *so_find_module(const char *name){return !strcmp(name,"libmono.so")?&mono:&unity_module;}
static so_module *so_module_at(uintptr_t p){return foreign?&other:&unity_module;}
static uintptr_t so_symbol(so_module *m,const char *name){
 if(missing_api)return 0;
 return !strcmp(name,"mono_lookup_internal_call")?(uintptr_t)lookup:(uintptr_t)add;
}
static MonoClass *klass(MonoImage *img,const char *ns,const char *name){assert(!strcmp(name,"QualitySettings"));return (void *)1;}
static MonoMethod *method(MonoClass *cls,const char *name,int n){
 if(!strcmp(name,"get_masterTextureLimit")){assert(n==0);return (void *)1;}
 if(!strcmp(name,"set_masterTextureLimit")){assert(n==1);return (void *)2;}
 assert(!strcmp(name,"SetQualityLevel") && n==2);return (void *)3;
}
static struct {MonoClass *(*class_from_name)(MonoImage *,const char *,const char *);
 MonoMethod *(*get_method)(MonoClass *,const char *,int);} api={klass,method};
#define L_INFO(...) ((void)0)
#define L_WARN(...) ((void)0)
'''
h+=src[a:b]
h+=r'''
static void reset(void){texture_budget_ready=0;texture_limit_native_get=NULL;texture_limit_native_set=NULL;quality_level_original=NULL;mapping=set_quality;limit=sets=adds=quality_calls=0;}
int main(void){
 mono_texture_budget_tick();assert(!adds && !sets);
 unity_image=(void *)1;registered=0;mono_texture_budget_tick();assert(!adds && !sets && !texture_budget_ready);
 registered=1;mono_texture_budget_tick();assert(adds==1 && sets==1 && limit==1 && selected==-1);
 for(int i=0;i<100;i++)mono_texture_budget_tick();assert(sets==1 && adds==1);
 limit=2;mono_texture_budget_tick();assert(limit==2 && sets==1);
 mapping(0,1);assert(selected==0 && expensive_arg==1 && quality_calls==1 && limit==1 && sets==2);
 mapping(1,0);assert(selected==1 && expensive_arg==0 && quality_calls==2 && limit==1 && sets==3);
 limit=0;mono_texture_budget_tick();assert(limit==1 && sets==4);
 reset();foreign=1;mono_texture_budget_tick();assert(!adds && !sets && texture_budget_ready==-1);
 reset();foreign=0;missing_api=1;mono_texture_budget_tick();assert(!adds && !sets && texture_budget_ready==-1);
 puts("PASS: startup/retry, native icall validation, quality/expensive arguments preserved, no recursion, stronger cap kept, periodic repair");
}
'''
with tempfile.TemporaryDirectory() as d:
    p=Path(d);(p/'test.c').write_text(h)
    subprocess.run(['cc','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)

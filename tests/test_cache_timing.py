#!/usr/bin/env python3
"""Cache lookup timing forwards unchanged, only to the validated engine icall."""
from pathlib import Path
import subprocess,tempfile,zipfile
root=Path(__file__).resolve().parents[1]
with zipfile.ZipFile(root.parent/'srr224.apk') as z:
 assert b'UnityEngine.Caching::IsVersionCached\0' in z.read('lib/armeabi-v7a/libunity.so')
src=(root/'src/mono_probe.c').read_text();a=src.index('static uint8_t (*cache_version_native)');b=src.index('void mono_menu_fix',a)
h=r'''
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
typedef void MonoObject,MonoMethod,MonoClass;
typedef struct {int n;}so_module;
static so_module mono,unity,foreign;
static int mode,installed,calls,timed;
static int unity_image=1;
static uint8_t native(MonoObject *url,int version){assert(url==(void *)42&&version==123);calls++;return calls%2;}
static void *mapping=(void *)native;
static so_module *so_find_module(const char *s){return !strcmp(s,"libmono.so")?&mono:&unity;}
static so_module *so_module_at(uintptr_t p){return mode==1?&foreign:&unity;}
static void *lookup(MonoMethod *m){return mapping;}
static void add(const char *s,const void *fn){assert(!strcmp(s,"UnityEngine.Caching::IsVersionCached"));mapping=(void *)fn;installed++;}
static void *so_symbol(so_module *m,const char *s){if(mode==2)return NULL;return !strcmp(s,"mono_lookup_internal_call")?(void *)lookup:(void *)add;}
static MonoClass *klass(int i,const char *ns,const char *n){assert(i==1&&!strcmp(ns,"UnityEngine")&&!strcmp(n,"Caching"));return (void *)1;}
static MonoMethod *method(MonoClass *c,const char *n,int args){assert(!strcmp(n,"IsVersionCached")&&args==2);return (void *)2;}
static struct {MonoClass *(*class_from_name)(int,const char *,const char *);MonoMethod *(*get_method)(MonoClass *,const char *,int);}api={klass,method};
#define WORK_CACHE 8
static uint64_t work_trace_begin(void){return 100;}
static void work_trace_end(int k,uint64_t start,int b,uintptr_t caller){assert(k==WORK_CACHE&&start==100);timed++;}
#define L_INFO(...) ((void)0)
'''+src[a:b]+r'''
int main(int argc,char **argv){
 mode=argc>1?argv[1][0]-'0':0;cache_timing_init();cache_timing_init();
 if(mode){assert(!installed&&!calls&&!timed);}
 else{assert(installed==1);assert(traced_cache_version((void *)42,123)==1);assert(traced_cache_version((void *)42,123)==0);assert(calls==2&&timed==2);}
 puts("PASS: cache timer preserves boolean/version/URL, once-only registration, foreign/missing API guarded");
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.c').write_text(h)
 subprocess.run(['cc','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
 for mode in range(3):subprocess.run([str(p/'test'),str(mode)],check=True)

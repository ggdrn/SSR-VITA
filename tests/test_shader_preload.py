#!/usr/bin/env python3
"""Prepared GXM ownership transfers once, preserving live GL shader identity."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root.parent/'vitaGL/source/custom_shaders.c').read_text()
a=s.index('#define PRELOADED_SHADER_BUDGET');b=s.index('static GLboolean load_cached_shader',a)
c=s.index('const char *vglGetShaderCachePath');d=s.index('\n#endif',c)
h=r'''
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#define GL_TRUE 1
#define GL_FALSE 0
#define HAVE_GLSL_TEXTURE_SIZE 1
typedef int GLboolean;
typedef struct matrix_uniform {struct matrix_uniform *chain;} matrix_uniform;
typedef struct {int type,slot,dirty,is_glsl,ref_counter;void *prog;unsigned id,size,unif_buf_size;int semantics,sized_samplers_num;int sized_samplers[16];matrix_uniform *mat;void *unif_blk;char *source,*log;} shader;
static int fail_alloc,loads,released,invalid;
static char vgl_shader_cache_path[]="cache/v1";
static void *vglCalloc(size_t n,size_t z){return fail_alloc?NULL:calloc(n,z);}
#define vgl_free free
static void release_shader(shader *s){free(s->prog);free(s->mat);free(s->source);released++;}
'''+s[a:b]+r'''
static GLboolean load_cached_shader(const char *p,shader *s,GLboolean bindings){
 if(take_preloaded_shader(p,s,bindings))return 1;
 loads++;if(invalid)return 0;
 s->prog=malloc(32);s->mat=calloc(1,sizeof(matrix_uniform));assert(s->prog && s->mat);
 s->id=101;s->size=32;s->unif_buf_size=64;s->semantics=77;s->sized_samplers_num=1;s->sized_samplers[0]=9;
 return 1;
}
'''+s[c:d]+r'''
int main(void){
 assert(!strcmp(vglGetShaderCachePath(),"cache/v1"));
 assert(vglPreloadShaderCacheFile("v/AA/source.gxp",128)==1 && loads==1);
 assert(vglPreloadShaderCacheFile("v/AA/source.gxp",128)==1 && loads==1);
 size_t before=preloaded_shader_bytes;assert(before>128);
 shader live={.type=123,.slot=8,.dirty=1,.is_glsl=1,.ref_counter=2,.source=strdup("original GLSL"),.log=strdup("compiler log")};
 char *source=live.source,*log=live.log;
 assert(!take_preloaded_shader("v/AA/source.gxp",&live,0));
 assert(!take_preloaded_shader("wrong.gxp",&live,1));
 assert(load_cached_shader("v/AA/source.gxp",&live,1));assert(loads==1 && !preloaded_shader_bytes && !preloaded_shaders);
 assert(live.type==123 && live.slot==8 && live.dirty==1 && live.is_glsl==1 && live.ref_counter==2);
 assert(live.source==source && live.log==log && !strcmp(source,"original GLSL"));
 assert(live.id==101 && live.prog && live.mat && live.unif_buf_size==64 && live.semantics==77 && live.sized_samplers[0]==9);
 assert(!take_preloaded_shader("v/AA/source.gxp",&live,1));release_shader(&live);free(log);
 invalid=1;assert(!vglPreloadShaderCacheFile("bad.gxp",128) && !preloaded_shader_bytes);invalid=0;
 fail_alloc=1;assert(vglPreloadShaderCacheFile("oom.gxp",128)==-1);fail_alloc=0;
 assert(vglPreloadShaderCacheFile("large.gxp",1024*1024+1)==-1);
 int n=0;for(;;){char name[40];snprintf(name,sizeof(name),"shader-%d.gxp",n);int r=vglPreloadShaderCacheFile(name,1024*1024);if(r<0)break;assert(r==1);n++;}
 assert(n>0 && n<8 && preloaded_shader_bytes<=PRELOADED_SHADER_BUDGET);
 int old=released;vglReleasePreloadedShaderCache();assert(released==old+n && !preloaded_shaders && !preloaded_shader_bytes);
 vglReleasePreloadedShaderCache();assert(released==old+n);
 puts("PASS: registered program handoff, source/slot/refcount preservation, no duplicate disk load, bounded RAM, malformed/allocation failures and cleanup");
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.c').write_text(h)
 subprocess.run(['cc','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)

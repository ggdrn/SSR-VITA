#!/usr/bin/env python3
"""Actual cache loader/writer: hits, fallback, truncated reads and publication."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
def run(source,extra=(),args=((),)):
 with tempfile.TemporaryDirectory() as d:
  p=Path(d);(p/'test.c').write_text(source)
  subprocess.run(['cc','-I',str(root/'src'),'-pthread','-fsanitize=address,undefined',str(p/'test.c'),*extra,'-o',str(p/'test')],check=True)
  for a in args:subprocess.run([str(p/'test'),*a],check=True)


import re
src=(root.parent/'vitaGL/source/custom_shaders.c').read_text()
a=src.index('void vgl_shader_cache_event(unsigned event)');b=src.index('#ifdef HAVE_SHADER_CACHE\n#define vgl_compile_shader',a)
# Keep GLSL metadata guard intact; remove only the final enclosing #endif.
loader=src[a:b];idx=loader.rfind('#endif');loader=loader[:idx]+loader[idx+6:]
a=src.index('\tcache_fname[fname_len - 21] = 0;');b=src.index('\n#endif\n}',a)
writer=src[a:b]
h=r'''#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define HAVE_GLSL_TEXTURE_SIZE 1
#define SCE_GXM_MAX_TEXTURE_UNITS 16
#define SCE_O_RDONLY 1
#define SCE_O_CREAT 2
#define SCE_O_WRONLY 4
#define SCE_O_TRUNC 8
#define SCE_SEEK_END 2
#define SCE_SEEK_SET 0
#define GL_TRUE 1
#define GL_FALSE 0
typedef int SceUID,GLboolean;typedef long long SceOff;
typedef struct{char data[16];} binds_map;
typedef struct{char data[8];} glsl_samplers_info;
typedef struct{void *prog;} shader;
static unsigned char input[1024],disk[1024];static size_t input_size=512;
static unsigned counts[5];static int missing,short_read,alloc_fail,short_write,close_fail,rename_fail,deserialized,removed,committed,temp_removed;
static void event_impl(unsigned e){assert(e<5);counts[e]++;}
#define vgl_shader_cache_event event_impl
static int sceIoOpen(const char *p,int f,int mode){return f==SCE_O_RDONLY?(missing?-1:1):2;}
static SceOff sceIoLseek(int fd,SceOff off,int mode){return mode==SCE_SEEK_END?input_size:0;}
static int sceIoRead(int fd,void *buf,size_t n){if(short_read)return 0;memcpy(buf,input,n);return n;}
static void *vglMalloc(size_t n){return alloc_fail?NULL:malloc(n);}
static void vgl_free(void *p){free(p);}
static int sceIoClose(int fd){return close_fail?-1:0;}
static int sceIoRemove(const char *p){if(strstr(p,".tmp"))temp_removed++;else removed++;return 0;}
static int sceIoMkdir(const char *p,int mode){return 0;}
static int sceIoWrite(int fd,const void *buf,size_t n){if(short_write)return n-1;memcpy(disk,buf,n);return n;}
static int sceIoRename(const char *a,const char *b){assert(strstr(a,".tmp")&&!strstr(b,".tmp"));if(rename_fail)return -1;committed++;return 0;}
static void unserialize_shader(void *buf,size_t n,shader *s,GLboolean binds){deserialized++;s->prog=(void *)1;}
static void *serialize_shader(void *out,size_t *size,shader *s,GLboolean binds){*size=16;return alloc_fail?NULL:calloc(1,*size);}
'''+loader+'\nstatic void save(shader *s, GLboolean save_bindings, char *cache_fname, int fname_len){\n'+writer+'\n}\n'+r'''
static void valid(int bindings){
 memset(input,0,sizeof(input));input_size=512;unsigned offset=4+(bindings?sizeof(binds_map):0)+1;
 memcpy(input+offset,"GXP",4);uint32_t size=input_size-offset;memcpy(input+offset+8,&size,4);
}
int main(void){
 shader s={0};valid(0);missing=1;assert(!load_cached_shader("cache",&s,0)&&counts[1]==1&&!deserialized);missing=0;
 assert(load_cached_shader("cache",&s,0)&&counts[0]==1&&deserialized==1);
 valid(1);assert(load_cached_shader("cache",&s,1));int before=deserialized;
 short_read=1;assert(!load_cached_shader("cache",&s,1));short_read=0;assert(removed==0);
 alloc_fail=1;assert(!load_cached_shader("cache",&s,1));alloc_fail=0;assert(removed==0);
 input_size=1;assert(!load_cached_shader("cache",&s,1));input_size=2*1024*1024;assert(!load_cached_shader("cache",&s,1));
 valid(1);*(uint32_t *)input=UINT32_MAX;assert(!load_cached_shader("cache",&s,1));
 valid(1);input[20]=17;assert(!load_cached_shader("cache",&s,1));
 valid(1);input[21]='X';assert(!load_cached_shader("cache",&s,1));
 valid(1);input_size=400;assert(!load_cached_shader("cache",&s,1)); // GXP size exceeds payload
 assert(deserialized==before && removed==6);
 char path[128]="root/f/AA/0123456789ABCDEF.gxp";
 save(&s,1,path,strlen(path));assert(committed==1&&counts[2]==1);
 short_write=1;save(&s,1,path,strlen(path));short_write=0;
 close_fail=1;save(&s,1,path,strlen(path));close_fail=0;
 rename_fail=1;save(&s,1,path,strlen(path));rename_fail=0;
 alloc_fail=1;save(&s,1,path,strlen(path));alloc_fail=0;
 assert(committed==1&&temp_removed==4);
 puts("PASS: cache reuse/miss counters, bounded reads/metadata/GXP, failure fallback, atomic publication and failed-write cleanup");
}
'''
# A weak application hook is optional in production; our test provides it.
h=h.replace('void vgl_shader_cache_event(unsigned event) __attribute__((weak));', '')
h=h.replace('if (vgl_shader_cache_event) vgl_shader_cache_event(event);','vgl_shader_cache_event(event);')
run(h)

#!/usr/bin/env python3
"""Cache allocation/metadata/registration failures preserve the GLSL fallback."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root.parent/'vitaGL/source/custom_shaders.c').read_text()
a=s.index('void unserialize_shader(');b=s.index('\n#ifdef HAVE_SHADER_CACHE',a)
h=r'''
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
typedef int GLboolean;
typedef uint32_t SceGxmProgram,SceGxmProgramParameter;
typedef struct matrix_uniform {void *ptr;struct matrix_uniform *chain;} matrix_uniform;
typedef struct {void *prog;size_t size;unsigned id,unif_buf_size;int semantics;matrix_uniform *mat;char *source;} shader;
typedef int binds_map;
static int fail_after=-1,registered,unregistered,bad_program,bad_registration,gxm_shader_patcher;
static void *vglMalloc(size_t n){if(fail_after==0)return NULL;if(fail_after>0)fail_after--;return malloc(n);}
#define vgl_fast_memcpy memcpy
#define vgl_free free
static int sceGxmProgramCheck(void *p){return bad_program?-1:0;}
static unsigned sceGxmProgramGetParameterCount(void *p){return 4;}
static int sceGxmShaderPatcherRegisterProgram(int patcher,void *p,unsigned *id){if(bad_registration)return -1;registered++;*id=99;return 0;}
static unsigned sceGxmProgramGetDefaultUniformBufferSize(void *p){return 16;}
static uint32_t *vglProgramGetParameterBase(void *p){return p;}
static void vglVertexProgramCacheInvalidate(unsigned id){assert(id==99);unregistered++;}
'''+s[a:b]+r'''
int main(void){
 uint32_t data[20]={1,2};char source[]="original GLSL";
 for(int failure=0;failure<5;failure++) {
  shader sh={.source=source};fail_after=-1;bad_program=bad_registration=0;data[1]=2;
  if(failure==0)fail_after=0; // program allocation
  if(failure==1)fail_after=1; // matrix metadata allocation
  if(failure==2)bad_program=1;
  if(failure==3)bad_registration=1;
  if(failure==4)data[1]=4; // invalid parameter index
  unserialize_shader(data,sizeof(data),&sh,0);
  assert(!sh.prog && !sh.mat && sh.source==source && !strcmp(source,"original GLSL"));
 }
 fail_after=-1;bad_program=bad_registration=0;data[1]=2;shader sh={.source=source};
 unserialize_shader(data,sizeof(data),&sh,0);assert(sh.prog && sh.mat && sh.mat->ptr==(uint32_t *)sh.prog+8 && sh.source==source);
 free(sh.mat);free(sh.prog);assert(registered==2 && unregistered==1);
 puts("PASS: program/metadata OOM, invalid GXP, invalid matrix index, registration failure and intact GLSL fallback");
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.c').write_text(h)
 subprocess.run(['cc','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)

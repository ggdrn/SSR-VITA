#!/usr/bin/env python3
"""Exercise vitaGL's real attribute reflection with interleaved GXM metadata."""
from pathlib import Path
import subprocess,tempfile,sys
root=Path(__file__).resolve().parents[1]
s=(root.parent/'vitaGL/source/custom_shaders.c').read_text()
a=s.index('void glGetActiveAttrib(');b=s.index('\nvoid glGetActiveUniform',a)
function=s[a:b]
if len(sys.argv)>1: function=Path(sys.argv[1]).read_text()
h=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef unsigned GLuint,GLenum;
typedef char GLchar;
typedef int GLint,GLsizei;
#define GL_INVALID_VALUE 0x501
#define GL_FLOAT_VEC2 2
#define GL_FLOAT_VEC3 3
#define GL_FLOAT_VEC4 4
#define SCE_GXM_PARAMETER_CATEGORY_ATTRIBUTE 1
#define THREAD_SAFE()
#define SET_GL_ERROR(e) error=e; return;
#define min(a,b) ((a)<(b)?(a):(b))
static int error;
// Actual GXM table stride is 16 bytes, with uniforms mixed into the table.
typedef struct {uint32_t category,reg,name,components;} SceGxmProgramParameter;
static SceGxmProgramParameter params[]={{1,0,0,4},{2,16,1,4},{1,1,2,3},{1,2,3,2},{2,20,4,4}};
static const char *names[]={"_glesVertex","glstate_matrix_mvp","_glesNormal","_glesMultiTexCoord0","_DifTex_ST"};
typedef struct {void *prog;} shader;
typedef struct {shader *vshader;unsigned attr_num;} program;
static shader vs;
static program progs[]={{&vs,3}};
static unsigned sceGxmProgramGetParameterCount(void *p){return sizeof(params)/sizeof(*params);}
static uint32_t *vglProgramGetParameterBase(void *p){return (uint32_t *)params;}
static unsigned sceGxmProgramParameterGetCategory(const SceGxmProgramParameter *p){return p->category;}
static unsigned sceGxmProgramParameterGetResourceIndex(const SceGxmProgramParameter *p){return p->reg;}
static const char *sceGxmProgramParameterGetName(const SceGxmProgramParameter *p){return names[p->name];}
static unsigned sceGxmProgramParameterGetComponentCount(const SceGxmProgramParameter *p){return p->components;}
static unsigned sceGxmProgramParameterGetArraySize(const SceGxmProgramParameter *p){return 1;}
static GLenum gxm_attr_type_to_gl(unsigned n,unsigned size){return n*size;}
'''+function+r'''
int main(void){
 char name[40];int length,size;unsigned type;
 for(int sparse=0;sparse<2;sparse++) {
  if(sparse){params[0].reg=8;params[2].reg=20;params[3].reg=48;}
  const int wanted[]={0,2,3};
  for(unsigned i=0;i<3;i++) {
   memset(name,0xcc,sizeof(name));error=0;
   glGetActiveAttrib(1,i,sizeof(name),&length,&size,&type,name);
   assert(!error && !strcmp(name,names[wanted[i]]) && length==strlen(name));
   assert(size==1 && type==params[wanted[i]].components);
  }
 }
 char guard[3]={'A','B','C'};
 glGetActiveAttrib(1,0,0,&length,&size,&type,guard+1);assert(length==0 && guard[0]=='A' && guard[1]=='B');
 glGetActiveAttrib(1,0,0,NULL,&size,&type,NULL);assert(!error);
 glGetActiveAttrib(1,0,1,&length,&size,&type,guard+1);assert(length==0 && guard[1]==0 && guard[2]=='C');
 glGetActiveAttrib(1,1,5,NULL,&size,&type,name);assert(!strcmp(name,"_gle"));
 error=0;glGetActiveAttrib(1,3,40,&length,&size,&type,name);assert(error==GL_INVALID_VALUE);
 error=0;glGetActiveAttrib(1,0,-1,&length,&size,&type,name);assert(error==GL_INVALID_VALUE);
 puts("PASS: position/normal/UV reflection, interleaved uniforms, sparse registers, zero/truncated buffers, invalid indices");
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.c').write_text(h)
 subprocess.run(['cc','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)

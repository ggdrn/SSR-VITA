#!/usr/bin/env python3
"""Run vitaGL's packed-stream predicate against skinned and static meshes."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root.parent/'vitaGL/source/custom_shaders.c').read_text()
a=s.index('static GLboolean vertex_streams_are_packed(');b=s.index('\n}\n',a)+3
assert s.count('!vertex_streams_are_packed(p)')==3, 'all three draw paths must validate every active attribute'
h=r'''
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef int GLboolean;
#define vgl_log(...) ((void)0)
#define GL_TRUE 1
#define GL_FALSE 0
typedef struct {unsigned attr_num;uint8_t attr_map[16];} program;
typedef struct {uint32_t stride;} stream;
typedef struct {uint32_t vertex_attrib_state,vertex_attrib_offsets[16],vertex_attrib_vbo[16];stream vertex_stream_config[16];} vao;
static vao state,*cur_vao=&state;
'''+s[a:b]+r'''
static program p={3,{0,2,3}};
static void mesh(unsigned position,unsigned normal,unsigned uv,unsigned stride,unsigned uv_stride,unsigned buffer) {
 memset(&state,0,sizeof(state));state.vertex_attrib_state=(1<<0)|(1<<2)|(1<<3);
 state.vertex_attrib_offsets[0]=position;state.vertex_attrib_offsets[2]=normal;state.vertex_attrib_offsets[3]=uv;
 state.vertex_stream_config[0].stride=state.vertex_stream_config[2].stride=stride;
 state.vertex_stream_config[3].stride=uv_stride;
 state.vertex_attrib_vbo[0]=state.vertex_attrib_vbo[2]=state.vertex_attrib_vbo[3]=buffer;
}
int main(void){
 // Realistic CPU-skinned layout: position+normal together, UV elsewhere.
 // The old two-attribute check accepts it even though UV is outside the copy.
 mesh(0x1000,0x100c,0x8000,24,8,0);
 assert(state.vertex_attrib_offsets[2]>state.vertex_attrib_offsets[0] && state.vertex_attrib_offsets[2]<0x1000+24);
 assert(!vertex_streams_are_packed(&p));
 mesh(0x1000,0x100c,0x8000,24,24,0);assert(!vertex_streams_are_packed(&p)); // same stride, separate array
 mesh(0,12,65536,24,8,77);assert(!vertex_streams_are_packed(&p)); // one VBO, separate ranges
 mesh(0,12,24,32,32,77);assert(vertex_streams_are_packed(&p)); // true interleaved VBO
 mesh(0x1000,0x100c,0x1018,32,32,0);assert(vertex_streams_are_packed(&p)); // true client interleaved
 state.vertex_attrib_vbo[3]=88;assert(!vertex_streams_are_packed(&p));
 state.vertex_attrib_vbo[3]=0;state.vertex_stream_config[3].stride=8;assert(!vertex_streams_are_packed(&p));
 state.vertex_attrib_state &= ~(1<<3);assert(vertex_streams_are_packed(&p)); // disabled UV uses constant value
 state.vertex_attrib_state &= ~(1<<0);assert(!vertex_streams_are_packed(&p));
 mesh(0xfffffff0u,0xfffffffcu,8,32,32,0);assert(!vertex_streams_are_packed(&p)); // no address wrap
 mesh(0x1000,0x100c,0x0ff0,32,32,0);assert(!vertex_streams_are_packed(&p));
 mesh(0,12,32,32,32,0);assert(!vertex_streams_are_packed(&p)); // next vertex boundary
 p.attr_num=1;assert(!vertex_streams_are_packed(&p));
 p.attr_num=2;p.attr_map[1]=3;mesh(0x1000,0x100c,0x8000,24,8,0);assert(!vertex_streams_are_packed(&p)); // unlit position+UV
 puts("PASS: skinned position/normal + separate UV, client/VBO layouts, strides, disabled channels, sparse bindings and address bounds");
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.c').write_text(h)
 subprocess.run(['cc','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)

#!/usr/bin/env python3
"""Exercise actual vitaGL texture replacement code with failed allocations."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
src = (root / 'vitaGL/source/textures.c').read_text()
a = src.index('\t\tvoid *texture_data = gpu_alloc_mapped_for_gpu(size);', src.index('void _glTexSubImage2D'))
b = src.index('#ifdef HAVE_TEX_CACHE', a)
sub = src[a:b]
src = (root / 'vitaGL/source/utils/gpu_utils.c').read_text()
a = src.index('\t\tvoid *texture_data = tex->data;', src.index('void gpu_alloc_mipmaps'))
b = src.index('// Performing a chain downscale', a)
mip = src[a:b]
a = src.index('\tvoid *texture_data = NULL;', src.index('void gpu_alloc_texture'))
b = src.index('\n\tif (texture_data) {', a)
reserve = src[a:b]
harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef struct { void *data; int last_frame, gxm_tex, use_mips; } texture;
static char original[64], replacement[128];
static int fail, finishes, frees, copies, error, descriptor_updates, attempts;
static uint32_t vgl_framecount;
#define OBJ_NOT_USED -1
#define MEM_ALIGNMENT 16
#define MAX(a,b) ((a)>(b)?(a):(b))
static unsigned last_capacity, reserve_calls;
static int reserve_ok;
static unsigned nearest_po2(unsigned n) {unsigned p=1;while(p<n)p*=2;return p;}
static void *gpu_alloc_mapped_aligned_for_gpu_inner(unsigned align,unsigned size) {reserve_calls++;last_capacity=size;return reserve_ok?replacement:NULL;}
#define GL_OUT_OF_MEMORY 0x505
#define VGL_ALIGN(w,a) (((w)+(a)-1)&~((a)-1))
#define vgl_log(...) ((void)0)
#define SET_GL_ERROR(e) error=e; return;
static void *gpu_alloc_mapped_for_gpu(unsigned size) { attempts++; return fail ? NULL : replacement; }
static unsigned reserved;
static unsigned vgl_malloc_usable_size(void *ptr) { return reserved; }
static void *vgl_realloc(void *ptr, unsigned size) { return NULL; }
static void (*vgl_texture_allocation_failure)(const char *, uint32_t);
static void glFinish(void) { finishes++; }
static void copy(void *dst, const void *src, unsigned size) {
 assert(dst && src); assert(size<=sizeof(original)); copies++; memcpy(dst,src,size);
}
#define vgl_fast_memcpy copy
#define vgl_memcpy copy
static void gpu_free_texture_data(texture *tex) { assert(tex->data==original); frees++; }
static void sceGxmTextureSetData(int *desc, void *ptr) { assert(ptr==replacement); descriptor_updates++; }
static void update(texture *tex, unsigned size) {
'''
harness += sub + '\n}\nstatic void mipmap(texture *tex) {\nint count=0; unsigned size=128, orig_w=8, orig_h=4, bpp=2;\n'
harness += mip + '\nassert(texture_data==(reserved>=size?tex->data:replacement));\n}\n'
harness += 'static void *base_reserve(texture *tex,unsigned w,unsigned h,unsigned bpp) {unsigned tex_size=VGL_ALIGN(w,8)*h*bpp;\n'+reserve+'\nreturn texture_data;\n}\n'
harness += r'''
static void reset(void) { finishes=frees=copies=error=descriptor_updates=0; }
int main(void) {
 memset(original,0x5a,sizeof(original)); texture tex={original,42,0};
 fail=1; update(&tex,64);
 assert(tex.data==original && tex.last_frame==OBJ_NOT_USED);
 assert(finishes==1 && frees==0 && copies==0 && descriptor_updates==0);
 assert(original[0]==0x5a);
 reset(); fail=0; tex.last_frame=42; update(&tex,64);
 assert(tex.data==replacement && tex.last_frame==OBJ_NOT_USED);
 assert(!finishes && frees==1 && copies==1 && descriptor_updates==1);
 assert(!memcmp(original,replacement,64));
 reset(); fail=1; tex.data=original; mipmap(&tex);
 assert(error==GL_OUT_OF_MEMORY && tex.data==original && !frees && !copies);
 reset(); fail=0; mipmap(&tex);
 assert(!error && frees==1 && copies==1 && !memcmp(original,replacement,64));
 reset(); tex.data=original; reserved=128; int before=attempts;
 mipmap(&tex); assert(!error && !frees && !copies && attempts==before);
 reset(); reserved=0; tex.use_mips=1; reserve_ok=1; before=attempts;
 assert(base_reserve(&tex,2048,2048,2)==replacement && last_capacity==11184880 && attempts==before);
 reserve_ok=0; before=attempts;
 assert(base_reserve(&tex,2048,2048,2)==replacement && attempts==before+1);
 tex.use_mips=0; before=reserve_calls;
 assert(base_reserve(&tex,2048,2048,2)==replacement && reserve_calls==before);
 puts("PASS: texture OOM preserves live data, waits before in-place update, guards mip copy; success still copies");
}
'''
with tempfile.TemporaryDirectory() as d:
    p=Path(d)
    (p/'test.c').write_text(harness)
    subprocess.run(['cc','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)

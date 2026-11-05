#!/usr/bin/env python3
"""Exercise production native hook installation, array guards and rollback."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
src='\n'.join(l for l in (root/'src/util/rijndael256.c').read_text().splitlines() if not l.startswith('#include'))
harness=r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <sys/types.h>
#include "util/rijndael256_block.h"
#include "util/rijndael256_il.h"
#define L_INFO(...) ((void)0)
#define L_WARN(...) ((void)0)
#define DATA_PATH "/not-a-real-directory"
typedef void so_module;
static int mode,freed,hooks,inits,forwarded;
struct array { void *type; uint32_t bounds,length; uint8_t data[1024]; } arrays[5],input,output,keys;
static int crypto_type,byte_type,word_type,other_type;
static void *self=&crypto_type;
static uint32_t native_code[2],trampoline[1024];
static uint8_t method_il[sizeof(rijndael256_il)];
static FILE *marker_open(const char *p,const char *m) { return mode==1 ? tmpfile() : NULL; }
#define fopen marker_open
static void *get_corlib(void){return (void *)1;}
static const char *get_guid(void *im){return mode==2?"other":RIJNDAEL_MVID;}
static void *get_class(void *im,const char *ns,const char *n) {
 assert(!strcmp(ns,"System.Security.Cryptography")&&!strcmp(n,"RijndaelTransform"));return mode==13?NULL:&crypto_type;
}
static void *get_method(void *c,const char *n,int args){assert(c==&crypto_type&&!strcmp(n,"Decrypt256")&&args==3);return mode==14?NULL:(void *)2;}
static void *get_header(void *m){return mode==5?NULL:(void *)3;}
static const uint8_t *get_code(void *h,uint32_t *size,uint32_t *stack){*size=sizeof(method_il)-(mode==6);*stack=6;return method_il;}
static void free_header(void *h){freed++;}
static void *get_field(void *c,const char *n){
 const char *names[]={"iT0","iT1","iT2","iT3","iSBox"};
 for(int i=0;i<5;i++)if(!strcmp(n,names[i]))return mode==7&&i==3?NULL:(void *)(uintptr_t)(i+1);
 assert(0);return NULL;
}
static void *get_domain(void){return (void *)4;}
static void *get_vtable(void *d,void *t){return (void *)5;}
static void init_class(void *vt){inits++;}
static void get_static(void *vt,void *f,void *out){*(void **)out=&arrays[(uintptr_t)f-1];}
static void *compile(void *m){return mode==10?NULL:native_code;}
static void *get_object_class(void *obj){return obj==self?&crypto_type:((struct array *)obj)->type;}
static char *get_data(void *a,int stride,uintptr_t index){assert(index==0);return (char *)a+16+(mode==9);}
static so_module *so_find_module(const char *n){return (void *)1;}
static uintptr_t so_symbol(so_module *m,const char *n){
 if(mode==3&&!strcmp(n,"mono_runtime_class_init"))return 0;
 #define API_ENTRY(name,fn) if(!strcmp(n,name))return (uintptr_t)fn
 API_ENTRY("mono_get_corlib",get_corlib);API_ENTRY("mono_image_get_guid",get_guid);
 API_ENTRY("mono_class_from_name",get_class);API_ENTRY("mono_class_get_method_from_name",get_method);
 API_ENTRY("mono_method_get_header",get_header);API_ENTRY("mono_method_header_get_code",get_code);
 API_ENTRY("mono_metadata_free_mh",free_header);API_ENTRY("mono_class_get_field_from_name",get_field);
 API_ENTRY("mono_domain_get",get_domain);API_ENTRY("mono_class_vtable",get_vtable);
 API_ENTRY("mono_runtime_class_init",init_class);API_ENTRY("mono_field_static_get_value",get_static);
 API_ENTRY("mono_compile_method",compile);API_ENTRY("mono_object_get_class",get_object_class);
 API_ENTRY("mono_array_addr_with_size",get_data);assert(0);return 0;
}
void *mmap_bridge(void *a,size_t n,int prot,int flags,int fd,off_t off){return mode==12?(void *)-1:trampoline;}
static void kuKernelFlushCaches(void *p,unsigned n){assert(p==trampoline&&n==16);}
static void hook_addr(uintptr_t a,uintptr_t b){assert(a==(uintptr_t)native_code&&b);hooks++;}
static void fallback(void *s,void *i,void *o,void *k){forwarded++;}
'''
tests=r'''
static void reset(int m){
 mode=m;attempted=0;original=NULL;freed=hooks=inits=forwarded=0;fast_calls=fallback_calls=0;
 memcpy(method_il,rijndael256_il,sizeof(method_il));
 if(m==4)method_il[0]^=1;
 native_code[0]=m==11?0xea000001:0xe92d48f0;native_code[1]=0xe24dd080;
 for(int i=0;i<5;i++){
  arrays[i].type=i<4?&word_type:&byte_type;arrays[i].length=256;
  for(int j=0;j<1024;j++)arrays[i].data[j]=(i*97+j*13)&255;
 }
 if(m==8)arrays[0].length=255;
 input.type=output.type=&byte_type;keys.type=&word_type;
 input.length=output.length=40;keys.length=120;
 memset(input.data,0x45,sizeof(input.data));memset(keys.data,0x78,sizeof(keys.data));
}
int main(void){
 assert(sizeof(void *)==8); // Host fixture preserves the Vita array header offsets.
 for(int m=1;m<=14;m++){reset(m);rijndael256_install();assert(!hooks&&!original);}
 reset(0);rijndael256_install();assert(hooks==1&&freed==1&&inits==1);
 assert(trampoline[0]==native_code[0]&&trampoline[1]==native_code[1]&&trampoline[2]==0xe51ff004);
 rijndael256_install();assert(hooks==1&&freed==1);
 original=fallback;uint8_t expected[32];
 rijndael256_block(input.data,expected,(uint32_t *)keys.data,&tables);
 memset(output.data,0xa5,sizeof(output.data));decrypt_block(self,&input,&output,&keys);
 assert(fast_calls==1&&!forwarded&&!memcmp(expected,output.data,32));
 for(int i=32;i<1024;i++)assert(output.data[i]==0xa5);
 decrypt_block(self,&input,&input,&keys);assert(fast_calls==2&&!memcmp(expected,input.data,32));
 decrypt_block(NULL,&input,&output,&keys);decrypt_block(self,NULL,&output,&keys);
 decrypt_block(self,&input,NULL,&keys);decrypt_block(self,&input,&output,NULL);assert(forwarded==4);
 for(int i=0;i<3;i++){
  struct array *a=i==0?&input:i==1?&output:&keys;void *type=a->type;
  a->type=&other_type;decrypt_block(self,&input,&output,&keys);a->type=type;
  unsigned length=a->length;a->length=i<2?31:119;decrypt_block(self,&input,&output,&keys);a->length=length;
 }
 assert(forwarded==10&&fallback_calls==10&&fast_calls==2);
 // Exercise the CBC array guard boundary in the actual block module.
 struct array iv={0},scratch={0};iv.type=scratch.type=&byte_type;iv.length=scratch.length=40;
 memset(iv.data,0x12,sizeof(iv.data));memset(scratch.data,0x34,sizeof(scratch.data));
 uint8_t expected_iv[40],expected_scratch[40],expected_out[40];
 memcpy(expected_iv,iv.data,40);memcpy(expected_scratch,scratch.data,40);memcpy(expected_out,output.data,40);
 rijndael256_cbc_block(input.data,expected_out,expected_iv,expected_scratch,(uint32_t *)keys.data,&tables);
 assert(rijndael256_try_cbc(self,&input,&output,&iv,&scratch,&keys));
 assert(!memcmp(expected_out,output.data,40)&&!memcmp(expected_iv,iv.data,40)&&!memcmp(expected_scratch,scratch.data,40));
 uint8_t snapshots[4][1024];struct array *buffers[]={&input,&output,&iv,&scratch};
 for(int i=0;i<4;i++)memcpy(snapshots[i],buffers[i]->data,1024);
 assert(!rijndael256_try_cbc(NULL,&input,&output,&iv,&scratch,&keys));
 assert(!rijndael256_try_cbc(self,&input,&output,&iv,&iv,&keys));
 assert(!rijndael256_try_cbc(self,&input,&output,&input,&scratch,&keys));
 assert(!rijndael256_try_cbc(self,&input,&output,&iv,&output,&keys));
 assert(!rijndael256_try_cbc(self,&input,&output,&iv,&scratch,NULL));
 for(int i=0;i<4;i++){
  buffers[i]->length=31;assert(!rijndael256_try_cbc(self,&input,&output,&iv,&scratch,&keys));buffers[i]->length=40;
  buffers[i]->type=&other_type;assert(!rijndael256_try_cbc(self,&input,&output,&iv,&scratch,&keys));buffers[i]->type=&byte_type;
 }
 keys.length=119;assert(!rijndael256_try_cbc(self,&input,&output,&iv,&scratch,&keys));keys.length=120;
 for(int i=0;i<4;i++)assert(!memcmp(snapshots[i],buffers[i]->data,1024));
 original=NULL;assert(!rijndael256_try_cbc(self,&input,&output,&iv,&scratch,&keys));
 assert(!safe_prologue(0xe59f0010)&&!safe_prologue(0xe92d8000)&&!safe_prologue(0xe58df004));
 puts("PASS: native Rijndael wrapper, array bounds/types/nulls, exact IL/MVID/API/header/table/layout/prologue guards, rollback, fallback trampoline and idempotence (ASan/UBSan)");
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.c').write_text(harness+src+tests)
 subprocess.run(['cc','-O2','-fsanitize=address,undefined','-I',str(root/'src'),str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)

#!/usr/bin/env python3
"""Run the real CBC installer and dispatcher with guarded Mono/ARM fixtures."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
src='\n'.join(x for x in (root/'src/util/rijndael_cbc.c').read_text().splitlines() if not x.startswith('#include'))
h=r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include "util/rijndael256_il.h"
#include "util/rijndael_cbc_il.h"
#define DATA_PATH "/not-a-directory"
#define L_INFO(...) ((void)0)
#define L_WARN(...) ((void)0)
typedef void so_module;
static int mode,crypto_type,base_type,other_type,hooks,freed,forwarded,tries;
struct subject {void *type;uint8_t encrypt;int32_t block;void *iv,*scratch,*keys;int32_t nb,nr;} subject;
static int in,out,iv,scratch,keys;
static uint8_t cbc_code[sizeof(rijndael_cbc_il)],ecb_code[sizeof(rijndael_ecb_il)];
static uint32_t native[2],trampoline[1024];
static FILE *marker_open(const char *p,const char *m){return mode==1?tmpfile():NULL;}
#define fopen marker_open
static int rijndael256_ready(void){return mode!=15;}
static int rijndael256_try_cbc(void *s,void *i,void *o,void *v,void *t,void *k){
 assert(s==&subject && i==&in && o==&out && v==&iv && t==&scratch && k==&keys);tries++;return mode!=16;
}
static void *corlib(void){return (void *)1;}
static const char *guid(void *im){return mode==2?"other":RIJNDAEL_MVID;}
static void *klass(void *im,const char *ns,const char *n){
 assert(!strcmp(ns,"System.Security.Cryptography"));
 if(!strcmp(n,"RijndaelTransform"))return mode==8?NULL:&crypto_type;
 assert(!strcmp(n,"SymmetricTransform"));return &base_type;
}
static void *parent(void *c){assert(c==&crypto_type);return mode==9?&other_type:&base_type;}
static void *method(void *c,const char *n,int args){assert(args==2);if(c==&base_type){assert(!strcmp(n,"CBC"));return (void *)1;}assert(c==&crypto_type&&!strcmp(n,"ECB"));return (void *)2;}
static void *header(void *m){return mode==6?NULL:m;}
static const uint8_t *code(void *m,uint32_t *size,uint32_t *stack){*stack=8;*size=m==(void *)1?sizeof(cbc_code)-(mode==7):sizeof(ecb_code);return m==(void *)1?cbc_code:ecb_code;}
static void release(void *h){freed++;}
static void *field(void *c,const char *n){
 const char *names[]={"encrypt","BlockSizeByte","temp","temp2","expandedKey","Nb","Nr"};
 for(int i=0;i<7;i++)if(!strcmp(n,names[i])){assert(c==(i<4?&base_type:&crypto_type));return mode==10&&i==4?NULL:(void *)(uintptr_t)(i+1);}
 assert(0);return NULL;
}
static uint32_t offset(void *f){
 unsigned i=(uintptr_t)f-1;uint32_t o[]={offsetof(struct subject,encrypt),offsetof(struct subject,block),offsetof(struct subject,iv),offsetof(struct subject,scratch),offsetof(struct subject,keys),offsetof(struct subject,nb),offsetof(struct subject,nr)};
 return mode==14&&i==3?0:o[i];
}
static void *compile(void *m){assert(m==(void *)1);return mode==12?NULL:native;}
static void *object_type(void *p){return *(void **)p;}
static so_module *so_find_module(const char *n){return (void *)1;}
static uintptr_t so_symbol(so_module *m,const char *n){
 if(mode==3&&!strcmp(n,"mono_field_get_offset"))return 0;
 #define API_ENTRY(nm,fn) if(!strcmp(n,nm))return (uintptr_t)fn
 API_ENTRY("mono_get_corlib",corlib);API_ENTRY("mono_image_get_guid",guid);API_ENTRY("mono_class_from_name",klass);
 API_ENTRY("mono_class_get_parent",parent);API_ENTRY("mono_class_get_method_from_name",method);API_ENTRY("mono_method_get_header",header);
 API_ENTRY("mono_method_header_get_code",code);API_ENTRY("mono_metadata_free_mh",release);API_ENTRY("mono_class_get_field_from_name",field);
 API_ENTRY("mono_field_get_offset",offset);API_ENTRY("mono_compile_method",compile);API_ENTRY("mono_object_get_class",object_type);
 assert(0);return 0;
}
void *mmap_bridge(void *a,size_t n,int p,int f,int fd,off_t o){return mode==13?(void *)-1:trampoline;}
static void kuKernelFlushCaches(void *p,unsigned n){assert(p==trampoline && n==16);}
static void hook_addr(uintptr_t a,uintptr_t b){assert(a==(uintptr_t)native && b);hooks++;}
static void fallback(void *s,void *i,void *o){assert(i==&in&&o==&out);forwarded++;}
'''+src+r'''
static void reset(int m){
 mode=m;attempted=0;original=NULL;crypto_class=NULL;hooks=freed=forwarded=tries=0;fast_calls=fallback_calls=0;
 memcpy(cbc_code,rijndael_cbc_il,sizeof(cbc_code));memcpy(ecb_code,rijndael_ecb_il,sizeof(ecb_code));
 if(m==4)cbc_code[30]^=1;if(m==5)ecb_code[30]^=1;
 native[0]=m==11?0xea000001:0xe92d48f0;native[1]=0xe24dd040;
 subject=(struct subject){&crypto_type,0,32,&iv,&scratch,&keys,8,14};
}
int main(void){
 for(int m=1;m<=15;m++){reset(m);rijndael_cbc_install();assert(!hooks&&!original);}
 reset(0);rijndael_cbc_install();assert(hooks==1 && freed==2 && original);
 assert(trampoline[0]==native[0]&&trampoline[1]==native[1]&&trampoline[2]==0xe51ff004);
 rijndael_cbc_install();assert(hooks==1);
 original=fallback;decrypt_cbc(&subject,&in,&out);assert(tries==1&&fast_calls==1&&!forwarded);
 subject.encrypt=1;decrypt_cbc(&subject,&in,&out);subject.encrypt=0;
 subject.block=16;decrypt_cbc(&subject,&in,&out);subject.block=32;
 subject.nb=4;decrypt_cbc(&subject,&in,&out);subject.nb=8;
 subject.nr=12;decrypt_cbc(&subject,&in,&out);subject.nr=14;
 subject.type=&other_type;decrypt_cbc(&subject,&in,&out);subject.type=&crypto_type;
 decrypt_cbc(NULL,&in,&out);assert(forwarded==6&&tries==1);
 mode=16;decrypt_cbc(&subject,&in,&out);assert(forwarded==7&&tries==2&&fallback_calls==7);
 assert(rijndael_cbc_fast_calls()==1 && rijndael_cbc_fallback_calls()==7);
 puts("PASS: real CBC guards, mode/block/round/class fallback, failed-array forwarding, original trampoline, IL/MVID/API/layout checks and idempotence (ASan/UBSan)");
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.c').write_text(h)
 subprocess.run(['cc','-fsanitize=address,undefined','-I',str(root/'src'),str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)

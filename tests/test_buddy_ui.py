#!/usr/bin/env python3
"""Exercise real buddy idle guard and installation/fallback with ASan/UBSan."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
source = '\n'.join(l for l in (root/'src/util/buddy_ui.c').read_text().splitlines() if not l.startswith('#include'))
harness = r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include "util/buddy_cell_il.h"
#define DATA_PATH "/nonexistent/srr-fixture"
#define L_INFO(...) ((void)0)
#define L_WARN(...) ((void)0)
typedef struct { int id; } so_module;
static so_module mono;
struct cell { void *klass; void *sync; uint8_t loaded, drawn; float draw, check; } fixture;
static int cell_type, panel_type, camera_type, update_method, panel_method, camera_method, init_method;
struct field { unsigned offset; int kind; } fields[4];
static uint8_t code_bytes[sizeof(buddy_cell_il)];
static int marker, missing_api, wrong_guid, wrong_size, missing_field, freed, registered, menu_registered;
static int (*registered_filter)(void *);
static FILE *open_marker(const char *p,const char *mode) { return marker ? tmpfile() : NULL; }
#define fopen open_marker
static so_module *so_find_module(const char *n) { return &mono; }
static void *get_class(void *image,const char *ns,const char *n) {
 if(!strcmp(n,"ui_chao_set_cell"))return &cell_type;
 if(!strcmp(n,"UIPanel"))return &panel_type;
 if(!strcmp(n,"UICamera"))return &camera_type;
 return NULL;
}
static void *get_method(void *c,const char *n,int argc) {
 assert(argc==0);
 if(c==&cell_type)return &update_method;
 if(c==&panel_type)return &panel_method;
 if(c==&camera_type)return &camera_method;
 return NULL;
}
static void *get_header(void *m) { assert(m==&update_method);return m; }
static const uint8_t *get_code(void *h,uint32_t *size,uint32_t *stack) {
 *size=wrong_size ? sizeof(code_bytes)-1 : sizeof(code_bytes);*stack=2;return code_bytes;
}
static void free_header(void *h) { freed++; }
static const char *get_guid(void *image) { return wrong_guid ? "wrong" : "4900a8e6-ea01-4e8a-ae66-036836545dd2"; }
static void *get_field(void *c,const char *n) {
 const char *names[]={"m_isLoadCmp","m_isDrawChao","m_drawDelay","m_checkDelay"};
 for(int i=0;i<4;i++)if(!strcmp(names[i],n))return missing_field==i+1 ? NULL : &fields[i];
 return NULL;
}
static uint32_t get_offset(void *f) { return ((struct field *)f)->offset; }
static void *get_field_type(void *f) { return f; }
static int get_kind(void *f) { return ((struct field *)f)->kind; }
static int get_size(void *c) { return sizeof(fixture); }
static void *get_object_class(void *obj) { return ((struct cell *)obj)->klass; }
static void *get_token(void *image,uint32_t token,void *klass) { assert(token==0x060048a0);return &init_method; }
static uintptr_t so_symbol(so_module *m,const char *n) {
 if(missing_api && !strcmp(n,"mono_type_get_type"))return 0;
 #define API_ENTRY(name,fn) if(!strcmp(n,name))return (uintptr_t)fn
 API_ENTRY("mono_class_from_name",get_class);API_ENTRY("mono_class_get_method_from_name",get_method);
 API_ENTRY("mono_method_get_header",get_header);API_ENTRY("mono_method_header_get_code",get_code);
 API_ENTRY("mono_metadata_free_mh",free_header);API_ENTRY("mono_image_get_guid",get_guid);
 API_ENTRY("mono_class_get_field_from_name",get_field);API_ENTRY("mono_field_get_offset",get_offset);
 API_ENTRY("mono_field_get_type",get_field_type);API_ENTRY("mono_type_get_type",get_kind);
 API_ENTRY("mono_get_method",get_token);
 API_ENTRY("mono_class_instance_size",get_size);API_ENTRY("mono_object_get_class",get_object_class);
 return 0;
}
static void managed_trace_idle_method(void *method,int (*filter)(void *)) {
 assert(method==&update_method);registered++;registered_filter=filter;
}
static void managed_trace_menu_methods(void *p,void *c,void *cam,void *init) {
 assert(p==&panel_method && c==&update_method && cam==&camera_method && init==&init_method);menu_registered++;
}
'''
tests = r'''
static void reset(void) {
 attempted=0;registered=menu_registered=freed=0;registered_filter=NULL;
 marker=missing_api=wrong_guid=wrong_size=missing_field=0;
 memcpy(code_bytes,buddy_cell_il,sizeof(code_bytes));
 fields[0]=(struct field){offsetof(struct cell,loaded),2};
 fields[1]=(struct field){offsetof(struct cell,drawn),2};
 fields[2]=(struct field){offsetof(struct cell,draw),12};
 fields[3]=(struct field){offsetof(struct cell,check),12};
 fixture=(struct cell){.klass=&cell_type,.loaded=1,.drawn=1,.draw=-1,.check=0};
}
int main(void) {
 reset();buddy_ui_install(NULL);assert(!attempted);
 marker=1;buddy_ui_install((void *)1);assert(!registered);
 reset();missing_api=1;buddy_ui_install((void *)1);assert(!registered);
 reset();wrong_guid=1;buddy_ui_install((void *)1);assert(!registered && freed==1);
 reset();wrong_size=1;buddy_ui_install((void *)1);assert(!registered && freed==1);
 reset();code_bytes[0]^=1;buddy_ui_install((void *)1);assert(!registered);
 for(int i=1;i<=4;i++){reset();missing_field=i;buddy_ui_install((void *)1);assert(!registered);}
 reset();fields[2].kind=8;buddy_ui_install((void *)1);assert(!registered);
 reset();fields[3].offset=sizeof(fixture);buddy_ui_install((void *)1);assert(!registered);
 reset();fields[0].offset=0;buddy_ui_install((void *)1);assert(!registered);
 reset();buddy_ui_install((void *)1);assert(registered==1 && menu_registered==1 && freed==1);
 buddy_ui_install((void *)1);assert(registered==1);
 assert(registered_filter(&fixture));assert(!registered_filter(NULL));
 fixture.klass=&panel_type;assert(!registered_filter(&fixture));fixture.klass=&cell_type;
 for(int loaded=0;loaded<=1;loaded++)for(int drawn=0;drawn<=1;drawn++) {
   fixture.loaded=loaded;fixture.drawn=drawn;
   float times[]={-INFINITY,-1,-0.0f,0,0.00001f,1,INFINITY,NAN};
   for(unsigned d=0;d<8;d++)for(unsigned c=0;c<8;c++) {
     fixture.draw=times[d];fixture.check=times[c];
     assert(registered_filter(&fixture)==(loaded && drawn && !(times[d]>0) && !(times[c]>0)));
   }
 }
 puts("PASS: buddy exact IL/MVID/type/offset guards, rollback, idempotence and pending/NaN timer handling");
}
'''
with tempfile.TemporaryDirectory() as d:
    p=Path(d);(p/'test.c').write_text(harness+source+tests)
    subprocess.run(['cc','-fsanitize=address,undefined','-I',str(root/'src'),str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)

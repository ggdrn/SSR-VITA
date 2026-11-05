#!/usr/bin/env python3
"""Exercise actual Unity scheduler policy and presentation interval selection."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/mono_probe.c').read_text()
fn=s[s.index('static void race_frame_rate_tick(void)'):s.index('// Native integer/boolean getters')]
menu=(root/'src/port_menu.c').read_text()
init=menu[menu.index('void port_menu_init(void)'):menu.index('// --- Settings screen')]
h=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef void MonoClass,MonoMethod;
typedef struct { int id; } so_module;
static so_module mono,unity,foreign;
static int engine_rate,sets,selected_fps=30,mode,interval;
static void *unity_image=(void *)1;
static int port_target_fps(void){return selected_fps;}
static int get_rate(void){return engine_rate;}
static void set_rate(int v){sets++;engine_rate=v;}
static MonoClass *class_from_name(void *image,const char *ns,const char *name){
 assert(image==unity_image && !strcmp(ns,"UnityEngine") && !strcmp(name,"Application"));return (void *)1;
}
static MonoMethod *get_method(MonoClass *k,const char *n,int argc){
 if(!strcmp(n,"get_targetFrameRate")){assert(!argc);return (void *)1;}
 assert(!strcmp(n,"set_targetFrameRate") && argc==1);return (void *)2;
}
static struct {MonoClass *(*class_from_name)(void *,const char *,const char *);MonoMethod *(*get_method)(MonoClass *,const char *,int);} api={class_from_name,get_method};
static void *lookup(MonoMethod *m){return mode==1?NULL:m==(void *)1?(void *)get_rate:(void *)set_rate;}
static so_module *so_find_module(const char *n){return !strcmp(n,"libmono.so")?&mono:&unity;}
static void *so_symbol(so_module *m,const char *n){assert(m==&mono && !strcmp(n,"mono_lookup_internal_call"));return lookup;}
static so_module *so_module_at(uintptr_t a){return mode==2?&foreign:&unity;}
#define L_INFO(...) ((void)0)
static struct {int fps,fps_counter;} cfg;
static void fps_display_callback(void *p){}
static void vglSetDisplayCallback(void (*cb)(void *)){assert(cb==fps_display_callback);}
static void gl_set_swap_interval(int v){interval=v;}
'''+fn+init+r'''
int main(int argc,char **argv){
 mode=argc>1?argv[1][0]-'0':0;
 cfg.fps=30;port_menu_init();assert(interval==2);
 cfg.fps=60;port_menu_init();assert(interval==1);
 engine_rate=30;race_frame_rate_tick();
 if(mode){assert(!sets && engine_rate==30);return 0;}
 assert(sets==1 && engine_rate==60 && selected_fps==30);
 race_frame_rate_tick();assert(sets==1); // no writes while rate matches
 engine_rate=30;race_frame_rate_tick();assert(sets==2 && engine_rate==60); // game reset
 selected_fps=60;race_frame_rate_tick();assert(sets==2 && engine_rate==60);
 engine_rate=-1;race_frame_rate_tick();assert(sets==3 && engine_rate==60);
 puts("PASS: display cap 30/60 keeps intervals 2/1; Unity scheduler 60, game resets and missing/foreign APIs guarded");
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.c').write_text(h)
 subprocess.run(['cc','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
 for mode in range(3):subprocess.run([str(p/'test'),str(mode)],check=True)

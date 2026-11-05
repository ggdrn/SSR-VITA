#!/usr/bin/env python3
"""Exercise the actual pre-game menu with simulated controller/touch input."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
import xml.etree.ElementTree as ET
livearea = ET.parse(root / 'sce_sys/livearea/contents/template.xml')
assert livearea.find('.//target').text == 'psla:-settings'
main = (root / 'src/main.c').read_text()
assert main.index('port_settings_screen()') < main.index('  check_requirements();')
assert main.index('port_settings_screen()') < main.index('  load_libraries();')
source = (root / 'src/port_menu.c').read_text()
# Host substitutes only the Vita boundary; drawing, settings and input are real.
source = '\n'.join(line for line in source.splitlines() if not line.startswith('#include'))
harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "port_menu.h"
#include "util/port_font.h"
#define DATA_PATH "."
#define SCREEN_MAX_W 960
#define SCREEN_MAX_H 544
#define L_WARN(...) ((void)0)
#define L_INFO(...) ((void)0)
#define SCE_CTRL_UP 1
#define SCE_CTRL_DOWN 2
#define SCE_CTRL_LEFT 4
#define SCE_CTRL_RIGHT 8
#define SCE_CTRL_CROSS 16
#define SCE_CTRL_CIRCLE 32
#define SCE_CTRL_SELECT 64
#define SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW 0
#define SCE_DISPLAY_PIXELFORMAT_A8B8G8R8 0
#define SCE_DISPLAY_SETBUF_NEXTFRAME 0
#define SCE_TOUCH_PORT_FRONT 0
typedef enum { SCE_TOUCH_SAMPLING_STATE_STOP, SCE_TOUCH_SAMPLING_STATE_START } SceTouchSamplingState;
typedef int SceUID;
typedef struct { unsigned buttons; } SceCtrlData;
typedef struct { int id, x, y; } Report;
typedef struct { unsigned reportNum; Report report[2]; } SceTouchData;
typedef struct { unsigned size; void *base; int pitch, pixelformat, width, height; } SceDisplayFrameBuf;
int DISPLAY_WIDTH=960, DISPLAY_HEIGHT=544, DISPLAY_STRIDE=960;
void gl_set_swap_interval(int i) {}
void vglSetDisplayCallback(void (*cb)(void *)) {}
void vglSwapBuffers(int common) {}
typedef struct { unsigned type; char dat[1024]; } SceAppUtilAppEventParam;
static int event_receive, event_parse, event_type, restarts;
static const char *event_arg="", *app_param="";
static int sceAppUtilReceiveAppEvent(SceAppUtilAppEventParam *e) {
 e->type=event_type;return event_receive;
}
static int sceAppUtilAppEventParseLiveArea(const SceAppUtilAppEventParam *e,char *p) {
 assert(e->type==5);strcpy(p,event_arg);return event_parse;
}
static int sceAppMgrGetAppParam(char *p) { strcpy(p,app_param);return 0; }
static int sceAppMgrLoadExec(const char *path,char *const argv[],void *opt) {
 assert(!strcmp(path,"app0:/eboot.bin"));assert(!strcmp(argv[0],"settings"));restarts++;return -1;
}
static void log_finish(void) {}
static void *memory;
static SceTouchSamplingState sampling;
static int frame, count;
static struct { unsigned buttons, fingers; int x,y; } frames[32];
static SceUID sceKernelAllocMemBlock(const char *n,int t,unsigned size,void *o) { memory=malloc(size);assert(memory);return 1; }
static int sceKernelGetMemBlockBase(SceUID b,void **p) { *p=memory;return 0; }
static int sceKernelFreeMemBlock(SceUID b) { free(memory);memory=NULL;return 0; }
static int sceDisplaySetFrameBuf(SceDisplayFrameBuf *fb,int t) { return 0; }
static int sceDisplayWaitVblankStart(void) { frame++;return 0; }
static uint64_t sceKernelGetProcessTimeWide(void) { return (uint64_t)(frame+1)*16667; }
static int sceCtrlPeekBufferPositive(int p,SceCtrlData *pad,int n) {
 assert(frame<count);pad->buttons=frames[frame].buttons;return 1;
}
static int sceTouchGetSamplingState(int p,SceTouchSamplingState *s) { *s=sampling;return 0; }
static int sceTouchSetSamplingState(int p,SceTouchSamplingState s) { sampling=s;return 0; }
static int sceTouchPeek(int p,SceTouchData *t,int n) {
 t->reportNum=frames[frame].fingers;
 t->report[0]=(Report){1,frames[frame].x*2,frames[frame].y*2};return 1;
}
''' + source + r'''
static void reset(void) {
 frame=-1;count=0;sampling=SCE_TOUCH_SAMPLING_STATE_STOP;
 memset(frames,0,sizeof(frames));
 cfg.resolution=PORT_RES_QHD;cfg.quality=PORT_QUALITY_DEFAULT;cfg.material_shaders=1;
}
static void input(unsigned buttons,unsigned fingers,int x,int y) {
 frames[count].buttons=buttons;frames[count].fingers=fingers;
 frames[count].x=x;frames[count++].y=y;
}
static void tap(int x,int y) { input(0,1,x,y);input(0,0,0,0); }
static void run(int expected) {
 assert(port_settings_screen()==expected);assert(!memory);
 assert(sampling==SCE_TOUCH_SAMPLING_STATE_STOP);
}
int main(int argc,char **argv) {
 // Low retains Light engine quality while preserving half-size font atlases.
 cfg.quality=PORT_QUALITY_LOW;
 assert(port_unity_quality_level()==0 && port_texture_limit()==1);
 cfg.quality=PORT_QUALITY_DEFAULT;
 assert(port_unity_quality_level()==1 && port_texture_limit()==1);
 assert(PORT_QUALITY_COUNT==2);
 char value[32];row_value(ROW_GRAPHICS,value,sizeof(value));assert(!strcmp(value,"Default"));
 // Saved High values migrate to Default through the existing range clamp.
 FILE *legacy=fopen(PORT_SETTINGS_PATH,"w");assert(legacy);
 fputs("graphics=2\n",legacy);fclose(legacy);port_settings_load();
 assert(port_quality()==PORT_QUALITY_DEFAULT && port_unity_quality_level()==1);
 // Older shader choices must not restore an original-shader option.
 FILE *old=fopen(PORT_SETTINGS_PATH,"w");assert(old);
 fputs("graphics=0\ncharacter_shader_original=1\n",old);fclose(old);
 port_settings_load();assert(port_quality()==PORT_QUALITY_LOW);
 assert(port_unity_quality_level()==0 && port_texture_limit()==1);
 assert(ROW_COUNT==7 && port_material_shaders()==1);
 row_value(ROW_MATERIAL_SHADERS,value,sizeof(value));assert(!strcmp(value,"On"));
 // Touch toggles the setting, saves it, and the next launch restores Off.
 reset();input(0,0,0,0);tap(700,row_y(ROW_MATERIAL_SHADERS));
 tap(500,row_y(ROW_START));run(1);assert(!port_material_shaders());
 cfg.material_shaders=1;port_settings_load();assert(!port_material_shaders());
 row_value(ROW_MATERIAL_SHADERS,value,sizeof(value));assert(!strcmp(value,"Off"));
 reset();input(0,0,0,0);tap(200,row_y(ROW_MATERIAL_SHADERS));
 tap(200,row_y(ROW_MATERIAL_SHADERS));tap(500,row_y(ROW_START));run(1);
 assert(port_material_shaders());
 // Exercise event detection before running any game/menu work.
 reset();input(0,0,0,0);frame=0;
 assert(!port_settings_requested(0,NULL));
 event_type=5;event_arg="-settings";assert(port_settings_requested(0,NULL));
 port_settings_poll();assert(restarts==1);
 event_arg="eboot";assert(!port_settings_requested(0,NULL));
 event_arg="-settings";event_parse=-1;assert(!port_settings_requested(0,NULL));
 event_parse=0;event_receive=-1;assert(!port_settings_requested(0,NULL));
 event_receive=0;event_type=0;assert(!port_settings_requested(0,NULL));
 char *args[]={"-settings"};assert(port_settings_requested(1,args));
 args[0]="settings";assert(port_settings_requested(1,args));
 frames[0].buttons=SCE_CTRL_SELECT;assert(port_settings_requested(0,NULL));
 reset();input(0,0,0,0);input(SCE_CTRL_CROSS,0,0,0);run(1);
 reset();input(0,0,0,0);tap(500,row_y(ROW_START));run(1);
 reset();input(0,0,0,0);tap(500,row_y(ROW_EXIT));run(0);
 reset();input(0,0,0,0);input(SCE_CTRL_CIRCLE,0,0,0);run(0);
 // A held launch touch must not start the game on release.
 reset();input(0,1,500,row_y(ROW_START));input(0,0,0,0);
 tap(500,row_y(ROW_EXIT));run(0);
 // Dragging across rows must not activate either action.
 reset();input(0,0,0,0);input(0,1,500,row_y(ROW_START));
 input(0,1,500,row_y(ROW_EXIT));input(0,0,0,0);
 tap(500,row_y(ROW_START));run(1);
 // Two fingers cancel the gesture.
 reset();input(0,0,0,0);input(0,1,500,row_y(ROW_EXIT));
 input(0,2,500,row_y(ROW_EXIT));input(0,0,0,0);
 tap(500,row_y(ROW_START));run(1);
 reset();input(0,0,0,0);tap(700,row_y(ROW_RESOLUTION));
 tap(500,row_y(ROW_START));run(1);assert(port_screen_w==640);
 cfg.resolution=PORT_RES_QHD;port_settings_load();assert(port_screen_w==640);
 reset();input(0,0,0,0);tap(200,row_y(ROW_RESOLUTION));
 tap(500,row_y(ROW_START));run(1);assert(port_screen_w==720);
 reset();input(0,0,0,0);input(SCE_CTRL_UP,0,0,0);input(0,0,0,0);
 input(SCE_CTRL_CROSS,0,0,0);input(0,0,0,0);input(SCE_CTRL_DOWN,0,0,0);
 input(SCE_CTRL_CROSS,0,0,0);run(1);assert(cfg.fps_counter==1);
 // Render every selection under ASan; verify black background and hit bounds.
 uint32_t *fb=malloc(FB_W*FB_H*4);
 for(int r=0;r<ROW_COUNT;r++) {
  draw_screen(fb,r);assert(fb[80*FB_W+10]==RGB(0,0,0));
  assert(row_at(50,row_y(r))==r);
 }
 assert(row_at(0,100)==-1);assert(row_at(50,400)==-1);
 if(argc>1) {
  if(argc>2) {shader_phase=1;shader_done=42;shader_total=150;shader_prepared=40;shader_progress_callback(fb);}
  else draw_screen(fb,ROW_START);FILE *f=fopen(argv[1],"wb");assert(f);
  fprintf(f,"P6\n960 544\n255\n");
  for(int i=0;i<FB_W*FB_H;i++) { uint32_t c=fb[i];fputc(c&255,f);fputc((c>>8)&255,f);fputc((c>>16)&255,f); }
  fclose(f);
 }
 free(fb);puts("PASS: menu launch/exit, touch gestures, controller, saved settings and black render");
}
'''
with tempfile.TemporaryDirectory() as d:
    p = Path(d)
    (p / 'test.c').write_text(harness)
    subprocess.run(['cc', '-fsanitize=address,undefined', '-I', str(root / 'src'),
                    str(p / 'test.c'), '-o', str(p / 'test')], check=True)
    import sys
    subprocess.run([str(p / 'test'), *sys.argv[1:]], cwd=p, check=True)

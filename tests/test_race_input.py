#!/usr/bin/env python3
"""Test actual pointer/button state transitions, independent touches and pause."""
from pathlib import Path
import subprocess, tempfile, re
root = Path(__file__).resolve().parents[1]
src = (root/'src/bridges/input_bridge.c').read_text()
constants = '\n'.join(l for l in src.splitlines() if l.startswith('#define ') and not l.startswith('#define FAKE_QUEUE'))
types = src[src.index('typedef struct {'):src.index('static input_event queue')]
logic = src[src.index('typedef struct {',src.index('// --- Vita polling')):src.index('void input_poll(void)')]
buttons = sorted(set(re.findall(r'SCE_CTRL_\w+',logic)))
harness = '#include <assert.h>\n#include <stdint.h>\n#include <stdio.h>\n#include <string.h>\n'+constants+'\n'+types
harness += '\n'.join(f'#define {b} (1u<<{i})' for i,b in enumerate(buttons))
harness += r'''
#define SCREEN_W 960
#define SCREEN_H 544
#define OBS_INPUT 0
#define observe_span(...) ((void)0)
static int64_t now_ns(void) { return 123456; }
static input_event events[64];static unsigned count;
static void push_event(const input_event *e) { assert(count<64);events[count++]=*e; }
'''+logic+r'''
int main(void) {
 update_virtual_buttons(SCE_CTRL_SQUARE,0);assert(!count);
 input_set_race_controls(1);
 update_virtual_buttons(SCE_CTRL_SQUARE,SCE_CTRL_SQUARE);assert(!count);
 for(int i=0;i<6;i++) pointer_down(i,10,20);
 uint32_t both=SCE_CTRL_SQUARE|SCE_CTRL_TRIANGLE;
 update_virtual_buttons(both,0);assert(count==8&&active_count()==8);
 assert(events[6].pointer_count==7&&events[6].action==(5|(6<<8)));
 assert(events[6].pointers[6].id==6&&events[6].pointers[6].x==300&&events[6].pointers[6].y==480);
 assert(events[7].pointer_count==8&&events[7].pointers[7].id==7&&events[7].pointers[7].x==675);
 update_virtual_buttons(both,both);assert(count==8);
 update_virtual_buttons(SCE_CTRL_TRIANGLE,both);assert(count==9&&active_count()==7);
 assert(events[8].action==(6|(6<<8))&&events[8].pointer_count==8);
 assert(slots[5].active&&slots[7].active&&!slots[6].active);
 input_set_race_controls(0);update_virtual_buttons(SCE_CTRL_TRIANGLE,SCE_CTRL_TRIANGLE);
 assert(count==10&&!slots[7].active&&slots[5].active);
 assert(events[9].action==(6|(6<<8))&&events[9].pointers[6].id==7);
 unsigned before=count;
 update_virtual_buttons(SCE_CTRL_START,0);
 update_virtual_buttons(SCE_CTRL_START|SCE_CTRL_LTRIGGER,SCE_CTRL_START);
 update_virtual_buttons(SCE_CTRL_LTRIGGER,SCE_CTRL_START|SCE_CTRL_LTRIGGER);
 update_virtual_buttons(0,SCE_CTRL_LTRIGGER);
 assert(count==before+2&&events[before].keycode==4&&events[before].action==0&&events[before+1].action==1);
 for(int i=0;i<6;i++)pointer_up(i);assert(!active_count());
 puts("PASS: jump + item + swap + five fingers, held-button gating, release on pause, aggregate BACK");
}
'''
with tempfile.TemporaryDirectory() as d:
    p=Path(d);(p/'test.c').write_text(harness)
    subprocess.run(['cc','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)

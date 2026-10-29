#!/usr/bin/env python3
"""Heavy managed snapshots must not repeat while the same stage is running."""
from pathlib import Path
import subprocess
import tempfile
src = (Path(__file__).resolve().parents[1]/'src/mono_probe.c').read_text()
a = src.index('void mono_probe_tick(unsigned frame) {')
harness = r'''
#include <string.h>
#include <assert.h>
#include <stdio.h>
static int unity_image = 1, sound_requests, snapshots, gc_playing_scene;
static const char *scene = "MainMenu";
static int probe_init(void) { return 1; }
static void *call(const char *c,const char *m,void *s,void *a,int n) { return (void *)scene; }
static void string_of(void *s,char *out,size_t n) { snprintf(out,n,"%s",(char *)s); }
static void mono_probe(void) { snapshots++; }
'''
harness += src[a:]
harness += r'''
int main(void) {
  mono_probe_tick(300); assert(snapshots == 1);
  mono_probe_tick(900); assert(snapshots == 2);
  scene = "s_playingstage"; mono_probe_tick(1200); assert(snapshots == 3);
  for (unsigned f = 1500; f < 15000; f += 300) mono_probe_tick(f);
  assert(snapshots == 3);
  scene = "MainMenu"; mono_probe_tick(15000); assert(snapshots == 4);
  puts("PASS: menu snapshots retained, one snapshot per stage entry, no periodic stage traversal");
}
'''
with tempfile.TemporaryDirectory() as d:
    p = Path(d)
    (p/'test.c').write_text(harness)
    subprocess.run(['cc','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)

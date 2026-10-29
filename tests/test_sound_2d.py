#!/usr/bin/env python3
"""Check that the compatibility adjustment targets only the game's SE source."""
from pathlib import Path
import subprocess
import tempfile
src = (Path(__file__).resolve().parents[1] / 'src/mono_probe.c').read_text()
a = src.index('static MonoObject *instance_call(')
b = src.index('static void sound_snapshot', a)
harness = r'''
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
typedef void MonoClass, MonoMethod, MonoField;
typedef struct Obj { uint8_t spatial; struct Obj *atom; } MonoObject;
static void *game_image = (void *)1;
static MonoObject music = {1, 0}, atom = {1, 0}, source = {0, &atom};
static int requests, sets, absent;
#define L_INFO(...) ((void)0)
#define L_WARN(...) ((void)0)
static MonoObject *call_in(void *img, const char *ns, const char *cls, const char *name,
                          void *self, void **args, int n) {
  assert(strcmp(cls,"SoundManager") == 0 && strcmp(name,"GetSource") == 0);
  assert(n == 1 && *(int *)args[0] == 2); requests++;
  return absent ? NULL : &source;
}
static MonoClass *klass(MonoObject *o) { return (MonoClass *)o; }
static MonoMethod *method(MonoClass *k, const char *name, int n) { return (MonoMethod *)name; }
static MonoField *field(MonoClass *k, const char *name) {
  assert(strcmp(name,"m_atomSource") == 0); return (void *)1;
}
static void value(MonoObject *o, MonoField *f, void *p) { *(MonoObject **)p = o->atom; }
static MonoObject *invoke(MonoMethod *m, void *self, void **args, MonoObject **exc) {
  MonoObject *o = self;
  if (strcmp((char *)m,"get_use3dPositioning") == 0) return o;
  assert(strcmp((char *)m,"set_use3dPositioning") == 0);
  assert(*(uint8_t *)args[0] == 0); o->spatial = 0; sets++; return NULL;
}
static void *unbox(MonoObject *o) { return &o->spatial; }
static struct {
  MonoClass *(*object_get_class)(MonoObject *);
  MonoMethod *(*get_method)(MonoClass *, const char *, int);
  MonoObject *(*invoke)(MonoMethod *, void *, void **, MonoObject **);
  void *(*object_unbox)(MonoObject *);
  MonoField *(*get_field)(MonoClass *, const char *);
  void (*field_value)(MonoObject *, MonoField *, void *);
} api = {klass,method,invoke,unbox,field,value};
'''
harness += src[a:b]
harness += r'''
int main(void) {
  mono_audio_tick(); assert(requests == 1 && sets == 1 && atom.spatial == 0 && music.spatial == 1);
  mono_audio_tick(); assert(sets == 1); // no repeated native parameter update
  absent = 1; mono_audio_tick(); assert(sets == 1);
  game_image = NULL; requests = 0; mono_audio_tick(); assert(requests == 0);
  puts("PASS: only SE channel 2 changes, BGM preserved, no repeated update, missing source safe");
}
'''
with tempfile.TemporaryDirectory() as d:
    p = Path(d)
    (p/'test.c').write_text(harness)
    subprocess.run(['cc', '-fsanitize=address,undefined', str(p/'test.c'), '-o', str(p/'test')], check=True)
    subprocess.run([str(p/'test')], check=True)

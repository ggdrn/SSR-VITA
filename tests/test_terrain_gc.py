#!/usr/bin/env python3
"""Exercise the real guarded terrain patch; verify only the GC call changes."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
src = (root / 'src/mono_probe.c').read_text()
a = src.index('static void patch_terrain_gc_il(void)')
b = src.index('// These finally blocks', a)
harness = r'''
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <assert.h>
#include <stdio.h>
#include "util/terrain_gc.h"
#include "util/race_warmup.h"
typedef void MonoImage, MonoMethod, MonoClass, so_module;
static MonoImage *game_image = (void *)1;
static uint8_t code[sizeof(terrain_create_il)];
static int mode, writes, flushes, releases;
#define L_INFO(...) ((void)0)
#define L_WARN(...) ((void)0)
static so_module *so_find_module(const char *n) { return (void *)1; }
static const char *guid(MonoImage *i) { return mode == 1 ? "other" : RACE_WARMUP_MVID; }
static MonoMethod *get_method(MonoImage *i, uint32_t token, MonoClass *c) {
  assert(i == game_image && token == 0x06004c98 && !c); return (void *)2;
}
static void *get_header(MonoMethod *m) { return mode == 4 ? NULL : (void *)3; }
static const uint8_t *get_code(void *h, uint32_t *size, uint32_t *stack) {
  *size = sizeof(code) - (mode == 2); return code;
}
static void free_header(void *h) { assert(h == (void *)3); releases++; }
static void *so_symbol(so_module *m, const char *n) {
  if (!strcmp(n, "mono_image_get_guid")) return guid;
  if (!strcmp(n, "mono_get_method")) return get_method;
  if (!strcmp(n, "mono_method_get_header")) return get_header;
  if (!strcmp(n, "mono_method_header_get_code")) return mode == 5 ? NULL : get_code;
  if (!strcmp(n, "mono_metadata_free_mh")) return free_header;
  assert(0); return NULL;
}
static void kuKernelCpuUnrestrictedMemcpy(void *dst, const void *src, unsigned size) {
  assert(dst == code + 0x2c9 && size == 5); writes++; memcpy(dst, src, size);
}
static void kuKernelFlushCaches(void *dst, unsigned size) {
  assert(dst == code + 0x2c9 && size == 5); flushes++;
}
'''
harness += src[a:b]
harness += r'''
int main(int argc, char **argv) {
  mode = atoi(argv[1]); memcpy(code, terrain_create_il, sizeof(code));
  if (mode == 3) code[10] ^= 1;
  uint8_t before[sizeof(code)]; memcpy(before, code, sizeof(code));
  patch_terrain_gc_il();
  if (mode == 0) {
    assert(writes == 1 && flushes == 1 && releases == 1);
    for (unsigned n = 0; n < sizeof(code); n++)
      assert(code[n] == (n >= 0x2c9 && n < 0x2ce ? 0 : before[n]));
    patch_terrain_gc_il(); assert(writes == 1 && flushes == 1 && releases == 2);
  } else { assert(!writes && !flushes && !memcmp(before, code, sizeof(code))); }
  return 0;
}
'''
with tempfile.TemporaryDirectory() as d:
    p = Path(d)
    (p / 'test.c').write_text(harness)
    subprocess.run(['cc', '-fsanitize=address,undefined', '-I', str(root / 'src'), str(p / 'test.c'), '-o', str(p / 'test')], check=True)
    for mode in range(6):
        subprocess.run([str(p / 'test'), str(mode)], check=True)
print('PASS: terrain patch: only 5 GC bytes changed; rejects MVID, size, body, missing header/API; no repeated edit')

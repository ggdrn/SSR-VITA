#!/usr/bin/env python3
"""Run the real preload function against Mono/memory stubs, without a Vita."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
src = (root / 'src/mono_probe.c').read_text()
fn = src[src.index('static void race_jit_warmup(void)'):src.index('static MonoObject *traced_bundle_load', src.index('static void race_jit_warmup(void)'))]
harness = r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <assert.h>
#include <stdlib.h>
#include "util/race_warmup.h"
typedef void MonoDomain, MonoImage, MonoClass, MonoMethod, so_module;
#define L_INFO(...) ((void)0)
#define L_WARN(...) ((void)0)
static MonoImage *game_image = (void *)1;
static int mode, calls, lookups, domain_id = 1;
static MonoDomain *domain_get(void) { return (void *)(uintptr_t)domain_id; }
static void *compile_method(MonoMethod *method) {
  assert(method); calls++;
  return mode == 4 ? NULL : (void *)1;
}
static struct { MonoDomain *(*domain_get)(void); void *(*compile_method)(MonoMethod *); } api = {domain_get, compile_method};
static uint64_t sceKernelGetProcessTimeWide(void) { return 1000; }
typedef struct { uint64_t total_free; } native_sample;
static native_sample native_heap_available(void) {
  return (native_sample){ mode == 2 ? 63u * 1024 * 1024 : 128u * 1024 * 1024 };
}
static so_module *so_find_module(const char *n) { return (void *)1; }
static const char *guid(MonoImage *i) { return mode == 1 ? "other-assembly" : RACE_WARMUP_MVID; }
static MonoMethod *get_method(MonoImage *i, uint32_t token, MonoClass *c) {
  assert(!c);
  for (unsigned n = 0; n < sizeof(race_warmup_methods) / sizeof(*race_warmup_methods); n++)
    if (race_warmup_methods[n].token == token) { lookups++; return (void *)(uintptr_t)(n + 1); }
  assert(0); return NULL;
}
static void *get_header(MonoMethod *m) { return m; }
static const uint8_t *get_code(void *h, uint32_t *size, uint32_t *stack) {
  unsigned n = (uintptr_t)h - 1;
  *size = race_warmup_methods[n].il_size + (mode == 3);
  return (const uint8_t *)1;
}
static void free_header(void *h) { assert(h); }
static void *so_symbol(so_module *m, const char *n) {
  if (!strcmp(n, "mono_image_get_guid")) return guid;
  if (!strcmp(n, "mono_get_method")) return get_method;
  if (!strcmp(n, "mono_method_get_header")) return get_header;
  if (!strcmp(n, "mono_method_header_get_code")) return get_code;
  if (!strcmp(n, "mono_metadata_free_mh")) return free_header;
  assert(0); return NULL;
}
'''
harness += fn
harness += r'''
int main(int argc, char **argv) {
  mode = atoi(argv[1]);
  unsigned count = sizeof(race_warmup_methods) / sizeof(*race_warmup_methods);
  race_jit_warmup();
  if (mode == 1 || mode == 2) assert(calls == 0 && lookups == 0);
  else if (mode == 3) assert(calls == 0 && lookups == count);
  else assert(calls == count && lookups == count);
  unsigned before = calls;
  race_jit_warmup();
  assert(calls == before); // no recompiling on a second scene load
  if (mode == 2) { // reserve recovered: preparation can resume on next load
    mode = 0; race_jit_warmup(); assert(calls == count);
  } else if (mode == 0) {
    domain_id = 2; race_jit_warmup(); assert(calls == 2 * count);
  }
  return 0;
}
'''
with tempfile.TemporaryDirectory() as d:
    p = Path(d)
    (p / 'test.c').write_text(harness)
    subprocess.run(['cc', '-fsanitize=address,undefined', '-I', str(root / 'src'), str(p / 'test.c'), '-o', str(p / 'test')], check=True)
    for mode in range(5):
        subprocess.run([str(p / 'test'), str(mode)], check=True)
print('PASS: real JIT preload: MVID guard, memory reserve/resume, IL-size guard, compile failures, once per domain')

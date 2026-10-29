#!/usr/bin/env python3
"""The tracing notifier must chain Mono's callback exactly once, without rehooking."""
from pathlib import Path
import subprocess
import tempfile
src = (Path(__file__).resolve().parents[1]/'src/mono_probe.c').read_text()
a = src.index('static void (*original_gc_event)(int);')
b = src.index('static int probe_init(void)',a)
harness = r'''
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#include <stdio.h>
typedef struct { int id; } so_module;
static so_module mono;
static int valid, traced, called, installed_logs;
static void original(int event) { assert(event == 5 && traced == called + 1); called++; }
static void (*notify)(int) = original;
static void *so_symbol(so_module *m,const char *name) { return &notify; }
static so_module *so_module_at(uintptr_t p) { return valid && p == (uintptr_t)original ? &mono : NULL; }
static uint64_t sceKernelGetProcessTimeWide(void) { return 123; }
static void gc_policy_event(int event) {assert(event==5);}
static void gc_trace_event(int event, uint64_t now) { assert(event == 5 && now == 123); traced++; }
#define L_INFO(...) (installed_logs++)
'''
harness += src[a:b]
harness += r'''
int main(void) {
  install_gc_trace(&mono); assert(notify == original && installed_logs == 0);
  valid = 1; install_gc_trace(&mono); assert(notify != original && installed_logs == 1);
  notify(5); assert(traced == 1 && called == 1);
  install_gc_trace(&mono); notify(5);
  assert(traced == 2 && called == 2 && installed_logs == 1);
  puts("PASS: unknown notifier untouched, Mono callback preserved exactly once, no self-hook");
}
'''
with tempfile.TemporaryDirectory() as d:
    p = Path(d)
    (p/'test.c').write_text(harness)
    subprocess.run(['cc','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)

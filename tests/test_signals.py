#!/usr/bin/env python3
"""Regression: a GC suspend handler must see a queued restart in its nested wait."""
from pathlib import Path
import subprocess
import tempfile
src = (Path(__file__).resolve().parents[1] / 'src/bridges/signal_bridge.c').read_text()
a = src.index('static int deliver_pending(uint32_t mask) {')
b = src.index('\nint signal_pending_self', a)
harness = r'''
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
typedef struct { uint32_t pending, active; } sig_slot;
static sig_slot slot;
static int total_pending, sig_lock;
static int order[16], norder, nested_resume;
static int deliver_pending(uint32_t mask);
static void sceKernelLockLwMutex(int *p, int n, void *t) {}
static void sceKernelUnlockLwMutex(int *p, int n) {}
static uint32_t self_id(void) { return 1; }
static sig_slot *slot_for(uint32_t self, int create) { return &slot; }
static void deliver_now(int sig) {
  order[norder++] = sig;
  if (sig == 30) nested_resume = deliver_pending(1u << 30);
}
'''
harness += src[a:b]
harness += r'''
int main(void) {
  slot.pending = (1u << 30) | (1u << 24); total_pending = 2;
  assert(deliver_pending(0) == 1);
  assert(nested_resume == 1);
  assert(norder == 2 && order[0] == 30 && order[1] == 24);
  assert(total_pending == 0 && slot.pending == 0 && slot.active == 0);
  slot.pending = 1u << 24; total_pending = 1;
  assert(deliver_pending(1u << 24) == 0 && total_pending == 1);
  slot.active = 1u << 24;
  assert(deliver_pending(0) == 0 && total_pending == 1);
  slot.active = 0;
  assert(deliver_pending(0) == 1 && total_pending == 0);
  puts("PASS: nested GC restart, blocked signals, no reentrant delivery");
}
'''
with tempfile.TemporaryDirectory() as d:
    p = Path(d)
    (p / 'test.c').write_text(harness)
    subprocess.run(['cc', '-fsanitize=address,undefined', str(p / 'test.c'), '-o', str(p / 'test')], check=True)
    subprocess.run([str(p / 'test')], check=True)

#!/usr/bin/env python3
"""Exercise the actual signal event wait with deterministic arrival races."""
from pathlib import Path
import subprocess
import tempfile
src = (Path(__file__).resolve().parents[1] / 'src/bridges/signal_bridge.c').read_text()
a = src.index('int sigsuspend_bridge(')
b = src.index('// --- CPU exception', a)
harness = r'''
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
typedef int SceUID;
typedef unsigned SceUInt;
typedef struct { int wake; } sig_slot;
static sig_slot slot;
static int sig_lock, err, waits, delays, mode;
static uint32_t pending;
#define WAIT_BEGIN(a,b) ((void)0)
static void thread_wait_end(void) {}
static int *__errno_bionic(void) { return &err; }
static int self_id(void) { return 1; }
static sig_slot *slot_for(int id, int create) { return &slot; }
static void sceKernelLockLwMutex(int *p, int n, void *t) {}
static void sceKernelUnlockLwMutex(int *p, int n) {}
static int deliver_pending(uint32_t mask) {
  uint32_t ready = pending & ~mask;
  if (!ready) return 0;
  pending &= ~(1u << __builtin_ctz(ready)); return 1;
}
static int sceKernelWaitSema(int id, int n, unsigned *timeout) {
  assert(*timeout == 5000); waits++;
  // An arrival between checking pending and entering the kernel wait.
  pending |= (mode == 2 && waits == 1) ? 1u << 30 : 1u << 24;
  return 0;
}
static void sceKernelDelayThread(unsigned timeout) {
  assert(timeout == 5000); delays++; pending |= 1u << 24;
}
'''
harness += src[a:b]
harness += r'''
int main(void) {
  uint32_t mask = ~(1u << 23); // Linux bit n-1: allow only SIGXCPU (24).
  slot.wake = 7; mode = 1;
  assert(sigsuspend_bridge(&mask) == -1 && err == 4);
  assert(waits == 1 && delays == 0 && pending == 0);
  waits = 0; mode = 2;
  assert(sigsuspend_bridge(&mask) == -1);
  assert(waits == 2 && pending == (1u << 30));
  pending = 0; slot.wake = -1; waits = 0;
  assert(sigsuspend_bridge(&mask) == -1);
  assert(waits == 0 && delays == 1 && pending == 0);
  puts("PASS: event arrival before wait, masked signals, allocation failure fallback");
}
'''
with tempfile.TemporaryDirectory() as d:
    p = Path(d)
    (p / 'test.c').write_text(harness)
    subprocess.run(['cc', '-fsanitize=address,undefined', str(p/'test.c'), '-o', str(p/'test')], check=True)
    subprocess.run([str(p/'test')], check=True)

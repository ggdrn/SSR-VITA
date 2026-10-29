#!/usr/bin/env python3
"""Production GC tracer: phase accounting, overwritten ring and concurrent reads."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
harness = r'''
#include "gc_trace.h"
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
static unsigned done;
static void collection(unsigned unit) {
  static uint64_t now = 0x100000064ULL;
  gc_trace_event(0, now);
  gc_trace_event(6, now += unit);
  gc_trace_event(7, now += unit);
  gc_trace_event(1, now += unit);
  gc_trace_event(2, now += 2*unit);
  gc_trace_event(3, now += unit);
  gc_trace_event(4, now += unit);
  gc_trace_event(8, now += unit);
  gc_trace_event(9, now += unit);
  gc_trace_event(5, now += unit);
}
static void check(gc_trace_sample s) {
  assert(s.total_us > 0 && s.total_us % 10 == 0);
  assert(s.stop_us == s.total_us/10);
  assert(s.mark_us == s.total_us/5);
  assert(s.reclaim_us == s.total_us/10);
  assert(s.restart_us == s.total_us/10);
}
static void *writer(void *arg) {
  for (unsigned i = 0; i < 100000; i++) collection(1 + i % 97);
  __atomic_store_n(&done, 1, __ATOMIC_RELEASE); return NULL;
}
int main(void) {
  gc_trace_sample out[16];
  gc_trace_event(5, 10); // incomplete cycle must not appear
  gc_trace_event(-1, 10); gc_trace_event(10, 10);
  assert(gc_trace_read(out) == 0);
  collection(1); assert(gc_trace_read(out) == 1); check(out[0]);
  assert(out[0].start_hi==1 && out[0].start_lo==100);
  assert(gc_trace_read(out) == 0);
  for (int i = 0; i < 40; i++) collection(3);
  assert(gc_trace_read(out) == 16);
  for (int i = 0; i < 16; i++) check(out[i]);
  pthread_t t; pthread_create(&t, NULL, writer, NULL);
  unsigned last = 0;
  do {
    unsigned n = gc_trace_read(out);
    assert(n <= 16);
    for (unsigned i = 0; i < n; i++) {
      check(out[i]); assert(out[i].sequence > last); last = out[i].sequence;
    }
  } while (!__atomic_load_n(&done, __ATOMIC_ACQUIRE));
  pthread_join(t, NULL);
  puts("PASS: GC phase accounting, incomplete cycle, bounded ring, concurrent publication");
}
'''
with tempfile.TemporaryDirectory() as d:
    p = Path(d)
    (p/'test.c').write_text(harness)
    subprocess.run(['cc','-O2','-pthread','-fsanitize=address,undefined','-I',str(root/'src/util'),str(p/'test.c'),str(root/'src/util/gc_trace.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)

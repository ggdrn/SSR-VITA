// Per-class managed allocation counters, fed by Mono's profiler callback on
// whichever thread allocates. Lock-free: classes claim a slot with one CAS and
// counters use relaxed atomics; a window is read and cleared by the main
// thread, so a few increments racing with the clear may move to the next one.
#include "alloc_trace.h"
#include <string.h>

#define SLOTS 1024
static alloc_trace_entry table[SLOTS];
static uint32_t total_count, total_bytes, overflow;
static uint32_t (*object_size)(void *);

void alloc_trace_set_sizer(uint32_t (*sizer)(void *object)) { object_size = sizer; }

void alloc_trace_record(void *profiler, void *object, void *klass) {
  (void)profiler;
  uint32_t size = object_size && object ? object_size(object) : 0;
  __atomic_add_fetch(&total_count, 1, __ATOMIC_RELAXED);
  __atomic_add_fetch(&total_bytes, size, __ATOMIC_RELAXED);
  uint32_t h = ((uintptr_t)klass >> 3) * 2654435761u;
  for (unsigned probe = 0; probe < 16; probe++) {
    alloc_trace_entry *e = &table[(h + probe) & (SLOTS - 1)];
    void *owner = __atomic_load_n(&e->klass, __ATOMIC_ACQUIRE);
    if (!owner) {
      void *expected = NULL;
      if (__atomic_compare_exchange_n(&e->klass, &expected, klass, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        owner = klass;
      else
        owner = expected;
    }
    if (owner == klass) {
      __atomic_add_fetch(&e->count, 1, __ATOMIC_RELAXED);
      __atomic_add_fetch(&e->bytes, size, __ATOMIC_RELAXED);
      return;
    }
  }
  __atomic_add_fetch(&overflow, 1, __ATOMIC_RELAXED);
}

unsigned alloc_trace_take(alloc_trace_entry *out, unsigned max, uint32_t *count, uint32_t *bytes, uint32_t *lost) {
  unsigned n = 0;
  for (unsigned i = 0; i < SLOTS; i++) {
    alloc_trace_entry e;
    e.klass = __atomic_load_n(&table[i].klass, __ATOMIC_ACQUIRE);
    if (!e.klass) continue;
    e.count = __atomic_exchange_n(&table[i].count, 0, __ATOMIC_RELAXED);
    e.bytes = __atomic_exchange_n(&table[i].bytes, 0, __ATOMIC_RELAXED);
    if (!e.count) continue;
    // Insertion into a small sorted array (descending bytes).
    unsigned pos = n < max ? n : max;
    while (pos > 0 && out[pos - 1].bytes < e.bytes) {
      if (pos < max) out[pos] = out[pos - 1];
      pos--;
    }
    if (pos < max) {
      out[pos] = e;
      if (n < max) n++;
    }
  }
  *count = __atomic_exchange_n(&total_count, 0, __ATOMIC_RELAXED);
  *bytes = __atomic_exchange_n(&total_bytes, 0, __ATOMIC_RELAXED);
  *lost = __atomic_exchange_n(&overflow, 0, __ATOMIC_RELAXED);
  return n;
}

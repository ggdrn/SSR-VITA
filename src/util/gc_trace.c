#include "gc_trace.h"
#include <limits.h>
#include <string.h>

// Boehm event numbering, also verified against libmono's start/end handler:
// https://github.com/ivmai/bdwgc/blob/master/include/gc/gc.h
// 0/5 collection, 6/7 stop, 1/2 mark, 3/4 reclaim, 8/9 restart.
// Events are serialized by the collector's allocation lock. A reader must
// never wait for that lock: it may be the thread the collector must suspend.
static struct {
  uint32_t sequence, start_lo, start_hi, total_us, stop_us, mark_us, reclaim_us, restart_us;
} ring[16];
static uint32_t published, consumed;
static uint64_t starts[10];
static unsigned seen;
static gc_trace_sample current;

static uint32_t elapsed(uint64_t now, uint64_t start) {
  uint64_t delta = now >= start ? now - start : 0;
  return delta > UINT32_MAX ? UINT32_MAX : (uint32_t)delta;
}

void gc_trace_event(int event, uint64_t now_us) {
  if (event < 0 || event > 9) return;
  if (event == 0) {
    memset(&current, 0, sizeof(current));
    seen = 0;
    current.start_lo = (uint32_t)now_us;
    current.start_hi = (uint32_t)(now_us >> 32);
  }
  starts[event] = now_us;
  seen |= 1u << event;
  if (event == 7 && (seen & (1u << 6))) current.stop_us += elapsed(now_us, starts[6]);
  if (event == 2 && (seen & (1u << 1))) current.mark_us += elapsed(now_us, starts[1]);
  if (event == 4 && (seen & (1u << 3))) current.reclaim_us += elapsed(now_us, starts[3]);
  if (event == 9 && (seen & (1u << 8))) current.restart_us += elapsed(now_us, starts[8]);
  if (event != 5 || !(seen & 1)) return;
  current.total_us = elapsed(now_us, starts[0]);
  uint32_t seq = __atomic_load_n(&published, __ATOMIC_RELAXED) + 1;
  unsigned i = seq % 16;
  // Invalidate before overwrite, then publish a complete record. Scalar
  // atomics keep the reader safe even if a later collection reuses a slot.
  __atomic_store_n(&ring[i].sequence, 0, __ATOMIC_SEQ_CST);
#define STORE(f) __atomic_store_n(&ring[i].f, current.f, __ATOMIC_RELAXED)
  STORE(start_lo); STORE(start_hi); STORE(total_us); STORE(stop_us); STORE(mark_us); STORE(reclaim_us); STORE(restart_us);
#undef STORE
  __atomic_store_n(&ring[i].sequence, seq, __ATOMIC_RELEASE);
  __atomic_store_n(&published, seq, __ATOMIC_RELEASE);
  seen = 0;
}

unsigned gc_trace_read(gc_trace_sample out[16]) {
  uint32_t end = __atomic_load_n(&published, __ATOMIC_ACQUIRE);
  uint32_t first = consumed + 1;
  if (end - consumed > 16) first = end - 15;
  unsigned count = 0;
  for (uint32_t seq = first; seq != end + 1; seq++) {
    unsigned i = seq % 16;
    if (__atomic_load_n(&ring[i].sequence, __ATOMIC_ACQUIRE) != seq) continue;
    gc_trace_sample sample = {.sequence = seq};
#define LOAD(f) sample.f = __atomic_load_n(&ring[i].f, __ATOMIC_RELAXED)
    LOAD(start_lo); LOAD(start_hi); LOAD(total_us); LOAD(stop_us); LOAD(mark_us); LOAD(reclaim_us); LOAD(restart_us);
#undef LOAD
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (__atomic_load_n(&ring[i].sequence, __ATOMIC_ACQUIRE) == seq) out[count++] = sample;
  }
  consumed = end;
  return count;
}

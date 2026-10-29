#ifndef SRR_GC_TRACE_H
#define SRR_GC_TRACE_H
#include <stdint.h>

typedef struct {
  uint32_t sequence, start_lo, start_hi, total_us, stop_us, mark_us, reclaim_us, restart_us;
} gc_trace_sample;

// Collector-side: no allocation, locks, logging, or managed calls.
void gc_trace_event(int event, uint64_t now_us);
// Main-thread reader: bounded, nonblocking; may skip samples overwritten by
// more than 16 collections between frames. Returns the number copied.
unsigned gc_trace_read(gc_trace_sample out[16]);
#endif

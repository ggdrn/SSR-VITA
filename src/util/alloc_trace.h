#ifndef SRR_ALLOC_TRACE_H
#define SRR_ALLOC_TRACE_H
#include <stdint.h>
typedef struct {
  void *klass;
  uint32_t count, bytes;
} alloc_trace_entry;
// Mono profiler allocation callback: (profiler, object, class).
void alloc_trace_record(void *profiler, void *object, void *klass);
void alloc_trace_set_sizer(uint32_t (*sizer)(void *object));
// Copies the busiest classes by bytes (at most max) and clears the window.
// Returns how many were copied; totals cover every class in the window.
unsigned alloc_trace_take(alloc_trace_entry *out, unsigned max, uint32_t *total_count, uint32_t *total_bytes,
                          uint32_t *overflow);
#endif

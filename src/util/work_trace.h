#ifndef SRR_WORK_TRACE_H
#define SRR_WORK_TRACE_H
#include <stdint.h>
enum { WORK_IO, WORK_WAIT, WORK_CLONE, WORK_RESOURCE, WORK_IMAGE, WORK_UNLOAD, WORK_MANAGED, WORK_META, WORK_CACHE, WORK_KINDS };
typedef struct { uint64_t us, bytes, longest_us; uintptr_t caller; unsigned calls; } work_counter;
typedef struct { work_counter counter[WORK_KINDS]; } work_sample;
// Only the render thread records; workers never modify its counters.
void work_trace_frame_begin(void);
uint64_t work_trace_begin(void);
void work_trace_end(unsigned kind, uint64_t start, uint64_t bytes, uintptr_t caller);
void work_trace_take(work_sample *out);
#endif

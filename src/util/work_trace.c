#include "work_trace.h"
#include <string.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
static int owner;
static work_sample sample;
void work_trace_frame_begin(void) {
  __atomic_store_n(&owner, sceKernelGetThreadId(), __ATOMIC_RELEASE);
  memset(&sample, 0, sizeof(sample));
}
uint64_t work_trace_begin(void) {
  int uid = __atomic_load_n(&owner, __ATOMIC_ACQUIRE);
  return uid && uid == sceKernelGetThreadId() ? sceKernelGetProcessTimeWide() : 0;
}
void work_trace_end(unsigned kind, uint64_t start, uint64_t bytes, uintptr_t caller) {
  if (!start || kind >= WORK_KINDS) return;
  uint64_t now = sceKernelGetProcessTimeWide();
  uint64_t us = now >= start ? now - start : 0;
  work_counter *c = &sample.counter[kind];
  c->us += us; c->bytes += bytes; c->calls++;
  if (us > c->longest_us) { c->longest_us = us; c->caller = caller; }
}
void work_trace_take(work_sample *out) { *out = sample; }

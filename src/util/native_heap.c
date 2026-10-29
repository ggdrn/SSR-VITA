#include "native_heap.h"
#include <malloc.h>
#include <stddef.h>
#include <unistd.h>
#include <psp2/kernel/sysmem.h>
extern char *__malloc_sbrk_base;
extern unsigned int _get_vita_heap_size(void);

native_heap_sample native_heap_available(void) {
  struct mallinfo mi = mallinfo();
  native_heap_sample s = {0};
  if (mi.fordblks > 0) s.chunks_free = (unsigned)mi.fordblks;
  s.total_free = s.chunks_free;
  uintptr_t start = (uintptr_t)__malloc_sbrk_base;
  if (!start || start == (uintptr_t)-1) return s;
  SceKernelMemBlockInfo info = {.size = sizeof(info)};
  // VitaSDK reserves the complete native heap up front; mallinfo reports
  // only the portion already handed to dlmalloc by sbrk. Count the rest only
  // after confirming that this is that exact reserved kernel block.
  if (sceKernelGetMemBlockInfoByAddr((void *)start, &info) < 0 ||
      !info.mappedSize || info.mappedSize != _get_vita_heap_size()) return s;
  uintptr_t base = (uintptr_t)info.mappedBase;
  uintptr_t current = (uintptr_t)sbrk(0);
  if (base > UINTPTR_MAX - info.mappedSize) return s;
  uintptr_t end = base + info.mappedSize;
  if (start < base || start >= end || current < start || current >= end) return s;
  // Keep a page outside the reported growth budget; never count graphics
  // pools or unreserved system RAM as native allocation headroom.
  uint64_t tail = end - current;
  if (tail > 4096) s.unused_reserve = tail - 4096;
  s.total_free += s.unused_reserve;
  return s;
}

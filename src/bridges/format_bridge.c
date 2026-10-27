// VitaSDK's newlib is built without C99 printf/scanf formats: "%zu" prints
// "zu" without consuming its argument, shifting every later argument (Unity
// crashed in strlen() this way). Bionic supports them, so guest format strings
// are rewritten before reaching newlib: z/t are dropped (32-bit on ARM) and j
// becomes ll. Also: allocation failures are logged with their caller, and
// guest free()/realloc() calls are checked against the newlib heap so a bad
// pointer is logged and ignored instead of corrupting the allocator.

#define _GNU_SOURCE  // vasprintf
#include "bridges.h"
#include "../loader/so_util.h"
#include "../util/log.h"

#include <malloc.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *fmt_c99(const char *fmt, char *buf, size_t size) {
  if (!fmt || !strpbrk(fmt, "ztj"))
    return fmt;
  size_t o = 0;
  for (const char *p = fmt; *p && o + 3 < size; p++) {
    buf[o++] = *p;
    if (*p != '%')
      continue;
    if (p[1] == '%') {
      buf[o++] = *++p;
      continue;
    }
    while (p[1] && strchr("-+ #0123456789.*'", p[1]))
      buf[o++] = *++p;
    while (p[1] && strchr("hlLqztj", p[1])) {
      char m = *++p;
      if (m == 'j') {
        buf[o++] = 'l';
        buf[o++] = 'l';
      } else if (m != 'z' && m != 't') {
        buf[o++] = m;
      }
    }
  }
  buf[o] = 0;
  return buf;
}

#define FIX(fmt) char fixbuf_[1024]; fmt = fmt_c99(fmt, fixbuf_, sizeof(fixbuf_))

int vsnprintf_bridge(char *s, size_t n, const char *fmt, va_list ap) {
  FIX(fmt);
  return vsnprintf(s, n, fmt, ap);
}

int snprintf_bridge(char *s, size_t n, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int r = vsnprintf_bridge(s, n, fmt, ap);
  va_end(ap);
  return r;
}

int vsprintf_bridge(char *s, const char *fmt, va_list ap) {
  FIX(fmt);
  return vsprintf(s, fmt, ap);
}

int sprintf_bridge(char *s, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int r = vsprintf_bridge(s, fmt, ap);
  va_end(ap);
  return r;
}

int vasprintf_bridge(char **out, const char *fmt, va_list ap) {
  FIX(fmt);
  return vasprintf(out, fmt, ap);
}

int sscanf_bridge(const char *s, const char *fmt, ...) {
  FIX(fmt);
  va_list ap;
  va_start(ap, fmt);
  int r = vsscanf(s, fmt, ap);
  va_end(ap);
  return r;
}

// --- allocation failures ----------------------------------------------------

static void alloc_failed(const char *what, size_t size, size_t align, void *caller) {
  struct mallinfo mi = mallinfo();
  L_ERROR("%s(%u, align %u) failed from %p: heap arena %u KB, in use %u KB, free %u KB", what, size, align, caller,
          mi.arena / 1024, mi.uordblks / 1024, mi.fordblks / 1024);
}

// newlib (dlmalloc) chunk layout: the size word sits 4 bytes before the user
// pointer with PREV_INUSE in bit 0; the next chunk's PREV_INUSE bit tells
// whether this chunk is currently allocated.
extern char *__malloc_sbrk_base;
extern void *sbrk(ptrdiff_t);

static const char *bad_chunk(void *ptr) {
  uintptr_t p = (uintptr_t)ptr, lo = (uintptr_t)__malloc_sbrk_base, hi = (uintptr_t)sbrk(0);
  if (p & 7)
    return "misaligned";
  if (lo == (uintptr_t)-1 || p < lo + 8 || p >= hi)
    return "outside the heap";
  uint32_t size = ((uint32_t *)p)[-1] & ~7u;
  uintptr_t next = p - 8 + size;
  if (size < 16 || next + 8 > hi)
    return "corrupt chunk size";
  if (!(((uint32_t *)next)[1] & 1))
    return "already free";
  return NULL;
}

static int bad_free_reports;

static int reject_free(const char *what, void *ptr, void *caller) {
  const char *why = bad_chunk(ptr);
  if (!why)
    return 0;
  if (bad_free_reports < 32) {
    bad_free_reports++;
    so_module *m = so_module_at((uintptr_t)caller);
    if (m)
      L_ERROR("%s(%p) ignored: %s, from %s+0x%X", what, ptr, why, m->name, (uintptr_t)caller - m->base);
    else
      L_ERROR("%s(%p) ignored: %s, from %p", what, ptr, why, caller);
  }
  return 1;
}

void free_bridge(void *ptr) {
  if (ptr && reject_free("free", ptr, __builtin_return_address(0)))
    return;
  free(ptr);
}

void *malloc_bridge(size_t size) {
  void *p = malloc(size);
  if (!p && size)
    alloc_failed("malloc", size, 0, __builtin_return_address(0));
  return p;
}

void *calloc_bridge(size_t n, size_t size) {
  void *p = calloc(n, size);
  if (!p && n && size)
    alloc_failed("calloc", n * size, 0, __builtin_return_address(0));
  return p;
}

void *realloc_bridge(void *old, size_t size) {
  // The old block's contents can't be trusted, so start a fresh one.
  if (old && reject_free("realloc", old, __builtin_return_address(0)))
    old = NULL;
  void *p = realloc(old, size);
  if (!p && size)
    alloc_failed("realloc", size, 0, __builtin_return_address(0));
  return p;
}

void *memalign_bridge(size_t align, size_t size) {
  void *p = memalign(align, size);
  if (!p && size)
    alloc_failed("memalign", size, align, __builtin_return_address(0));
  return p;
}

void log_heap_usage(const char *when) {
  struct mallinfo mi = mallinfo();
  L_INFO("heap %s: arena %u KB, in use %u KB, free %u KB", when, mi.arena / 1024, mi.uordblks / 1024,
         mi.fordblks / 1024);
}

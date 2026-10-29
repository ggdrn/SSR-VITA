// Game-specific binary patches, applied after linking and before the
// libraries' constructors run. Each patch checks the original bytes first so
// a different APK build fails loudly instead of being corrupted.

#include "loader/so_util.h"
#include "util/log.h"
#include "util/gc_policy.h"

#include <string.h>

#include <kubridge.h>
#include <stdint.h>

static int expect(so_module *m, uintptr_t off, uint32_t word) {
  uint32_t have = *(uint32_t *)(m->base + off);
  if (have != word) {
    L_ERROR("patch: %s+0x%X is 0x%08X, expected 0x%08X (different APK build?)", m->name, off, have, word);
    return 0;
  }
  return 1;
}

// mono_arch_flush_icache(code, size) issues the Linux cacheflush syscall
// (svc 0 with r7 = 0xF0002) inline, which crashes on the Vita. The JIT calls it
// after emitting every method.
static void mono_flush_icache(uint8_t *code, int size) { kuKernelFlushCaches(code, size); }

#define MONO_FLUSH_ICACHE 0x113104

// Boehm 7.1 min_words_allocd() and the GOT slots it reads. The whole body is
// compared; any difference keeps the collector's original policy.
#define MONO_MIN_WORDS 0x2bcb30
#define GOT_ARRAYS 0x3ad9c8
#define GOT_INCREMENTAL 0x3ad9e0
#define GOT_TIME_LIMIT 0x3ad9e4
#define GOT_ROOT_SIZE 0x3ada0c
#define GOT_DIVISOR 0x3ada10
static const uint32_t min_words_body[] = {0xe92d4810,0xe28db008,0xe24dd00c,0xe59f30e8,0xe08f3003,0xe3024710,0xe3540000,0xaa000000,0xe2644000,0xe1a02084,0xe1a01002,0xe59f20cc,0xe7932002,0xe5922000,0xe0812002,0xe50b2010,0xe59f20bc,0xe7932002,0xe5921000,0xe59f20b0,0xe7932002,0xe5922014,0xe0621001,0xe59f20a0,0xe7932002,0xe5922014,0xe1a02122,0xe0811002,0xe51b2010,0xe0812002,0xe1a02122,0xe50b2014,0xe59f2080,0xe7932002,0xe5922000,0xe3520000,0x0a00000f,0xe59f2070,0xe7932002,0xe5921000,0xe304223f,0xe340200f,0xe1510002,0x0a000008,0xe59f2058,0xe7933002,0xe5933000,0xe1a03083,0xe51b0014,0xe1a01003,0xeb0062dc,0xe1a03000,0xea000006,0xe59f2034,0xe7933002,0xe5933000,0xe51b0014,0xe1a01003,0xeb0062d4,0xe1a03000,0xe1a00003,0xe24bd008,0xe8bd8810,0x000f10bc,0xfffffe08,0xfffffdc4,0xfffffddc,0xfffffde0,0xfffffe0c};

static uint32_t *mono_data(so_module *m, uintptr_t got) {
  uintptr_t p = *(uintptr_t *)(m->base + got);
  if (p < m->base || p + 0x40 > m->end || (p & 3)) return NULL;
  return (uint32_t *)p;
}

// Runs before libmono's constructors, so no collector thread can be inside
// the function while it is replaced.
static void install_gc_pacing(so_module *mono) {
  if (mono->end < mono->base + GOT_DIVISOR + 4 ||
      memcmp((void *)(mono->base + MONO_MIN_WORDS), min_words_body, sizeof(min_words_body))) {
    L_WARN("gc: min_words_allocd differs; collection pacing disabled");
    return;
  }
  uint32_t *arrays = mono_data(mono, GOT_ARRAYS), *root = mono_data(mono, GOT_ROOT_SIZE);
  uint32_t *incremental = mono_data(mono, GOT_INCREMENTAL), *limit = mono_data(mono, GOT_TIME_LIMIT);
  uint32_t *divisor = mono_data(mono, GOT_DIVISOR);
  if (!arrays || !root || !incremental || !limit || !divisor || !gc_policy_attach(divisor)) {
    L_WARN("gc: collector globals not where expected; collection pacing disabled");
    return;
  }
  gc_policy_attach_pacing(arrays, root, incremental, limit);
  hook_addr(mono->base + MONO_MIN_WORDS, (uintptr_t)gc_policy_min_words);
  L_INFO("patch: libmono min_words_allocd -> paced budget (explicit GC unchanged)");
}

void apply_patches(void) {
  so_module *mono = so_find_module("libmono.so");
  if (mono && expect(mono, MONO_FLUSH_ICACHE, 0xE92D0890) && expect(mono, MONO_FLUSH_ICACHE + 0x44, 0xEF000000)) {
    hook_addr(mono->base + MONO_FLUSH_ICACHE, (uintptr_t)mono_flush_icache);
    L_INFO("patch: libmono mono_arch_flush_icache -> kuKernelFlushCaches");
  }
  if (mono) install_gc_pacing(mono);
}

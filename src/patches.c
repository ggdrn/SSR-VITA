// Game-specific binary patches, applied after linking and before the
// libraries' constructors run. Each patch checks the original bytes first so
// a different APK build fails loudly instead of being corrupted.

#include "loader/so_util.h"
#include "util/log.h"

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

void apply_patches(void) {
  so_module *mono = so_find_module("libmono.so");
  if (mono && expect(mono, MONO_FLUSH_ICACHE, 0xE92D0890) && expect(mono, MONO_FLUSH_ICACHE + 0x44, 0xEF000000)) {
    hook_addr(mono->base + MONO_FLUSH_ICACHE, (uintptr_t)mono_flush_icache);
    L_INFO("patch: libmono mono_arch_flush_icache -> kuKernelFlushCaches");
  }
}

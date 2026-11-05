// Accelerate the original decrypting Rijndael-256 CBC loop, retaining all
// higher-level padding/final-block/stream handling and every other cipher.
#include "rijndael_cbc.h"
#include "rijndael256.h"
#include "rijndael256_il.h"
#include "rijndael_cbc_il.h"
#include "../loader/so_util.h"
#include "../config.h"
#include "log.h"
#include <kubridge.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>

typedef void (*cbc_fn)(void *, void *, void *);
static cbc_fn original;
static void *crypto_class;
static void *(*object_class)(void *);
static uint32_t offsets[7]; // encrypt, BlockSizeByte, temp, temp2, expandedKey, Nb, Nr
static unsigned fast_calls, fallback_calls;
static int attempted;
extern void *mmap_bridge(void *, size_t, int, int, int, off_t);

static void decrypt_cbc(void *self, void *input, void *output) {
  if (self && object_class(self) == crypto_class) {
    char *p = self;
    if (!*(uint8_t *)(p + offsets[0]) && *(int32_t *)(p + offsets[1]) == 32 &&
        *(int32_t *)(p + offsets[5]) == 8 && *(int32_t *)(p + offsets[6]) == 14 &&
        rijndael256_try_cbc(self, input, output, *(void **)(p + offsets[2]),
                           *(void **)(p + offsets[3]), *(void **)(p + offsets[4]))) {
      __atomic_fetch_add(&fast_calls, 1, __ATOMIC_RELAXED);
      return;
    }
  }
  __atomic_fetch_add(&fallback_calls, 1, __ATOMIC_RELAXED);
  original(self, input, output);
}
static int safe_prologue(uint32_t word) {
  return ((word & 0xffff8000u) == 0xe92d0000u) ||
         ((word & 0xfffff000u) == 0xe24dd000u) || word == 0xe1a0b00du || word == 0xe1a0c00du ||
         ((word & 0xffff0000u) == 0xe58d0000u && ((word >> 12) & 15) != 15);
}
void rijndael_cbc_install(void) {
  if (attempted) return;
  attempted = 1;
  if (!rijndael256_ready()) return;
  FILE *marker = fopen(DATA_PATH "/original_rijndael_cbc", "rb");
  if (marker) { fclose(marker); L_INFO("stage: original CBC requested"); return; }
  so_module *mono = so_find_module("libmono.so");
  if (!mono) return;
#define API(type, name) ((type)so_symbol(mono, name))
  void *(*corlib)(void) = API(void *(*)(void), "mono_get_corlib");
  const char *(*guid)(void *) = API(const char *(*)(void *), "mono_image_get_guid");
  void *(*klass)(void *, const char *, const char *) = API(void *(*)(void *, const char *, const char *), "mono_class_from_name");
  void *(*parent)(void *) = API(void *(*)(void *), "mono_class_get_parent");
  void *(*method)(void *, const char *, int) = API(void *(*)(void *, const char *, int), "mono_class_get_method_from_name");
  void *(*header)(void *) = API(void *(*)(void *), "mono_method_get_header");
  const uint8_t *(*code)(void *, uint32_t *, uint32_t *) = API(const uint8_t *(*)(void *, uint32_t *, uint32_t *), "mono_method_header_get_code");
  void (*free_header)(void *) = API(void (*)(void *), "mono_metadata_free_mh");
  void *(*field)(void *, const char *) = API(void *(*)(void *, const char *), "mono_class_get_field_from_name");
  uint32_t (*offset)(void *) = API(uint32_t (*)(void *), "mono_field_get_offset");
  void *(*compile)(void *) = API(void *(*)(void *), "mono_compile_method");
  object_class = API(void *(*)(void *), "mono_object_get_class");
  if (!corlib || !guid || !klass || !parent || !method || !header || !code || !free_header ||
      !field || !offset || !compile || !object_class) return;
  void *image = corlib();
  const char *id = image ? guid(image) : NULL;
  if (!id || strcasecmp(id, RIJNDAEL_MVID)) return;
  void *crypto = klass(image, "System.Security.Cryptography", "RijndaelTransform");
  void *base = klass(image, "System.Security.Cryptography", "SymmetricTransform");
  if (!crypto || !base || parent(crypto) != base) return;
  void *cbc = method(base, "CBC", 2), *ecb = method(crypto, "ECB", 2);
  const struct { void *method; const uint8_t *il; uint32_t size; } targets[] = {
    {cbc, rijndael_cbc_il, sizeof(rijndael_cbc_il)}, {ecb, rijndael_ecb_il, sizeof(rijndael_ecb_il)}
  };
  for (unsigned i = 0; i < 2; i++) {
    void *mh = targets[i].method ? header(targets[i].method) : NULL;
    uint32_t size = 0, stack = 0;
    const uint8_t *il = mh ? code(mh, &size, &stack) : NULL;
    int valid = il && size == targets[i].size && !memcmp(il, targets[i].il, size);
    if (mh) free_header(mh);
    if (!valid) { L_WARN("stage: CBC/ECB IL differs; original CBC preserved"); return; }
  }
  const char *names[] = {"encrypt", "BlockSizeByte", "temp", "temp2", "expandedKey", "Nb", "Nr"};
  for (unsigned i = 0; i < 7; i++) {
    void *f = field(i < 4 ? base : crypto, names[i]);
    if (!f || !(offsets[i] = offset(f))) return;
  }
  uintptr_t address = (uintptr_t)compile(cbc);
  if (!address || (address & 3)) return;
  uint32_t first[2]; memcpy(first, (void *)address, sizeof(first));
  if (!safe_prologue(first[0]) || !safe_prologue(first[1])) {
    L_WARN("stage: CBC JIT prologue differs; original CBC preserved"); return;
  }
  void *trampoline = mmap_bridge(NULL, 4096, 7, 0x20, -1, 0);
  if (!trampoline || trampoline == (void *)-1) return;
  uint32_t instructions[] = {first[0], first[1], 0xe51ff004, (uint32_t)address + 8};
  memcpy(trampoline, instructions, sizeof(instructions));
  kuKernelFlushCaches(trampoline, sizeof(instructions));
  crypto_class = crypto;
  original = (cbc_fn)trampoline;
  hook_addr(address, (uintptr_t)decrypt_cbc);
  L_INFO("stage: native CBC installed for decrypting Rijndael-256 only (padding/streams and other ciphers preserved)");
#undef API
}
unsigned rijndael_cbc_fast_calls(void) { return __atomic_load_n(&fast_calls, __ATOMIC_RELAXED); }
unsigned rijndael_cbc_fallback_calls(void) { return __atomic_load_n(&fallback_calls, __ATOMIC_RELAXED); }

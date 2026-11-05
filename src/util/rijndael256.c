// Guarded block primitive shared with the optional native CBC path.
// Preserve Mono's key expansion, padding and stream APIs.
#include "rijndael256.h"
#include "rijndael256_block.h"
#include "rijndael256_il.h"
#include "../loader/so_util.h"
#include "../config.h"
#include "log.h"
#include <kubridge.h>
#include <stdio.h>
#include <strings.h>
#include <sys/types.h>

typedef void (*decrypt_fn)(void *, void *, void *, void *);
static decrypt_fn original;
static rijndael256_tables tables;
static void *crypto_class, *byte_array_class, *word_array_class;
static void *(*object_class)(void *);
// Supported 32-bit MonoArray: two object words, bounds pointer, max_length.
static uint32_t array_length(const void *array) {
  uint32_t n; memcpy(&n, (const char *)array + 12, sizeof(n)); return n;
}
static char *(*array_data)(void *, int, uintptr_t);
static unsigned fast_calls, fallback_calls;
static int attempted;
extern void *mmap_bridge(void *, size_t, int, int, int, off_t);

static void decrypt_block(void *self, void *input, void *output, void *keys) {
  if (self && input && output && keys && object_class(self) == crypto_class &&
      object_class(input) == byte_array_class && object_class(output) == byte_array_class &&
      object_class(keys) == word_array_class && array_length(input) >= 32 &&
      array_length(output) >= 32 && array_length(keys) >= 120) {
    rijndael256_block((uint8_t *)array_data(input, 1, 0), (uint8_t *)array_data(output, 1, 0),
                      (uint32_t *)array_data(keys, 4, 0), &tables);
    __atomic_fetch_add(&fast_calls, 1, __ATOMIC_RELAXED);
    return;
  }
  __atomic_fetch_add(&fallback_calls, 1, __ATOMIC_RELAXED);
  original(self, input, output, keys);
}
// Relocate only PC-independent ARM stack setup into the fallback trampoline.
static int safe_prologue(uint32_t word) {
  return ((word & 0xffff8000u) == 0xe92d0000u) ||
         ((word & 0xfffff000u) == 0xe24dd000u) || word == 0xe1a0b00du || word == 0xe1a0c00du ||
         ((word & 0xffff0000u) == 0xe58d0000u && ((word >> 12) & 15) != 15);
}
void rijndael256_install(void) {
  if (attempted) return;
  attempted = 1;
  FILE *marker = fopen(DATA_PATH "/original_rijndael256", "rb");
  if (marker) { fclose(marker); L_INFO("stage: original Rijndael256 requested"); return; }
  so_module *mono = so_find_module("libmono.so");
  if (!mono) return;
#define API(type, name) ((type)so_symbol(mono, name))
  void *(*corlib)(void) = API(void *(*)(void), "mono_get_corlib");
  const char *(*guid)(void *) = API(const char *(*)(void *), "mono_image_get_guid");
  void *(*klass)(void *, const char *, const char *) = API(void *(*)(void *, const char *, const char *), "mono_class_from_name");
  void *(*method)(void *, const char *, int) = API(void *(*)(void *, const char *, int), "mono_class_get_method_from_name");
  void *(*header)(void *) = API(void *(*)(void *), "mono_method_get_header");
  const uint8_t *(*code)(void *, uint32_t *, uint32_t *) = API(const uint8_t *(*)(void *, uint32_t *, uint32_t *), "mono_method_header_get_code");
  void (*free_header)(void *) = API(void (*)(void *), "mono_metadata_free_mh");
  void *(*field)(void *, const char *) = API(void *(*)(void *, const char *), "mono_class_get_field_from_name");
  void *(*domain)(void) = API(void *(*)(void), "mono_domain_get");
  void *(*vtable)(void *, void *) = API(void *(*)(void *, void *), "mono_class_vtable");
  void (*init)(void *) = API(void (*)(void *), "mono_runtime_class_init");
  void (*static_value)(void *, void *, void *) = API(void (*)(void *, void *, void *), "mono_field_static_get_value");
  void *(*compile)(void *) = API(void *(*)(void *), "mono_compile_method");
  object_class = API(void *(*)(void *), "mono_object_get_class");
  array_data = API(char *(*)(void *, int, uintptr_t), "mono_array_addr_with_size");
  if (!corlib || !guid || !klass || !method || !header || !code || !free_header || !field ||
      !domain || !vtable || !init || !static_value || !compile || !object_class || !array_data) {
    L_WARN("stage: Rijndael256 APIs unavailable; original decrypt preserved"); return;
  }
  void *image = corlib();
  const char *id = image ? guid(image) : NULL;
  if (!id || strcasecmp(id, RIJNDAEL_MVID)) {
    L_WARN("stage: Rijndael256 mscorlib differs; original decrypt preserved"); return;
  }
  void *type = klass(image, "System.Security.Cryptography", "RijndaelTransform");
  if (!type) {
    L_WARN("stage: System.Security.Cryptography.RijndaelTransform missing; original decrypt preserved"); return;
  }
  void *mm = method(type, "Decrypt256", 3);
  if (!mm) { L_WARN("stage: Rijndael256 method missing; original decrypt preserved"); return; }
  void *mh = header(mm);
  uint32_t size = 0, stack = 0;
  const uint8_t *il = mh ? code(mh, &size, &stack) : NULL;
  int valid = il && size == sizeof(rijndael256_il) && !memcmp(il, rijndael256_il, size);
  if (mh) free_header(mh);
  if (!valid) { L_WARN("stage: Rijndael256 IL differs; original decrypt preserved"); return; }
  void *vt = vtable(domain(), type);
  if (!vt) return;
  init(vt); // Populate the original immutable tables before copying them.
  const char *names[] = {"iT0", "iT1", "iT2", "iT3", "iSBox"};
  void *array_types[5];
  for (unsigned i = 0; i < 5; i++) {
    void *f = field(type, names[i]), *array = NULL;
    if (!f) return;
    static_value(vt, f, &array);
    if (!array || array_length(array) != 256) return;
    array_types[i] = object_class(array);
    if (!array_types[i] || (i && i < 4 && array_types[i] != array_types[0])) return;
    unsigned stride = i < 4 ? 4 : 1;
    void *data = array_data(array, stride, 0);
    if (!data || data != (char *)array + 16) return;
    memcpy(i < 4 ? (void *)tables.inverse[i] : (void *)tables.sbox, data, stride * 256);
  }
  uintptr_t address = (uintptr_t)compile(mm);
  if (!address || (address & 3)) { L_WARN("stage: Rijndael256 JIT is not aligned ARM; preserved"); return; }
  uint32_t first[2]; memcpy(first, (void *)address, sizeof(first));
  if (!safe_prologue(first[0]) || !safe_prologue(first[1])) {
    L_WARN("stage: Rijndael256 JIT prologue differs (%08X %08X); preserved", first[0], first[1]); return;
  }
  void *trampoline = mmap_bridge(NULL, 4096, 7, 0x20, -1, 0);
  if (!trampoline || trampoline == (void *)-1) return;
  uint32_t instructions[] = {first[0], first[1], 0xe51ff004, (uint32_t)address + 8};
  memcpy(trampoline, instructions, sizeof(instructions));
  kuKernelFlushCaches(trampoline, sizeof(instructions));
  crypto_class = type; word_array_class = array_types[0]; byte_array_class = array_types[4];
  original = (decrypt_fn)trampoline;
  hook_addr(address, (uintptr_t)decrypt_block);
  L_INFO("stage: Rijndael256 native blocks installed (original keys/CBC/padding/text preserved)");
#undef API
}
unsigned rijndael256_fast_calls(void) { return __atomic_load_n(&fast_calls, __ATOMIC_RELAXED); }
unsigned rijndael256_fallback_calls(void) { return __atomic_load_n(&fallback_calls, __ATOMIC_RELAXED); }

// Only used after the separate CBC hook has validated the transform's mode,
// block size and round count. No mutation until every array and alias guard
// succeeds; unusual input follows the unmodified managed method.
int rijndael256_try_cbc(void *self, void *input, void *output, void *iv, void *scratch, void *keys) {
  if (!original || !self || object_class(self) != crypto_class || !keys ||
      object_class(keys) != word_array_class || array_length(keys) < 120) return 0;
  void *bytes[] = {input, output, iv, scratch};
  for (unsigned i = 0; i < 4; i++)
    if (!bytes[i] || object_class(bytes[i]) != byte_array_class || array_length(bytes[i]) < 32) return 0;
  if (iv == scratch || iv == input || iv == output || scratch == input || scratch == output) return 0;
  rijndael256_cbc_block((uint8_t *)array_data(input, 1, 0), (uint8_t *)array_data(output, 1, 0),
                        (uint8_t *)array_data(iv, 1, 0), (uint8_t *)array_data(scratch, 1, 0),
                        (uint32_t *)array_data(keys, 4, 0), &tables);
  return 1;
}
int rijndael256_ready(void) { return original != NULL; }

#include "ui_geometry.h"
#include "ui_geometry_copy.h"
#include "ui_geometry_il.h"
#include "../loader/so_util.h"
#include "log.h"
#include <kubridge.h>
#include <sys/types.h>

typedef void (*geometry_fn)(void *, void *, void *, void *, void *, void *);
static geometry_fn original;
static unsigned fast_calls, fallback_calls;
static uint32_t geo_offset[3], buffer_offset[3], size_offset[3];
static void *list_class[3];
static void *(*object_class)(void *);
extern void *mmap_bridge(void *, size_t, int, int, int, off_t);

static int read_list(void *list, unsigned i, ui_copy_list *out) {
  if (!list || object_class(list) != list_class[i]) return 0;
  uint8_t *buffer = *(uint8_t **)((char *)list + buffer_offset[i]);
  if (!buffer) return 0;
  out->data = buffer + 16;
  out->used = *(int32_t *)((char *)list + size_offset[i]);
  out->capacity = *(int32_t *)(buffer + 12);
  return out->used >= 0 && out->used <= out->capacity;
}
static void geometry_copy(void *self, void *verts, void *uvs, void *cols, void *norms, void *tans) {
  if (self && !norms) {
    void *source[3] = {*(void **)((char *)self + geo_offset[0]), *(void **)((char *)self + geo_offset[1]),
                       *(void **)((char *)self + geo_offset[2])};
    if (!source[0]) { fast_calls++; return; }
    int32_t count = *(int32_t *)((char *)source[0] + size_offset[0]);
    if (count <= 0) { fast_calls++; return; }
    void *target[3] = {verts, uvs, cols};
    ui_copy_list src[3], dst[3];
    int valid = 1;
    for (unsigned i = 0; i < 3; i++)
      if (!read_list(source[i], i, &src[i]) || !read_list(target[i], i, &dst[i])) valid = 0;
    if (valid && ui_copy_geometry(src, dst, count)) {
      for (unsigned i = 0; i < 3; i++) *(int32_t *)((char *)target[i] + size_offset[i]) = dst[i].used;
      fast_calls++;
      return;
    }
  }
  // Capacity growth, normals/tangents and unusual inputs retain the exact
  // original method through a permanent trampoline. No unpatching/reentry.
  fallback_calls++;
  original(self, verts, uvs, cols, norms, tans);
}
// Only relocate PC-independent ARM prologue instructions. Any other JIT
// layout keeps the original implementation instead of guessing relocations.
static int safe_prologue(uint32_t word) {
  return ((word & 0xffff8000u) == 0xe92d0000u) ||
         ((word & 0xfffff000u) == 0xe24dd000u) || word == 0xe1a0b00du || word == 0xe1a0c00du ||
         ((word & 0xffff0000u) == 0xe58d0000u && ((word >> 12) & 15) != 15);
}
void ui_geometry_install(void *image) {
  if (original || !image) return;
  so_module *mono = so_find_module("libmono.so");
#define API(type, symbol) ((type)so_symbol(mono, symbol))
  void *(*klass)(void *, const char *, const char *) = API(void *(*)(void *, const char *, const char *), "mono_class_from_name");
  void *(*field)(void *, const char *) = API(void *(*)(void *, const char *), "mono_class_get_field_from_name");
  uint32_t (*offset)(void *) = API(uint32_t (*)(void *), "mono_field_get_offset");
  void *(*field_type)(void *) = API(void *(*)(void *), "mono_field_get_type");
  void *(*type_class)(void *) = API(void *(*)(void *), "mono_class_from_mono_type");
  void *(*method)(void *, const char *, int) = API(void *(*)(void *, const char *, int), "mono_class_get_method_from_name");
  void *(*header)(void *) = API(void *(*)(void *), "mono_method_get_header");
  const uint8_t *(*code)(void *, uint32_t *, uint32_t *) = API(const uint8_t *(*)(void *, uint32_t *, uint32_t *), "mono_method_header_get_code");
  void (*free_header)(void *) = API(void (*)(void *), "mono_metadata_free_mh");
  void *(*compile)(void *) = API(void *(*)(void *), "mono_compile_method");
  object_class = API(void *(*)(void *), "mono_object_get_class");
  if (!klass || !field || !offset || !field_type || !type_class || !method || !header || !code ||
      !free_header || !compile || !object_class) return;
  void *geo = klass(image, "", "UIGeometry");
  void *mm = geo ? method(geo, "WriteToBuffers", 5) : NULL;
  void *mh = mm ? header(mm) : NULL;
  uint32_t size = 0, stack = 0;
  const uint8_t *il = mh ? code(mh, &size, &stack) : NULL;
  int valid = il && size == sizeof(ui_geometry_il) && !memcmp(il, ui_geometry_il, size);
  if (mh) free_header(mh);
  if (!valid) { L_WARN("ui: geometry IL differs; original append preserved"); return; }
  const char *fields[3] = {"mRtpVerts", "uvs", "cols"};
  for (unsigned i = 0; i < 3; i++) {
    void *f = field(geo, fields[i]);
    list_class[i] = f ? type_class(field_type(f)) : NULL;
    void *buf = list_class[i] ? field(list_class[i], "buffer") : NULL;
    void *used = list_class[i] ? field(list_class[i], "size") : NULL;
    if (!f || !buf || !used) return;
    geo_offset[i] = offset(f); buffer_offset[i] = offset(buf); size_offset[i] = offset(used);
  }
  uintptr_t address = (uintptr_t)compile(mm);
  if (!address || (address & 3)) { L_WARN("ui: geometry JIT is not aligned ARM; preserved"); return; }
  uint32_t first[2]; memcpy(first, (void *)address, sizeof(first));
  if (!safe_prologue(first[0]) || !safe_prologue(first[1])) {
    L_WARN("ui: geometry JIT prologue differs (%08X %08X); original append preserved", first[0], first[1]); return;
  }
  void *trampoline = mmap_bridge(NULL, 4096, 7, 0x20, -1, 0);
  if (!trampoline || trampoline == (void *)-1) return;
  uint32_t instructions[] = {first[0], first[1], 0xe51ff004, (uint32_t)address + 8};
  memcpy(trampoline, instructions, sizeof(instructions));
  kuKernelFlushCaches(trampoline, sizeof(instructions));
  original = (geometry_fn)trampoline;
  hook_addr(address, (uintptr_t)geometry_copy);
  L_INFO("ui: NGUI geometry appends use bounded bulk copies (original growth/normal path retained)");
#undef API
}
unsigned ui_geometry_fast_calls(void) { return fast_calls; }
unsigned ui_geometry_fallback_calls(void) { return fallback_calls; }

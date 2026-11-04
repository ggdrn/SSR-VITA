// Asks the game's own C# side what it is doing, through libmono's embedding
// API: the loaded scene, whether a level is loading, time scale, how many
// GameObjects exist and what every camera renders. Runs on the main (Unity)
// thread between frames, so Unity's API is used from the thread it expects.

#include "loader/so_util.h"
#include "bridges/bridges.h"
#include "util/log.h"
#include "util/gc_trace.h"
#include "util/hot_loops.h"
#include "util/work_trace.h"
#include "util/observe.h"
#include <psp2/kernel/sysmem.h>
#include "util/gc_policy.h"
#include "util/bundle_mips.h"
#include "ui_mesh.h"
#include "util/ui_geometry.h"
#include "util/managed_trace.h"
#include "util/buddy_ui.h"
#include "util/native_heap.h"
#include "util/race_warmup.h"
#include "util/terrain_gc.h"
#include "util/ui_trim.h"
#include "util/ui_sort.h"
#include "util/rijndael256.h"
#include "util/stage_parse.h"
#include "config.h"
#include "port_menu.h"
#include <malloc.h>
#include <stdio.h>
#include <psp2/kernel/processmgr.h>

#include <kubridge.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>

typedef void MonoImage, MonoClass, MonoMethod, MonoObject, MonoDomain, MonoType, MonoField;

static struct {
  MonoImage *(*image_loaded)(const char *);
  MonoClass *(*class_from_name)(MonoImage *, const char *, const char *);
  MonoMethod *(*get_method)(MonoClass *, const char *, int);
  MonoObject *(*invoke)(MonoMethod *, void *, void **, MonoObject **);
  char *(*string_to_utf8)(MonoObject *);
  void (*mono_free)(void *);
  MonoDomain *(*domain_get)(void);
  MonoType *(*class_get_type)(MonoClass *);
  MonoObject *(*type_get_object)(MonoDomain *, MonoType *);
  MonoClass *(*object_get_class)(MonoObject *);
  const char *(*class_get_name)(MonoClass *);
  const char *(*class_get_namespace)(MonoClass *);
  MonoImage *(*get_corlib)(void);
  MonoObject *(*string_new)(MonoDomain *, const char *);
  void *(*compile_method)(MonoMethod *);
  void *(*object_unbox)(MonoObject *);
  int (*gc_count)(int);
  int64_t (*gc_heap)(void);
  int64_t (*gc_used)(void);
  MonoField *(*get_field)(MonoClass *, const char *);
  void (*field_value)(MonoObject *, MonoField *, void *);
} api;

static MonoImage *unity_image;
static int probe_disabled;

#define SYM(field, name)                               \
  do {                                                 \
    *(void **)&api.field = (void *)so_symbol(m, name); \
    if (!api.field) {                                  \
      L_WARN("probe: %s not exported", name);          \
      return 0;                                        \
    }                                                  \
  } while (0)

// This APK does not export Boehm's tuning API. Verify the complete native
// min_bytes_allocd function, its relocated GOT slot and the default value.
// Only the GC notifier writes the setting, under the collector's own lock.
static void install_gc_policy(so_module *mono) {
  if (gc_policy_bound()) return;
  static const uint32_t expected[] = {0xe92d4810,0xe28db008,0xe24dd00c,0xe59f30e8,0xe08f3003,0xe3024710,0xe3540000,0xaa000000,0xe2644000,0xe1a02084,0xe1a01002,0xe59f20cc,0xe7932002,0xe5922000,0xe0812002,0xe50b2010,0xe59f20bc,0xe7932002,0xe5921000,0xe59f20b0,0xe7932002,0xe5922014,0xe0621001,0xe59f20a0,0xe7932002,0xe5922014,0xe1a02122,0xe0811002,0xe51b2010,0xe0812002,0xe1a02122,0xe50b2014,0xe59f2080,0xe7932002,0xe5922000,0xe3520000,0x0a00000f,0xe59f2070,0xe7932002,0xe5921000,0xe304223f,0xe340200f,0xe1510002,0x0a000008,0xe59f2058,0xe7933002,0xe5933000,0xe1a03083,0xe51b0014,0xe1a01003,0xeb0062dc,0xe1a03000,0xea000006,0xe59f2034,0xe7933002,0xe5933000,0xe51b0014,0xe1a01003,0xeb0062d4,0xe1a03000,0xe1a00003,0xe24bd008,0xe8bd8810,0x000f10bc,0xfffffe08,0xfffffdc4,0xfffffddc,0xfffffde0,0xfffffe0c};
  uintptr_t base = mono->base;
  if (mono->end < base + 0x3b22e4 ||
      so_symbol(mono, "mono_gc_collect") != base + 0x13f0f4 ||
      memcmp((void *)(base + 0x2bcb30), expected, sizeof(expected)) ||
      *(uintptr_t *)(base + 0x3ada10) != base + 0x3b22e0 ||
      !gc_policy_attach((uint32_t *)(base + 0x3b22e0))) {
    L_WARN("gc: native budget layout differs; original collector policy preserved");
    return;
  }
  L_INFO("gc: adaptive collection budget ready (default divisor 3, bounded gameplay target 2)");
}
static int gc_playing_scene;
static uint8_t (*gc_is_loading)(void);

void mono_gc_policy_tick(void) {
  // The policy binds at load time now; the probe API arrives later.
  if (!gc_policy_bound() || !unity_image || !api.gc_heap || !api.gc_count) return;
  if (!gc_is_loading) {
    so_module *mono = so_find_module("libmono.so");
    void *(*lookup)(MonoMethod *) = (void *)so_symbol(mono, "mono_lookup_internal_call");
    MonoClass *klass = api.class_from_name(unity_image, "UnityEngine", "Application");
    MonoMethod *method = klass ? api.get_method(klass, "get_isLoadingLevel", 0) : NULL;
    void *fn = method && lookup ? lookup(method) : NULL;
    if (fn && so_module_at((uintptr_t)fn) == so_find_module("libunity.so")) gc_is_loading = fn;
  }
  native_heap_sample native = native_heap_available();
  uint64_t heap = api.gc_heap(), used = api.gc_used();
  uint64_t native_free = native.total_free;
  unsigned next = gc_policy_request(gc_playing_scene, !gc_is_loading || gc_is_loading(), heap, used, native_free);
  static unsigned last = 3;
  if (next != last) {
    L_INFO("gc: requested divisor %u; managed heap %u free %u native free %u KiB", next,
           (unsigned)(heap / 1024), (unsigned)((heap >= used ? heap - used : 0) / 1024), (unsigned)(native_free / 1024));
    last = next;
  }
  int collections = api.gc_count(0);
  unsigned budget = gc_policy_pacing_tick(collections, heap, used, native_free);
  static unsigned last_budget;
  static int last_cycle = -1;
  if (budget != last_budget && (budget == 0 || last_budget == 0 || collections != last_cycle)) {
    L_INFO("gc: pacing budget %u KiB (collector default %u KiB) heap %u large-free %u native free %u KiB cycle %d",
           budget * 4 / 1024, gc_policy_base_words() * 4 / 1024, gc_policy_pacing_heap() / 1024,
           gc_policy_pacing_large_free() / 1024, (unsigned)(native_free / 1024), collections);
    last_cycle = collections;
  }
  last_budget = budget;
}

static void (*original_gc_event)(int);
static void traced_gc_event(int event) {
  gc_policy_event(event);
  gc_trace_event(event, sceKernelGetProcessTimeWide());
  // Keep Mono's statistics and profiler notifications intact.
  if (original_gc_event) original_gc_event(event);
}

static void install_gc_trace(so_module *mono) {
  static int installed;
  if (installed) return;
  void (**notify)(int) = (void *)so_symbol(mono, "GC_notify_event");
  void (*original)(int) = notify ? __atomic_load_n(notify, __ATOMIC_ACQUIRE) : NULL;
  if (!original || so_module_at((uintptr_t)original) != mono) return;
  original_gc_event = original;
  __atomic_store_n(notify, traced_gc_event, __ATOMIC_RELEASE);
  installed = 1;
  L_INFO("perf: installed allocation-free GC phase timings (original notifier preserved)");
}

// Diagnostic: race hitches of 100-250 ms show no GC, file I/O, shader link
// or instantiate time. Time JIT compilation on the main thread (methods are
// compiled on first call) without changing code generation: only the JIT
// event flag is enabled; allocation events stay off so allocators remain inline.
static SceUID jit_main_thread;
static uint64_t jit_started, jit_frame_us, jit_frame_longest_us, jit_window_us;
static unsigned jit_depth, jit_frame_methods, jit_window_methods, jit_other_methods;
static MonoMethod *jit_frame_longest, *jit_window_longest;
static uint64_t jit_window_longest_us;
static MonoClass *(*method_class_fn)(MonoMethod *);
static const char *(*method_name_fn)(MonoMethod *);

static void jit_begin(void *profiler, MonoMethod *method) {
  (void)profiler; (void)method;
  if (sceKernelGetThreadId() != jit_main_thread) return;
  if (jit_depth++ == 0) jit_started = sceKernelGetProcessTimeWide();
}
static void jit_end(void *profiler, MonoMethod *method, int result) {
  (void)profiler; (void)result;
  if (sceKernelGetThreadId() != jit_main_thread) { __atomic_add_fetch(&jit_other_methods, 1, __ATOMIC_RELAXED); return; }
  jit_frame_methods++;
  if (!jit_depth || --jit_depth) return;
  uint64_t us = sceKernelGetProcessTimeWide() - jit_started;
  jit_frame_us += us;
  if (us > jit_frame_longest_us) { jit_frame_longest_us = us; jit_frame_longest = method; }
}
static void install_jit_trace(so_module *mono) {
  static int installed;
  if (installed) return;
  installed = 1;
  void (*install)(void *, void *) = (void *)so_symbol(mono, "mono_profiler_install");
  void (*install_jit)(void *, void *) = (void *)so_symbol(mono, "mono_profiler_install_jit_compile");
  void (*set_events)(int) = (void *)so_symbol(mono, "mono_profiler_set_events");
  method_class_fn = (void *)so_symbol(mono, "mono_method_get_class");
  method_name_fn = (void *)so_symbol(mono, "mono_method_get_name");
  if (!install || !install_jit || !set_events || !method_class_fn || !method_name_fn) {
    L_WARN("jit: Mono profiler API unavailable; compile time not traced");
    return;
  }
  static uint32_t profiler_state;
  jit_main_thread = sceKernelGetThreadId();
  install(&profiler_state, NULL);
  install_jit(jit_begin, jit_end);
  set_events(0x10); // MONO_PROFILE_JIT_COMPILATION for this profiler only
  L_INFO("jit: main-thread compile timing installed (allocation profiling off)");
}
static void jit_method_label(MonoMethod *method, char *out, size_t size) {
  MonoClass *klass = method && method_class_fn ? method_class_fn(method) : NULL;
  snprintf(out, size, "%s.%s", klass && api.class_get_name ? api.class_get_name(klass) : "?",
           method && method_name_fn ? method_name_fn(method) : "-");
}

static int probe_init(void) {
  so_module *m = so_find_module("libmono.so");
  if (!m)
    return 0;
  SYM(image_loaded, "mono_image_loaded");
  SYM(class_from_name, "mono_class_from_name");
  SYM(get_method, "mono_class_get_method_from_name");
  SYM(invoke, "mono_runtime_invoke");
  SYM(string_to_utf8, "mono_string_to_utf8");
  SYM(mono_free, "mono_free");
  SYM(domain_get, "mono_domain_get");
  SYM(class_get_type, "mono_class_get_type");
  SYM(type_get_object, "mono_type_get_object");
  SYM(object_get_class, "mono_object_get_class");
  SYM(class_get_name, "mono_class_get_name");
  SYM(class_get_namespace, "mono_class_get_namespace");
  SYM(get_corlib, "mono_get_corlib");
  SYM(string_new, "mono_string_new");
  SYM(compile_method, "mono_compile_method");
  SYM(object_unbox, "mono_object_unbox");
  SYM(gc_count, "mono_gc_collection_count");
  SYM(gc_heap, "mono_gc_get_heap_size");
  SYM(gc_used, "mono_gc_get_used_size");
  SYM(get_field, "mono_class_get_field_from_name");
  SYM(field_value, "mono_field_get_value");
  unity_image = api.image_loaded("UnityEngine");
  if (!unity_image)
    unity_image = api.image_loaded("/vita/srr224.apk/assets/bin/Data/Managed/UnityEngine.dll");
  if (unity_image) {
    install_gc_trace(m);
    if (original_gc_event) install_gc_policy(m);
    install_jit_trace(m);
  }
  return unity_image != NULL;
}

// Invokes a static or instance method; returns NULL (and logs) on exceptions.
static MonoObject *call(const char *cls, const char *method, void *self, void **args, int nargs) {
  MonoClass *k = api.class_from_name(unity_image, "UnityEngine", cls);
  MonoMethod *mm = k ? api.get_method(k, method, nargs) : NULL;
  if (!mm) {
    L_WARN("probe: UnityEngine.%s.%s not found", cls, method);
    return NULL;
  }
  MonoObject *exc = NULL;
  MonoObject *r = api.invoke(mm, self, args, &exc);
  if (exc) {
    L_WARN("probe: UnityEngine.%s.%s threw %s", cls, method, api.class_get_name(api.object_get_class(exc)));
    return NULL;
  }
  return r;
}

static MonoObject *call_in(MonoImage *img, const char *ns, const char *cls, const char *method, void *self,
                           void **args, int nargs) {
  MonoClass *k = api.class_from_name(img, ns, cls);
  MonoMethod *mm = k ? api.get_method(k, method, nargs) : NULL;
  if (!mm) {
    L_WARN("probe: %s.%s.%s not found", ns, cls, method);
    return NULL;
  }
  MonoObject *exc = NULL;
  MonoObject *r = api.invoke(mm, self, args, &exc);
  return exc ? NULL : r;
}

// This optional menu prompt used Convert.ToDateTime with InvariantInfo even
// though the game writes facebookTime using the current culture. Keep the
// original comparison, but contain parse failures at this boundary.
static MonoImage *game_image;
static MonoMethod *ui_panel_late_method;
typedef SOFTFP float (*input_time_getter)(void);
static input_time_getter input_time_scale;
static int input_race_scene;
void mono_input_tick(void) {
  input_set_race_controls(input_race_scene && input_time_scale && input_time_scale() > 0.0f);
}
static int facebook_window_safe(MonoObject *self) {
  MonoImage *corlib = api.get_corlib();
  MonoObject *date = call_in(game_image, "SaveData", "SystemData", "get_facebookTime", self, NULL, 0);
  if (!date) return 1;
  char *utf8 = api.string_to_utf8(date);
  int empty = !utf8 || !*utf8;
  api.mono_free(utf8);
  if (empty) return 1;
  MonoObject *provider = call_in(corlib, "System.Globalization", "DateTimeFormatInfo", "get_InvariantInfo", NULL, NULL, 0);
  void *args[2] = {date, provider};
  MonoObject *saved = call_in(corlib, "System", "Convert", "ToDateTime", NULL, args, 2);
  if (!saved) {
    args[1] = call_in(corlib, "System.Globalization", "CultureInfo", "get_CurrentCulture", NULL, NULL, 0);
    saved = call_in(corlib, "System", "Convert", "ToDateTime", NULL, args, 2);
  }
  if (!saved) {
    static int warned;
    if (!warned++) L_WARN("menu: invalid facebookTime; optional Facebook prompt skipped (save preserved)");
    return 0;
  }
  MonoObject *now = call_in(corlib, "System", "DateTime", "get_Now", NULL, NULL, 0);
  if (!now) return 0;
  void *compare_args[2] = {api.object_unbox(now), api.object_unbox(saved)};
  MonoObject *result = call_in(corlib, "System", "DateTime", "Compare", NULL, compare_args, 2);
  return result && *(int32_t *)api.object_unbox(result) > 0;
}

// Replace only the game's optional debug logger, never the playback method.
// Bounded sampling avoids a synchronous file write for every collected ring.
static unsigned sound_requests;
static void sound_play_log(MonoObject *message) {
  unsigned n = ++sound_requests;
  if (n > 24 && (n & (n - 1)) != 0) return;
  char *text = message ? api.string_to_utf8(message) : NULL;
  L_INFO("sound: request %u %s", n, text ? text : "(null)");
  if (text) api.mono_free(text);
}

// Texture budget: use Unity's own mip limit so atlas UVs and dimensions keep
// their engine semantics. The internal-call bridge also covers inlined C#
// overloads, while preserving the game's level/applyExpensiveChanges arguments.
static int (*texture_limit_native_get)(void);
static void (*texture_limit_native_set)(int);
static void (*quality_level_original)(int, int);
static int texture_budget_ready;
static void texture_budget_init(void);

void mono_texture_budget_tick(void) {
  if (!texture_budget_ready) texture_budget_init();
  if (!texture_limit_native_get || !texture_limit_native_set) return;
  int current = texture_limit_native_get();
  bundle_mips_set_limit(current);
  if (current >= 1) return; // Preserve a stronger limit already selected.
  texture_limit_native_set(1);
  int applied = texture_limit_native_get();
  bundle_mips_set_limit(applied);
  if (applied >= 1)
    L_INFO("mem: Unity masterTextureLimit %d -> %d (texture memory cap; quality mode preserved)", current, applied);
  else {
    static int warned;
    if (!warned++) L_WARN("mem: Unity did not apply masterTextureLimit=1");
  }
}

static int quality_level_forced = -1;  // Port settings level; -1 keeps the game's.
static void quality_level_with_texture_budget(int level, int expensive) {
  if (quality_level_forced >= 0) level = quality_level_forced;
  // Any texture reload inside the quality change reads complete mip chains.
  bundle_mips_set_limit(0);
  quality_level_original(level, expensive);
  mono_texture_budget_tick();
}

static void texture_budget_init(void) {
  if (texture_budget_ready || !unity_image || !api.class_from_name) return;
  MonoClass *klass = api.class_from_name(unity_image, "UnityEngine", "QualitySettings");
  if (!klass) return;
  so_module *mono = so_find_module("libmono.so");
  so_module *unity = so_find_module("libunity.so");
  void *(*lookup)(MonoMethod *) = (void *)so_symbol(mono, "mono_lookup_internal_call");
  void (*add)(const char *, const void *) = (void *)so_symbol(mono, "mono_add_internal_call");
  if (!lookup || !add || !unity) {
    texture_budget_ready = -1;
    L_WARN("mem: Unity texture budget API unavailable");
    return;
  }
  MonoMethod *getter = api.get_method(klass, "get_masterTextureLimit", 0);
  MonoMethod *setter = api.get_method(klass, "set_masterTextureLimit", 1);
  MonoMethod *quality = api.get_method(klass, "SetQualityLevel", 2);
  void *get = getter ? lookup(getter) : NULL;
  void *set = setter ? lookup(setter) : NULL;
  void *level = quality ? lookup(quality) : NULL;
  if (!get || !set || !level) return; // Retry once the engine's icalls are registered.
  if (so_module_at((uintptr_t)get) != unity || so_module_at((uintptr_t)set) != unity ||
      so_module_at((uintptr_t)level) != unity) {
    texture_budget_ready = -1;
    L_WARN("mem: texture budget icalls did not resolve to Unity; left original");
    return;
  }
  texture_budget_ready = 1;
  texture_limit_native_get = get;
  texture_limit_native_set = set;
  quality_level_original = level;
  // Exact registration names verified in this APK's libunity.so. Call the
  // original native implementation directly, never through this new mapping.
  add("UnityEngine.QualitySettings::SetQualityLevel", quality_level_with_texture_budget);
  if (lookup(quality) == (void *)quality_level_with_texture_budget)
    L_INFO("mem: installed allocation-free texture budget guard on quality changes");
  else L_WARN("mem: quality icall kept its old entry; periodic texture budget guard remains active");
  mono_texture_budget_tick();
}

// Each WWW.texture access creates another native texture. Reuse the image
// already assigned to the owner instead of decoding/uploading it 2–3 times.
static void patch_image_il(void) {
  so_module *mono = so_find_module("libmono.so");
  MonoClass *(*nested)(MonoClass *, void **) = (void *)so_symbol(mono, "mono_class_get_nested_types");
  const char *(*name)(MonoClass *) = (void *)so_symbol(mono, "mono_class_get_name");
  void *(*get_header)(MonoMethod *) = (void *)so_symbol(mono, "mono_method_get_header");
  const uint8_t *(*get_code)(void *, uint32_t *, uint32_t *) =
    (void *)so_symbol(mono, "mono_method_header_get_code");
  void (*free_header)(void *) = (void *)so_symbol(mono, "mono_metadata_free_mh");
  if (!nested || !name || !get_header || !get_code || !free_header) {
    L_WARN("mem: image IL patch API unavailable");
    return;
  }
  static const uint8_t p0_0_old[] = {0x02,0x7c,0x4a,0x3d,0x00,0x04,0x02,0x7b,0x4e,0x3d,0x00,0x04,0x6f,0x72,0x00,0x00,0x0a,0x14,0x28,0x29,0x00,0x00,0x0a,0x3a,0x06,0x00,0x00,0x00,0x14,0x38,0x0b,0x00,0x00,0x00,0x02,0x7b,0x4e,0x3d,0x00,0x04,0x6f,0x72,0x00,0x00,0x0a,0x7d,0x92,0x15,0x00,0x04};
  static const uint8_t p0_0_new[] = {0x02,0x7c,0x4a,0x3d,0x00,0x04,0x02,0x7b,0x4e,0x3d,0x00,0x04,0x6f,0x72,0x00,0x00,0x0a,0x7d,0x92,0x15,0x00,0x04,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
  static const uint8_t p0_1_old[] = {0x02,0x7b,0x4e,0x3d,0x00,0x04,0x6f,0x72,0x00,0x00,0x0a};
  static const uint8_t p0_1_new[] = {0x02,0x7c,0x4a,0x3d,0x00,0x04,0x7b,0x92,0x15,0x00,0x04};
  static const uint8_t p1_0_old[] = {0x02,0x7b,0x6b,0x3d,0x00,0x04,0x6f,0x72,0x00,0x00,0x0a,0x14,0x28,0x29,0x00,0x00,0x0a,0x39,0xcc,0x00,0x00,0x00,0x02,0x7b,0x6a,0x3d,0x00,0x04,0x02,0x7b,0x6b,0x3d,0x00,0x04,0x6f,0x72,0x00,0x00,0x0a,0x6f,0x2a,0x17,0x00,0x06};
  static const uint8_t p1_0_new[] = {0x02,0x7b,0x6a,0x3d,0x00,0x04,0x02,0x7b,0x6b,0x3d,0x00,0x04,0x6f,0x72,0x00,0x00,0x0a,0x6f,0x2a,0x17,0x00,0x06,0x02,0x7b,0x6a,0x3d,0x00,0x04,0x6f,0x29,0x17,0x00,0x06,0x14,0x28,0x29,0x00,0x00,0x0a,0x39,0xb6,0x00,0x00,0x00};
  const struct { const char *root, *child, *grandchild; uint32_t size; } targets[] = {
    {"HudLoadingCharaInfo", "<LoadWWW>c__Iterator37", NULL, 0x551},
    {"InformationImageManager", "ImageData", "<DoFetchImages>c__Iterator3B", 0x17d}
  };
  for (unsigned i = 0; i < 2; i++) {
    MonoClass *klass = api.class_from_name(game_image, "", targets[i].root);
    const char *children[2] = {targets[i].child, targets[i].grandchild};
    for (unsigned depth = 0; depth < 2 && children[depth] && klass; depth++) {
      void *iter = NULL;
      MonoClass *child, *found = NULL;
      while ((child = nested(klass, &iter))) {
        if (!strcmp(name(child), children[depth])) { found = child; break; }
      }
      klass = found;
    }
    MonoMethod *method = klass ? api.get_method(klass, "MoveNext", 0) : NULL;
    void *header = method ? get_header(method) : NULL;
    uint32_t size = 0, stack = 0;
    const uint8_t *code = header ? get_code(header, &size, &stack) : NULL;
    int matches = code && size == targets[i].size;
    if (matches && i == 0) {
      matches = !memcmp(code + 0x3fd, p0_0_old, sizeof(p0_0_old)) && !memcmp(code + 0x440, p0_1_old, sizeof(p0_1_old));
    }
    if (matches && i == 1) {
      matches = !memcmp(code + 0x7c, p1_0_old, sizeof(p1_0_old));
    }
    if (matches) {
      if (i == 0) {
        kuKernelCpuUnrestrictedMemcpy((void *)(code + 0x3fd), p0_0_new, sizeof(p0_0_new));
        kuKernelCpuUnrestrictedMemcpy((void *)(code + 0x440), p0_1_new, sizeof(p0_1_new));
      }
      if (i == 1) {
        kuKernelCpuUnrestrictedMemcpy((void *)(code + 0x7c), p1_0_new, sizeof(p1_0_new));
      }
      kuKernelFlushCaches((void *)code, size);
      L_INFO("mem: %s image loader now creates one texture per download (resolution preserved)", targets[i].root);
    } else L_WARN("mem: %s image IL differs; left original", targets[i].root);
    if (header) free_header(header);
  }
}

// Patches the original IL before gameplay methods are JIT compiled. Only
// the two verified call sites change; the APK and save remain untouched.
static void patch_stage_il(void) {
  so_module *mono = so_find_module("libmono.so");
  void *(*get_header)(MonoMethod *) = (void *)so_symbol(mono, "mono_method_get_header");
  const uint8_t *(*get_code)(void *, uint32_t *, uint32_t *) =
    (void *)so_symbol(mono, "mono_method_header_get_code");
  void (*free_header)(void *) = (void *)so_symbol(mono, "mono_metadata_free_mh");
  if (!get_header || !get_code || !free_header) {
    L_WARN("stage: IL patch API unavailable");
    return;
  }
  // ActivateBlock's final instruction forces a complete GC on each terrain
  // block activation. Automatic allocation-triggered GC and every other
  // explicit collection (loading/scene transitions) remain original.
  MonoClass *klass = api.class_from_name(game_image, "", "ObjectSpawnManager");
  MonoMethod *method = klass ? api.get_method(klass, "ActivateBlock", -1) : NULL;
  void *header = method ? get_header(method) : NULL;
  uint32_t size = 0, stack = 0;
  const uint8_t *code = header ? get_code(header, &size, &stack) : NULL;
  static const uint8_t gc_tail[] = {0x6f, 0x89, 0x00, 0x00, 0x0a, 0x28, 0x89, 0x08, 0x00, 0x0a, 0x2a};
  static const uint8_t nops[5] = {0};
  if (code && size == 0xfb && memcmp(code + 0xf0, gc_tail, sizeof(gc_tail)) == 0) {
    kuKernelCpuUnrestrictedMemcpy((void *)(code + 0xf5), nops, sizeof(nops));
    kuKernelFlushCaches((void *)(code + 0xf5), sizeof(nops));
    L_INFO("stage: removed per-block forced GC (automatic and transition GC preserved)");
  } else L_WARN("stage: ActivateBlock IL differs; forced GC left original");
  if (header) free_header(header);

  // StateChangeLevel (level change inside a run) and OnClickEndPause (resume)
  // run GC.Collect; Resources.UnloadUnusedAssets; GC.Collect. That froze the
  // race for ~2.8 s every level change while native memory stayed flat
  // (92.0 -> 92.7 -> 92.3 MiB): the previous level leaves nothing to unload.
  // NOP only those three calls and the pop; the same cleanup still runs on
  // stage start/end, on entering pause and on every scene change. Verified:
  // no branch target or EH boundary inside the replaced span.
  static const uint8_t gc_unload_gc[] = {0x28,0x89,0x08,0x00,0x0a,0x28,0xa7,0x05,0x00,0x0a,0x26,0x28,0x89,0x08,0x00,0x0a};
  static const uint8_t gc_nops[sizeof(gc_unload_gc)] = {0};
  static const uint8_t change_before[] = {0x38,0x9e,0x05,0x00,0x00};
  static const uint8_t change_after[] = {0x28,0x09,0x3e,0x00,0x06};
  static const uint8_t end_pause[] = {0x72,0x74,0x1b,0x00,0x70,0x72,0x94,0x1b,0x00,0x70,0x28,0x8a,0x3b,0x00,0x06,0x26,
                                      0x28,0x89,0x08,0x00,0x0a,0x28,0xa7,0x05,0x00,0x0a,0x26,0x28,0x89,0x08,0x00,0x0a,
                                      0x02,0x19,0x7d,0x59,0x14,0x00,0x04,0x02,0x22,0x00,0x00,0x80,0x3f,0x7d,0x5a,0x14,
                                      0x00,0x04,0x2a};
  const struct { const char *cls, *name; uint32_t size, at; } unload_sites[] = {
    {"GameModeStage", "StateChangeLevel", 0x670, 0xc6},
    {"HudCockpit", "OnClickEndPause", sizeof(end_pause), 0x10},
  };
  for (unsigned i = 0; i < sizeof(unload_sites) / sizeof(unload_sites[0]); i++) {
    klass = api.class_from_name(game_image, "", unload_sites[i].cls);
    method = klass ? api.get_method(klass, unload_sites[i].name, -1) : NULL;
    header = method ? get_header(method) : NULL;
    code = header ? get_code(header, &size, &stack) : NULL;
    uint32_t at = unload_sites[i].at;
    int ok = code && size == unload_sites[i].size && !memcmp(code + at, gc_unload_gc, sizeof(gc_unload_gc));
    if (ok && i == 0)
      ok = !memcmp(code + at - sizeof(change_before), change_before, sizeof(change_before)) &&
           !memcmp(code + at + sizeof(gc_unload_gc), change_after, sizeof(change_after));
    if (ok && i == 1) ok = !memcmp(code, end_pause, sizeof(end_pause));
    if (ok) {
      kuKernelCpuUnrestrictedMemcpy((void *)(code + at), gc_nops, sizeof(gc_nops));
      kuKernelFlushCaches((void *)(code + at), sizeof(gc_nops));
      L_INFO("stage: %s.%s no longer forces GC+UnloadUnusedAssets+GC", unload_sites[i].cls, unload_sites[i].name);
    } else L_WARN("stage: %s.%s IL differs; forced cleanup left original", unload_sites[i].cls, unload_sites[i].name);
    if (header) free_header(header);
  }

  // GC.Collect() calls placed right before/after Resources.UnloadUnusedAssets
  // (whose operation already runs a full collection) and on menu entry or
  // roulette spins. Each costs 0.4-1.9 s with the menu's live set; stage
  // start showed 5 back-to-back collections. Only the 5-byte call (no stack
  // effect) becomes NOPs; every UnloadUnusedAssets call stays, as do the
  // loading-screen collections (LoadSetTableFirst and downloads). The
  // terminal CreateTerrain collection is handled separately below: that
  // coroutine also runs when rebuilding terrain in the middle of a race.
  static const uint8_t gc_call[] = {0x28,0x89,0x08,0x00,0x0a};
  static const uint8_t call_nops[sizeof(gc_call)] = {0};
  const struct { const char *cls, *name; uint32_t size, at[2]; } collect_sites[] = {
    {"GameModeStage", "StateInit", 0xb04, {0x43, 0x4e}},
    {"GameModeStage", "StateFadeIn", 0xed, {0x28, 0x33}},
    {"GameModeStage", "StatePause", 0x16c, {0x77, 0x82}},
    {"GameModeStage", "StateQuickModeTimeUp", 0xe0, {0xad, 0xb8}},
    {"GameModeStage", "StateEnd", 0xdf, {0x4e, 0x59}},
    {"GameModeStage", "RegisterAllResource", 0x1fc, {0x1f1, 0}},
    {"GameModeTitle", "StateLoadLevel", 0x7b, {0x3f, 0}},
    {"MainMenu", "PrepareForSceneMove", 0x3f, {0x39, 0}},
    {"MainMenu", "Start", 0x2d1, {0x9d, 0}},
    {"FirstLaunchUserSetting", "StateEndProcessSpEgg", 0xea, {0xab, 0}},
    {"RouletteTop", "OnRouletteSpinStart", 0x154, {0x78, 0}},
    {"RouletteTop", "OnRouletteSpinEnd", 0xf0, {0x3f, 0}},
  };
  unsigned collect_removed = 0, collect_methods = 0;
  for (unsigned i = 0; i < sizeof(collect_sites) / sizeof(collect_sites[0]); i++) {
    klass = api.class_from_name(game_image, "", collect_sites[i].cls);
    method = klass ? api.get_method(klass, collect_sites[i].name, -1) : NULL;
    header = method ? get_header(method) : NULL;
    code = header ? get_code(header, &size, &stack) : NULL;
    int ok = code && size == collect_sites[i].size;
    for (unsigned j = 0; ok && j < 2 && collect_sites[i].at[j]; j++)
      ok = !memcmp(code + collect_sites[i].at[j], gc_call, sizeof(gc_call));
    if (ok) {
      for (unsigned j = 0; j < 2 && collect_sites[i].at[j]; j++) {
        kuKernelCpuUnrestrictedMemcpy((void *)(code + collect_sites[i].at[j]), call_nops, sizeof(call_nops));
        kuKernelFlushCaches((void *)(code + collect_sites[i].at[j]), sizeof(call_nops));
        collect_removed++;
      }
      collect_methods++;
    } else L_WARN("stage: %s.%s IL differs; GC.Collect left original", collect_sites[i].cls, collect_sites[i].name);
    if (header) free_header(header);
  }
  L_INFO("stage: removed %u redundant GC.Collect calls in %u methods (UnloadUnusedAssets kept)", collect_removed,
         collect_methods);

  // LightPlaySE suppresses rings and other pickup cues in LightMode. Replace
  // its IsLightMode() result with false, keeping branch/stack/cue selection.
  klass = api.class_from_name(game_image, "", "ObjUtil");
  method = klass ? api.get_method(klass, "LightPlaySE", 2) : NULL;
  header = method ? get_header(method) : NULL;
  code = header ? get_code(header, &size, &stack) : NULL;
  static const uint8_t light_se[] = {0x28,0xee,0x28,0x00,0x06,0x39,0x02,0x00,0x00,0x00,
                                    0x16,0x2a,0x02,0x03,0x28,0xc0,0x28,0x00,0x06,0x2a};
  static const uint8_t play_in_light_mode[] = {0x16,0,0,0,0};
  if (code && size == sizeof(light_se) && memcmp(code, light_se, sizeof(light_se)) == 0) {
    kuKernelCpuUnrestrictedMemcpy((void *)code, play_in_light_mode, sizeof(play_in_light_mode));
    kuKernelFlushCaches((void *)code, sizeof(play_in_light_mode));
    L_INFO("sound: enabled pickup cues in LightMode (graphics mode preserved)");
  } else L_WARN("sound: LightPlaySE IL differs; light-mode audio left original");
  if (header) free_header(header);
}

// BetterList<T>.Trim allocates an exact-size array for NGUI mesh uploads.
// Retain allocation and all branches; let Array.Copy handle values/references
// and GC write barriers instead of interpreting one ldelem/stelem per item.
static void patch_ui_trim_il(void) {
  so_module *mono = so_find_module("libmono.so");
  const char *(*guid)(MonoImage *) = (void *)so_symbol(mono, "mono_image_get_guid");
  MonoMethod *(*get_method)(MonoImage *, uint32_t, MonoClass *) = (void *)so_symbol(mono, "mono_get_method");
  void *(*get_header)(MonoMethod *) = (void *)so_symbol(mono, "mono_method_get_header");
  const uint8_t *(*get_code)(void *, uint32_t *, uint32_t *) = (void *)so_symbol(mono, "mono_method_header_get_code");
  void (*free_header)(void *) = (void *)so_symbol(mono, "mono_metadata_free_mh");
  const char *id = guid ? guid(game_image) : NULL;
  if (!id || strcasecmp(id, RACE_WARMUP_MVID) || !get_method || !get_header || !get_code || !free_header) {
    L_WARN("ui: Trim assembly/API differs; original copy preserved"); return;
  }
  MonoMethod *method = get_method(game_image, 0x060001ec, NULL);
  void *header = method ? get_header(method) : NULL;
  uint32_t size = 0, stack = 0;
  const uint8_t *code = header ? get_code(header, &size, &stack) : NULL;
  if (code && stack >= 5 && size == sizeof(ui_trim_original) && !memcmp(code, ui_trim_original, size)) {
    kuKernelCpuUnrestrictedMemcpy((void *)(code + UI_TRIM_COPY_AT), ui_trim_copy, sizeof(ui_trim_copy));
    kuKernelFlushCaches((void *)(code + UI_TRIM_COPY_AT), sizeof(ui_trim_copy));
    L_INFO("ui: BetterList Trim uses runtime Array.Copy (exact sizes and GC barriers preserved; future JIT)");
  } else L_WARN("ui: Trim IL/stack differs; original copy preserved");
  if (header) free_header(header);
}

// Preserve stable ordering without repeatedly scanning the entire list after
// a late icon changes its material/depth. Patch IL before the first UI JIT.
static void patch_ui_sort_il(void) {
  FILE *marker = fopen(DATA_PATH "/original_ui_sort", "rb");
  if (marker) { fclose(marker); L_INFO("ui: original BetterList sort requested"); return; }
  so_module *mono = so_find_module("libmono.so");
  const char *(*guid)(MonoImage *) = (void *)so_symbol(mono, "mono_image_get_guid");
  MonoMethod *(*get_method)(MonoImage *, uint32_t, MonoClass *) = (void *)so_symbol(mono, "mono_get_method");
  void *(*get_header)(MonoMethod *) = (void *)so_symbol(mono, "mono_method_get_header");
  const uint8_t *(*get_code)(void *, uint32_t *, uint32_t *) = (void *)so_symbol(mono, "mono_method_header_get_code");
  void (*free_header)(void *) = (void *)so_symbol(mono, "mono_metadata_free_mh");
  const char *id = guid ? guid(game_image) : NULL;
  if (!id || strcasecmp(id, RACE_WARMUP_MVID) || !get_method || !get_header || !get_code || !free_header) {
    L_WARN("ui: Sort assembly/API differs; original sort preserved"); return;
  }
  MonoMethod *method = get_method(game_image, 0x060001f6, NULL);
  void *header = method ? get_header(method) : NULL;
  uint32_t size = 0, stack = 0;
  const uint8_t *code = header ? get_code(header, &size, &stack) : NULL;
  if (code && stack >= 11 && size == sizeof(ui_sort_original) && !memcmp(code, ui_sort_original, size)) {
    kuKernelCpuUnrestrictedMemcpy((void *)code, ui_sort_adjacent, sizeof(ui_sort_adjacent));
    kuKernelFlushCaches((void *)code, sizeof(ui_sort_adjacent));
    L_INFO("ui: BetterList stable adjacent sort installed (no full-list bubble passes; future JIT)");
  } else L_WARN("ui: Sort IL/stack differs; original sort preserved");
  if (header) free_header(header);
}

// ReCreateTerrain starts this coroutine again during a run. Its final
// instruction forces a full collection even with ample free memory. Keep
// all terrain creation/return-to-pool and completion state; automatic GC
// and the initial LoadSetTableFirst / scene UnloadUnusedAssets still run.
static void patch_terrain_gc_il(void) {
  so_module *mono = so_find_module("libmono.so");
  const char *(*guid)(MonoImage *) = (void *)so_symbol(mono, "mono_image_get_guid");
  MonoMethod *(*get_method)(MonoImage *, uint32_t, MonoClass *) = (void *)so_symbol(mono, "mono_get_method");
  void *(*get_header)(MonoMethod *) = (void *)so_symbol(mono, "mono_method_get_header");
  const uint8_t *(*get_code)(void *, uint32_t *, uint32_t *) = (void *)so_symbol(mono, "mono_method_header_get_code");
  void (*free_header)(void *) = (void *)so_symbol(mono, "mono_metadata_free_mh");
  const char *id = guid ? guid(game_image) : NULL;
  if (!id || strcasecmp(id, RACE_WARMUP_MVID) || !get_method || !get_header || !get_code || !free_header) {
    L_WARN("stage: CreateTerrain assembly/API differs; terminal GC left original");
    return;
  }
  MonoMethod *method = get_method(game_image, 0x06004c98, NULL);
  void *header = method ? get_header(method) : NULL;
  uint32_t size = 0, stack = 0;
  const uint8_t *code = header ? get_code(header, &size, &stack) : NULL;
  if (code && size == sizeof(terrain_create_il) && !memcmp(code, terrain_create_il, size)) {
    static const uint8_t nops[5];
    kuKernelCpuUnrestrictedMemcpy((void *)(code + 0x2c9), nops, sizeof(nops));
    kuKernelFlushCaches((void *)(code + 0x2c9), sizeof(nops));
    L_INFO("stage: CreateTerrain terminal forced GC removed (automatic and scene cleanup preserved)");
  } else L_WARN("stage: CreateTerrain IL differs; terminal GC left original");
  if (header) free_header(header);
}

// These finally blocks box a List<SpawnableInfo>.Enumerator just to dispose
// the COPY. Its Dispose only clears the copy's list reference (mscorlib IL
// 02 14 7d 5f01000a 2a); it neither releases resources nor changes the list.
// The same methods also construct and immediately discard an empty List
// (newobj/pop), without ever assigning it. Its parameterless ctor only sets
// the new instance's backing array. Remove these dead allocations too.
// Preserve every branch, list mutation, spawn/despawn and EH boundary.
static void patch_spawn_il(void) {
  so_module *mono = so_find_module("libmono.so");
  void *(*get_header)(MonoMethod *) = (void *)so_symbol(mono, "mono_method_get_header");
  const uint8_t *(*get_code)(void *, uint32_t *, uint32_t *) = (void *)so_symbol(mono, "mono_method_header_get_code");
  void (*free_header)(void *) = (void *)so_symbol(mono, "mono_metadata_free_mh");
  if (!get_header || !get_code || !free_header) return;
  static const uint8_t CheckRangeIn[] = {0x14,0x0a,0x02,0x7b,0xb0,0x23,0x00,0x04,0x6f,0x7f,0x0d,0x00,0x0a,0x0c,0x38,0x82,0x00,0x00,0x00,0x12,0x02,0x28,0x80,0x0d,0x00,0x0a,0x0b,0x07,0x6f,0xdc,0x3b,0x00,0x06,0x3a,0x0b,0x00,0x00,0x00,0x07,0x6f,0xdd,0x3b,0x00,0x06,0x39,0x05,0x00,0x00,0x00,0x38,0x5f,0x00,0x00,0x00,0x07,0x7c,0x06,0x32,0x00,0x04,0x7b,0x38,0x00,0x00,0x0a,0x0d,0x09,0x03,0x59,0x13,0x04,0x09,0x03,0x59,0x07,0x7b,0x08,0x32,0x00,0x04,0x6f,0x0f,0x3c,0x00,0x06,0x41,0x25,0x00,0x00,0x00,0x02,0x07,0x28,0x1e,0x29,0x00,0x06,0x3a,0x19,0x00,0x00,0x00,0x06,0x3a,0x06,0x00,0x00,0x00,0x73,0x83,0x0d,0x00,0x0a,0x26,0x06,0x39,0x07,0x00,0x00,0x00,0x06,0x07,0x6f,0x8b,0x0d,0x00,0x0a,0x11,0x04,0x22,0x00,0x00,0x48,0x43,0x3d,0x05,0x00,0x00,0x00,0x38,0x05,0x00,0x00,0x00,0x38,0x0c,0x00,0x00,0x00,0x12,0x02,0x28,0x81,0x0d,0x00,0x0a,0x3a,0x72,0xff,0xff,0xff,0xdd,0x0c,0x00,0x00,0x00,0x08,0x8c,0xc4,0x01,0x00,0x1b,0x6f,0xa2,0x00,0x00,0x0a,0xdc,0x06,0x3a,0x01,0x00,0x00,0x00,0x2a,0x06,0x6f,0x7f,0x0d,0x00,0x0a,0x13,0x06,0x38,0x17,0x00,0x00,0x00,0x12,0x06,0x28,0x80,0x0d,0x00,0x0a,0x13,0x05,0x02,0x7b,0xb0,0x23,0x00,0x04,0x11,0x05,0x6f,0x8f,0x0d,0x00,0x0a,0x26,0x12,0x06,0x28,0x81,0x0d,0x00,0x0a,0x3a,0xdd,0xff,0xff,0xff,0xdd,0x0d,0x00,0x00,0x00,0x11,0x06,0x8c,0xc4,0x01,0x00,0x1b,0x6f,0xa2,0x00,0x00,0x0a,0xdc,0x2a};
  static const uint8_t CheckRangeOut[] = {0x14,0x0a,0x02,0x7b,0xb0,0x23,0x00,0x04,0x6f,0x7f,0x0d,0x00,0x0a,0x0c,0x38,0x72,0x00,0x00,0x00,0x12,0x02,0x28,0x80,0x0d,0x00,0x0a,0x0b,0x07,0x6f,0xdc,0x3b,0x00,0x06,0x39,0x0b,0x00,0x00,0x00,0x07,0x6f,0xde,0x3b,0x00,0x06,0x39,0x05,0x00,0x00,0x00,0x38,0x4f,0x00,0x00,0x00,0x03,0x07,0x7c,0x06,0x32,0x00,0x04,0x7b,0x38,0x00,0x00,0x0a,0x59,0x0d,0x09,0x22,0x00,0x00,0x00,0x00,0x41,0x05,0x00,0x00,0x00,0x38,0x3d,0x00,0x00,0x00,0x09,0x07,0x7b,0x08,0x32,0x00,0x04,0x6f,0x11,0x3c,0x00,0x06,0x43,0x20,0x00,0x00,0x00,0x06,0x3a,0x06,0x00,0x00,0x00,0x73,0x83,0x0d,0x00,0x0a,0x26,0x06,0x39,0x07,0x00,0x00,0x00,0x06,0x07,0x6f,0x8b,0x0d,0x00,0x0a,0x02,0x07,0x28,0x20,0x29,0x00,0x06,0x12,0x02,0x28,0x81,0x0d,0x00,0x0a,0x3a,0x82,0xff,0xff,0xff,0xdd,0x0c,0x00,0x00,0x00,0x08,0x8c,0xc4,0x01,0x00,0x1b,0x6f,0xa2,0x00,0x00,0x0a,0xdc,0x06,0x3a,0x01,0x00,0x00,0x00,0x2a,0x06,0x6f,0x7f,0x0d,0x00,0x0a,0x13,0x05,0x38,0x17,0x00,0x00,0x00,0x12,0x05,0x28,0x80,0x0d,0x00,0x0a,0x13,0x04,0x02,0x7b,0xb0,0x23,0x00,0x04,0x11,0x04,0x6f,0x8f,0x0d,0x00,0x0a,0x26,0x12,0x05,0x28,0x81,0x0d,0x00,0x0a,0x3a,0xdd,0xff,0xff,0xff,0xdd,0x0d,0x00,0x00,0x00,0x11,0x05,0x8c,0xc4,0x01,0x00,0x1b,0x6f,0xa2,0x00,0x00,0x0a,0xdc,0x2a};
  const struct { const char *name; const uint8_t *original; uint32_t size, offset[3], length[3]; } targets[] = {
    {"CheckRangeIn", CheckRangeIn, sizeof(CheckRangeIn), {0x6c, 0xa6, 0xee}, {6, 11, 12}},
    {"CheckRangeOut", CheckRangeOut, sizeof(CheckRangeOut), {0x6b, 0x96, 0xde}, {6, 11, 12}}
  };
  static const uint8_t nops[12] = {0};
  for (unsigned i = 0; i < 2; i++) {
    MonoClass *klass = api.class_from_name(game_image, "", "ObjectSpawnManager");
    MonoMethod *method = klass ? api.get_method(klass, targets[i].name, 1) : NULL;
    void *header = method ? get_header(method) : NULL;
    uint32_t size = 0, stack = 0;
    const uint8_t *code = header ? get_code(header, &size, &stack) : NULL;
    if (code && size == targets[i].size && !memcmp(code, targets[i].original, size)) {
      for (unsigned j = 0; j < 3; j++) {
        kuKernelCpuUnrestrictedMemcpy((void *)(code + targets[i].offset[j]), nops, targets[i].length[j]);
        kuKernelFlushCaches((void *)(code + targets[i].offset[j]), targets[i].length[j]);
      }
      L_INFO("perf: ObjectSpawnManager.%s removed boxed disposal and discarded empty lists", targets[i].name);
    } else L_WARN("perf: ObjectSpawnManager.%s IL differs; preserved", targets[i].name);
    if (header) free_header(header);
  }
}

// Avoid per-effect debug allocations; keep all transition collections intact.
static void patch_perf_il(void) {
  so_module *mono = so_find_module("libmono.so");
  void *(*get_header)(MonoMethod *) = (void *)so_symbol(mono, "mono_method_get_header");
  const uint8_t *(*get_code)(void *, uint32_t *, uint32_t *) =
    (void *)so_symbol(mono, "mono_method_header_get_code");
  void (*free_header)(void *) = (void *)so_symbol(mono, "mono_metadata_free_mh");
  if (!get_header || !get_code || !free_header) return;
  static const uint8_t se_original[] = {0x7e,0xcd,0x31,0x00,0x04,0x14,0x28,0x28,0x00,0x00,0x0a,0x39,0x02,0x00,0x00,0x00,0x16,0x2a,0x1b,0x8d,0x09,0x00,0x00,0x01,0x25,0x16,0x72,0x48,0x50,0x03,0x70,0xa2,0x25,0x17,0x02,0xa2,0x25,0x18,0x72,0x4b,0xe2,0x02,0x70,0xa2,0x25,0x19,0x03,0xa2,0x25,0x1a,0x72,0x38,0x06,0x01,0x70,0xa2,0x28,0x44,0x00,0x00,0x0a,0x28,0x92,0x3b,0x00,0x06,0x18,0x02,0x03,0x16,0x28,0x77,0x3b,0x00,0x06,0x2a};
  const struct { const char *klass, *method; int nargs; uint32_t size, offset, length; } targets[] = {
    {"SoundManager", "SePlay", 2, 0x4c, 0x12, 0x30}
  };
  static const uint8_t nops[0x30] = {0};
  for (unsigned i = 0; i < sizeof(targets) / sizeof(targets[0]); i++) {
    MonoClass *klass = api.class_from_name(game_image, "", targets[i].klass);
    MonoMethod *method = klass ? api.get_method(klass, targets[i].method, targets[i].nargs) : NULL;
    void *header = method ? get_header(method) : NULL;
    uint32_t size = 0, stack = 0;
    const uint8_t *code = header ? get_code(header, &size, &stack) : NULL;
    int matches = code && size == targets[i].size;
    if (matches) matches = memcmp(code, se_original, sizeof(se_original)) == 0;
    if (matches) {
      kuKernelCpuUnrestrictedMemcpy((void *)(code + targets[i].offset), nops, targets[i].length);
      kuKernelFlushCaches((void *)(code + targets[i].offset), targets[i].length);
      L_INFO("perf: %s.%s removed per-effect debug allocations (playback preserved)", targets[i].klass, targets[i].method);
    } else L_WARN("perf: %s.%s IL differs; left original", targets[i].klass, targets[i].method);
    if (header) free_header(header);
  }
}

// Called between frames before the menu is entered; no scene traversal.
// Avoid creating a boxed COPY solely to clear a reference inside that copy.
// The original List<T>.Enumerator.Dispose body must match before editing any
// caller. Nothing modifies the original enumerator/list, branches or EH ranges.
static void patch_hot_loop_il(void) {
  so_module *mono = so_find_module("libmono.so");
  MonoClass *(*nested)(MonoClass *, void **) = (void *)so_symbol(mono, "mono_class_get_nested_types");
  void *(*get_header)(MonoMethod *) = (void *)so_symbol(mono, "mono_method_get_header");
  const uint8_t *(*get_code)(void *, uint32_t *, uint32_t *) = (void *)so_symbol(mono, "mono_method_header_get_code");
  void (*free_header)(void *) = (void *)so_symbol(mono, "mono_metadata_free_mh");
  if (!nested || !get_header || !get_code || !free_header) return;
  MonoClass *list = api.class_from_name(api.get_corlib(), "System.Collections.Generic", "List`1");
  MonoClass *enumerator = NULL, *child; void *iter = NULL;
  if (list) while ((child = nested(list, &iter))) {
    if (!strcmp(api.class_get_name(child), "Enumerator")) { enumerator = child; break; }
  }
  MonoMethod *dispose = enumerator ? api.get_method(enumerator, "Dispose", 0) : NULL;
  void *header = dispose ? get_header(dispose) : NULL;
  uint32_t size = 0, stack = 0;
  const uint8_t *code = header ? get_code(header, &size, &stack) : NULL;
  static const uint8_t original_dispose[] = {0x02,0x14,0x7d,0x5f,0x01,0x00,0x0a,0x2a};
  int valid = code && size == sizeof(original_dispose) && !memcmp(code, original_dispose, size);
  if (header) free_header(header);
  if (!valid) { L_WARN("perf: List enumerator disposal differs; hot loops preserved"); return; }
  static const uint8_t nops[16] = {0}; unsigned methods = 0, boxes = 0;
  for (unsigned i = 0; i < sizeof(hot_loops)/sizeof(hot_loops[0]); i++) {
    const hot_loop_patch *p = &hot_loops[i];
    MonoClass *klass = api.class_from_name(game_image, p->ns, p->klass);
    MonoMethod *method = klass ? api.get_method(klass, p->method, 0) : NULL;
    header = method ? get_header(method) : NULL; size = 0;
    code = header ? get_code(header, &size, &stack) : NULL;
    if (code && size == p->size && !memcmp(code, p->original, size)) {
      for (unsigned j = 0; j < p->count; j++) {
        unsigned off = p->spans[j][0], len = p->spans[j][1];
        if (len > sizeof(nops) || off + len >= size || code[off + len] != 0xdc) continue;
        kuKernelCpuUnrestrictedMemcpy((void *)(code + off), nops, len);
        kuKernelFlushCaches((void *)(code + off), len); boxes++;
      }
      methods++;
    }
    if (header) free_header(header);
  }
  L_INFO("perf: allocation-free boxed disposal edits %u methods / %u sites (already-JITted methods retain original native code)", methods, boxes);
}

static void observe_mono_label(MonoObject *value, char *out, unsigned cap) {
  char *utf8 = value ? api.string_to_utf8(value) : NULL;
  snprintf(out, cap, "%s", utf8 ? utf8 : "(unnamed)");
  if (utf8) api.mono_free(utf8);
}

// The game requests 60 fps in menus and 30 fps in races with its LightMode.
// Apply the port menu's frame rate (default 60) in every scene, including
// after the game's own resets. Native icalls keep the poll allocation-free.
static void race_frame_rate_tick(void) {
  static int (*get_rate)(void);
  static void (*set_rate)(int);
  if (!get_rate || !set_rate) {
    so_module *mono = so_find_module("libmono.so"), *unity = so_find_module("libunity.so");
    void *(*lookup)(MonoMethod *) = (void *)so_symbol(mono, "mono_lookup_internal_call");
    MonoClass *klass = api.class_from_name(unity_image, "UnityEngine", "Application");
    MonoMethod *get = klass ? api.get_method(klass, "get_targetFrameRate", 0) : NULL;
    MonoMethod *set = klass ? api.get_method(klass, "set_targetFrameRate", 1) : NULL;
    void *a = lookup && get ? lookup(get) : NULL, *b = lookup && set ? lookup(set) : NULL;
    if (!a || !b || so_module_at((uintptr_t)a) != unity || so_module_at((uintptr_t)b) != unity) return;
    get_rate = a; set_rate = b;
    L_INFO("perf: frame rate target ready (quality settings preserved)");
  }
  int target = port_target_fps();
  int before = get_rate();
  if (before == target) return;
  set_rate(target);
  static unsigned changes;
  if (++changes <= 8 || (changes & (changes - 1)) == 0)
    L_INFO("perf: targetFrameRate %d -> %d (request, not measured fps)", before, get_rate());
}
// Native integer/boolean getters avoid managed allocations in the poll. The
// scene name is fetched only after its integer index changes.
void mono_observe_tick(void) {
  static int (*get_level)(void);
  static int (*get_touch_count)(void);
  static uint8_t (*is_loading)(void);
  static int previous_level = -2147483647;
  static char name[96] = "(initializing)";
  if (!unity_image) return;
  if (!get_level || !is_loading) {
    so_module *mono = so_find_module("libmono.so"), *unity = so_find_module("libunity.so");
    void *(*lookup)(MonoMethod *) = (void *)so_symbol(mono, "mono_lookup_internal_call");
    MonoClass *klass = api.class_from_name(unity_image, "UnityEngine", "Application");
    if (!lookup || !klass) return;
    MonoMethod *a = api.get_method(klass, "get_loadedLevel", 0), *b = api.get_method(klass, "get_isLoadingLevel", 0);
    void *fa = a ? lookup(a) : NULL, *fb = b ? lookup(b) : NULL;
    if (!fa || !fb || so_module_at((uintptr_t)fa) != unity || so_module_at((uintptr_t)fb) != unity) return;
    get_level = fa; is_loading = fb;
  }
  if (!get_touch_count) {
    so_module *mono = so_find_module("libmono.so"), *unity = so_find_module("libunity.so");
    void *(*lookup)(MonoMethod *) = (void *)so_symbol(mono, "mono_lookup_internal_call");
    MonoClass *klass = api.class_from_name(unity_image, "UnityEngine", "Input");
    MonoMethod *method = klass ? api.get_method(klass, "get_touchCount", 0) : NULL;
    void *fn = lookup && method ? lookup(method) : NULL;
    if (fn && so_module_at((uintptr_t)fn) == unity) get_touch_count = fn;
  }
  if (get_touch_count) {
    static int previous = -1;
    static unsigned polls;
    int touches = get_touch_count();
    if ((touches > 8 && touches != previous) || ++polls % 40 == 0)
      L_INFO("input: engine touchCount %d (bridge max 8)", touches);
    previous = touches;
  }
  int level = get_level();
  if (level != previous_level) {
    observe_mono_label(call("Application", "get_loadedLevelName", NULL, NULL, 0), name, sizeof(name));
    previous_level = level;
  }
  int loading = is_loading();
  if (!input_time_scale) {
    so_module *mono = so_find_module("libmono.so"), *unity = so_find_module("libunity.so");
    void *(*lookup)(MonoMethod *) = (void *)so_symbol(mono, "mono_lookup_internal_call");
    MonoClass *time = api.class_from_name(unity_image, "UnityEngine", "Time");
    MonoMethod *get = time ? api.get_method(time, "get_timeScale", 0) : NULL;
    void *fn = get && lookup ? lookup(get) : NULL;
    if (fn && so_module_at((uintptr_t)fn) == unity) {
      input_time_scale = (input_time_getter)fn;
      L_INFO("input: native race/pause gate ready for item and character buttons");
    } else {
      static int warned;
      if (!warned++) L_WARN("input: Time.get_timeScale unavailable; item/change touch shortcuts disabled");
    }
  }
  input_race_scene = !loading && !strcmp(name, "s_playingstage");
  mono_input_tick();
  managed_trace_menu_mode(!loading && !strcmp(name, "MainMenu"));
  managed_trace_focus(!loading && !strcmp(name, "s_playingstage") ? ui_panel_late_method : NULL);
  if (!loading) race_frame_rate_tick();
  observe_phase(name, loading);
}

// Profile asset creation at the internal-call boundary. Keep arguments,
// return values and the native function intact; no scene enumeration or
// managed allocation is added. These times can overlap GC and file I/O.
static MonoObject *(*clone_native)(MonoObject *);
static MonoObject *(*instantiate_native)(MonoObject *, void *, void *);
static MonoObject *(*resource_load_native)(MonoObject *, MonoObject *);
static MonoObject *(*www_texture_native)(MonoObject *, uint8_t);
static MonoObject *(*unload_native)(void);
static MonoObject *traced_clone(MonoObject *object) {
  char observed_name[128];
  snprintf(observed_name, sizeof(observed_name), "clone object=%p", (void *)object);
  uint64_t observed_start = observe_now();
  uint64_t start = work_trace_begin();
  MonoObject *result = clone_native(object);
  work_trace_end(WORK_CLONE, start, 0, (uintptr_t)__builtin_return_address(0));
  observe_span(OBS_CLONE, observed_name, observed_start, 0, result ? 1 : 0, (uintptr_t)object);
  return result;
}
static MonoObject *traced_instantiate(MonoObject *object, void *position, void *rotation) {
  char observed_name[128];
  snprintf(observed_name, sizeof(observed_name), "instantiate object=%p", (void *)object);
  uint64_t observed_start = observe_now();
  uint64_t start = work_trace_begin();
  MonoObject *result = instantiate_native(object, position, rotation);
  work_trace_end(WORK_CLONE, start, 0, (uintptr_t)__builtin_return_address(0));
  observe_span(OBS_CLONE, observed_name, observed_start, 0, result ? 1 : 0, (uintptr_t)object);
  return result;
}
static MonoObject *traced_resource_load(MonoObject *path, MonoObject *type) {
  char observed_name[128];
  observe_mono_label(path, observed_name, sizeof(observed_name));
  uint64_t observed_start = observe_now();
  uint64_t start = work_trace_begin();
  MonoObject *result = resource_load_native(path, type);
  work_trace_end(WORK_RESOURCE, start, 0, (uintptr_t)__builtin_return_address(0));
  observe_span(OBS_RESOURCE, observed_name, observed_start, 0, result ? 1 : 0, (uintptr_t)result);
  return result;
}
static MonoObject *traced_www_texture(MonoObject *self, uint8_t non_readable) {
  char observed_name[128];
  snprintf(observed_name, sizeof(observed_name), "www-texture object=%p", (void *)self);
  uint64_t observed_start = observe_now();
  uint64_t start = work_trace_begin();
  MonoObject *result = www_texture_native(self, non_readable);
  work_trace_end(WORK_IMAGE, start, 0, (uintptr_t)__builtin_return_address(0));
  observe_span(OBS_IMAGE, observed_name, observed_start, 0, result ? 1 : 0, (uintptr_t)self);
  return result;
}
static MonoObject *traced_unload(void) {
  char observed_name[128];
  snprintf(observed_name, sizeof(observed_name), "unload-unused object=%p", (void *)NULL);
  uint64_t observed_start = observe_now();
  uint64_t start = work_trace_begin();
  MonoObject *result = unload_native();
  work_trace_end(WORK_UNLOAD, start, 0, (uintptr_t)__builtin_return_address(0));
  observe_span(OBS_UNLOAD, observed_name, observed_start, 0, result ? 1 : 0, (uintptr_t)NULL);
  return result;
}
static MonoObject *(*bundle_load_native)(MonoObject *, MonoObject *, MonoObject *);
static MonoObject *(*bundle_async_native)(MonoObject *, MonoObject *, MonoObject *);
static MonoObject *(*scene_load_native)(MonoObject *, int, uint8_t, uint8_t);

// Compile ahead of the first race on Unity's main thread, after all IL
// optimizations are installed. mono_compile_method creates native code; it
// does not invoke the gameplay methods. No background Mono/Unity calls and
// no constructed enemies, rewards, effects or scene objects are needed.
static void race_jit_warmup(void) {
  static MonoDomain *prepared_domain;
  static unsigned next;
  static int disabled;
  if (disabled || !game_image) return;
  MonoDomain *domain = api.domain_get();
  if (!domain) return;
  if (prepared_domain != domain) { prepared_domain = domain; next = 0; }
  const unsigned count = sizeof(race_warmup_methods) / sizeof(*race_warmup_methods);
  if (next == count) return;
  so_module *mono = so_find_module("libmono.so");
  const char *(*guid)(MonoImage *) = (void *)so_symbol(mono, "mono_image_get_guid");
  MonoMethod *(*get_method)(MonoImage *, uint32_t, MonoClass *) = (void *)so_symbol(mono, "mono_get_method");
  void *(*get_header)(MonoMethod *) = (void *)so_symbol(mono, "mono_method_get_header");
  const uint8_t *(*get_code)(void *, uint32_t *, uint32_t *) = (void *)so_symbol(mono, "mono_method_header_get_code");
  void (*free_header)(void *) = (void *)so_symbol(mono, "mono_metadata_free_mh");
  const char *id = guid ? guid(game_image) : NULL;
  if (!get_method || !get_header || !get_code || !free_header || !id || strcasecmp(id, RACE_WARMUP_MVID)) {
    disabled = 1;
    L_WARN("jit: race precompile unavailable or assembly MVID differs; first-call JIT preserved");
    return;
  }
  uint64_t start = sceKernelGetProcessTimeWide();
  unsigned compiled = 0, skipped = 0, failed = 0;
  L_INFO("jit: race precompile begin %u/%u (loading only)", next, count);
  while (next < count) {
    // Warming cold code consumes native memory. Leave the same 64 MiB
    // reserve used by GC pacing for Unity's subsequent asset allocations.
    if ((next & 31) == 0 && native_heap_available().total_free < 64u * 1024 * 1024) break;
    MonoMethod *method = get_method(game_image, race_warmup_methods[next].token, NULL);
    void *header = method ? get_header(method) : NULL;
    uint32_t size = 0, stack = 0;
    const uint8_t *code = header ? get_code(header, &size, &stack) : NULL;
    int valid = code && size == race_warmup_methods[next].il_size;
    if (header) free_header(header);
    if (valid) {
      if (api.compile_method(method)) compiled++; else failed++;
    } else skipped++;
    next++;
    if ((next & 255) == 0) L_INFO("jit: race precompile progress %u/%u", next, count);
  }
  L_INFO("jit: race precompile %s %u/%u compiled %u skipped %u failed %u in %.2f ms native free %u KiB",
         next == count ? "complete" : "memory-limited", next, count, compiled, skipped, failed,
         (sceKernelGetProcessTimeWide() - start) / 1000.0,
         (unsigned)(native_heap_available().total_free / 1024));
}
static MonoObject *traced_bundle_load(MonoObject *self, MonoObject *path, MonoObject *type) {
  char name[128]; observe_mono_label(path, name, sizeof(name));
  uint64_t start = observe_now();
  MonoObject *result = bundle_load_native(self, path, type);
  observe_span(OBS_BUNDLE, name, start, 0, result ? 1 : 0, (uintptr_t)result);
  return result;
}
static MonoObject *traced_bundle_async(MonoObject *self, MonoObject *path, MonoObject *type) {
  char label[96], name[128]; observe_mono_label(path, label, sizeof(label));
  snprintf(name, sizeof(name), "async-submit:%s", label);
  uint64_t start = observe_now();
  MonoObject *result = bundle_async_native(self, path, type);
  observe_span(OBS_BUNDLE, name, start, 0, result ? 1 : 0, (uintptr_t)result);
  return result;
}
static MonoObject *traced_scene_load(MonoObject *path, int index, uint8_t additive, uint8_t complete) {
  char label[80], name[128]; observe_mono_label(path, label, sizeof(label));
  if (!additive) { input_race_scene = 0; input_set_race_controls(0); }
  if (!additive && !strcmp(label, "s_playingstage")) race_jit_warmup();
  snprintf(name, sizeof(name), "request:%s index=%d add=%u sync=%u", label, index, additive, complete);
  uint64_t start = observe_now();
  MonoObject *result = scene_load_native(path, index, additive, complete);
  observe_span(OBS_SCENE, name, start, 0, result ? 1 : 0, (uintptr_t)result);
  return result;
}
static void work_hooks_init(void) {
  static int done;
  if (done || !unity_image) return;
  so_module *mono = so_find_module("libmono.so"), *unity = so_find_module("libunity.so");
  void *(*lookup)(MonoMethod *) = (void *)so_symbol(mono, "mono_lookup_internal_call");
  void (*add)(const char *, const void *) = (void *)so_symbol(mono, "mono_add_internal_call");
  if (!lookup || !add || !unity) return;
  const struct { const char *klass, *method, *icall; int nargs; void **original; void *wrapper; } targets[] = {
    {"Object", "Internal_CloneSingle", "UnityEngine.Object::Internal_CloneSingle", 1, (void **)&clone_native, traced_clone},
    {"Object", "INTERNAL_CALL_Internal_InstantiateSingle", "UnityEngine.Object::INTERNAL_CALL_Internal_InstantiateSingle", 3, (void **)&instantiate_native, traced_instantiate},
    {"Resources", "Load", "UnityEngine.Resources::Load", 2, (void **)&resource_load_native, traced_resource_load},
    {"WWW", "GetTexture", "UnityEngine.WWW::GetTexture", 1, (void **)&www_texture_native, traced_www_texture},
    {"Resources", "UnloadUnusedAssets", "UnityEngine.Resources::UnloadUnusedAssets", 0, (void **)&unload_native, traced_unload},
    {"AssetBundle", "Load", "UnityEngine.AssetBundle::Load", 2, (void **)&bundle_load_native, traced_bundle_load},
    {"AssetBundle", "LoadAsync", "UnityEngine.AssetBundle::LoadAsync", 2, (void **)&bundle_async_native, traced_bundle_async},
    {"Application", "LoadLevelAsync", "UnityEngine.Application::LoadLevelAsync", 4, (void **)&scene_load_native, traced_scene_load}
  };
  unsigned installed = 0;
  for (unsigned i = 0; i < sizeof(targets) / sizeof(targets[0]); i++) {
    MonoClass *klass = api.class_from_name(unity_image, "UnityEngine", targets[i].klass);
    MonoMethod *method = klass ? api.get_method(klass, targets[i].method, targets[i].nargs) : NULL;
    void *fn = method ? lookup(method) : NULL;
    if (!fn || so_module_at((uintptr_t)fn) != unity) continue;
    *targets[i].original = fn;
    add(targets[i].icall, targets[i].wrapper);
    if (lookup(method) == targets[i].wrapper) installed++;
  }
  done = 1;
  L_INFO("perf: asset creation internal-call timers installed %u/8 (coverage measured by call counts)", installed);
}

// Distinguish the engine's cache lookup from CSV/URL work and filesystem
// metadata. No cache decisions or returned booleans are changed.
static uint8_t (*cache_version_native)(MonoObject *, int);
static uint8_t traced_cache_version(MonoObject *url, int version) {
  uint64_t start = work_trace_begin();
  uint8_t result = cache_version_native(url, version);
  work_trace_end(WORK_CACHE, start, 0, (uintptr_t)__builtin_return_address(0));
  return result;
}
static void cache_timing_init(void) {
  if (cache_version_native || !unity_image) return;
  so_module *mono = so_find_module("libmono.so"), *unity = so_find_module("libunity.so");
  void *(*lookup)(MonoMethod *) = (void *)so_symbol(mono, "mono_lookup_internal_call");
  void (*add)(const char *, const void *) = (void *)so_symbol(mono, "mono_add_internal_call");
  MonoClass *klass = api.class_from_name(unity_image, "UnityEngine", "Caching");
  MonoMethod *method = klass ? api.get_method(klass, "IsVersionCached", 2) : NULL;
  void *fn = method && lookup ? lookup(method) : NULL;
  if (!fn || !add || so_module_at((uintptr_t)fn) != unity) return;
  cache_version_native = fn;
  add("UnityEngine.Caching::IsVersionCached", traced_cache_version);
  L_INFO("perf: cache lookup timing %s", lookup(method) == (void *)traced_cache_version ? "installed" : "unavailable");
}

void mono_menu_fix(void) {
  static int done;
  if (done) return;
  if (!unity_image && !probe_init()) return;
  texture_budget_init();
  work_hooks_init();
  game_image = api.image_loaded("Assembly-CSharp");
  if (!game_image) return;
  MonoClass *klass = api.class_from_name(game_image, "SaveData", "SystemData");
  MonoMethod *method = klass ? api.get_method(klass, "IsFacebookWindowOrg", 0) : NULL;
  if (!method) {
    L_WARN("menu: Facebook date guard unavailable: method not found");
    done = 1;
    return;
  }
  void *code = api.compile_method(method);
  if (!code) return;
  hook_addr((uintptr_t)code, (uintptr_t)facebook_window_safe);
  done = 1;
  L_INFO("menu: installed Facebook date guard");
  patch_stage_il();
  patch_terrain_gc_il();
  patch_perf_il();
  patch_spawn_il();
  patch_hot_loop_il();
  stage_parse_install(game_image);
  cache_timing_init();
  patch_ui_trim_il();
  patch_ui_sort_il();
  rijndael256_install();
  ui_geometry_install(game_image);
  buddy_ui_install(game_image);
  MonoClass *panel = api.class_from_name(game_image, "", "UIPanel");
  ui_panel_late_method = panel ? api.get_method(panel, "LateUpdate", 0) : NULL;
  if (ui_panel_late_method) L_INFO("perf: gameplay timing focuses on UIPanel; full sample every 32 frames and 120 frames after a >100ms render");
  L_INFO("mem: original menu image loaders restored (0.0.42 image patch disabled)");
  klass = api.class_from_name(game_image, "", "SoundManager");
  method = klass ? api.get_method(klass, "OutputPlayLog", 1) : NULL;
  code = method ? api.compile_method(method) : NULL;
  if (code) {
    hook_addr((uintptr_t)code, (uintptr_t)sound_play_log);
    L_INFO("sound: installed sampled playback diagnostics");
  }
}

// Boxed value types keep their value right after the MonoObject header.
static int unbox_int(MonoObject *o) { return o ? *(int32_t *)((char *)o + 8) : -1; }
static float unbox_float(MonoObject *o) { return o ? *(float *)((char *)o + 8) : -1.0f; }
static int array_length(MonoObject *a) { return a ? *(int32_t *)((char *)a + 12) : -1; }
static MonoObject *array_at(MonoObject *a, int i) { return ((MonoObject **)((char *)a + 16))[i]; }

static void string_of(MonoObject *s, char *out, size_t size) {
  if (!s) {
    strncpy(out, "(null)", size);
    return;
  }
  char *u = api.string_to_utf8(s);
  strncpy(out, u ? u : "", size - 1);
  out[size - 1] = 0;
  api.mono_free(u);
}

static MonoObject *instance_call(MonoObject *self, const char *name, void **args, int nargs) {
  if (!self) return NULL;
  MonoMethod *m = api.get_method(api.object_get_class(self), name, nargs);
  MonoObject *exc = NULL;
  MonoObject *result = m ? api.invoke(m, self, args, &exc) : NULL;
  return exc ? NULL : result;
}

static int instance_bool(MonoObject *self, const char *name) {
  MonoObject *value = instance_call(self, name, NULL, 0);
  return value ? *(uint8_t *)api.object_unbox(value) : -1;
}

static MonoObject *sound_atom_source(MonoObject *source) {
  if (!source) return NULL;
  MonoField *field = api.get_field(api.object_get_class(source), "m_atomSource");
  MonoObject *atom = NULL;
  if (field) api.field_value(source, field, &atom);
  return atom;
}

// SoundManager.Update parks its object at (-1000, -1000, 0), while the
// dynamically created CRI sources default to 3D positioning. The shared SE
// channel carries jump/ring/UI cues rather than individual world emitters.
// Remove distance attenuation for that channel; music and other sources keep
// their original settings.
void mono_audio_tick(void) {
  if (!game_image) return;
  int id = 2; // SoundManager's shared SE channel
  void *args[1] = {&id};
  MonoObject *source = call_in(game_image, "", "SoundManager", "GetSource", NULL, args, 1);
  MonoObject *atom = sound_atom_source(source);
  MonoObject *spatial = instance_call(atom, "get_use3dPositioning", NULL, 0);
  if (spatial && *(uint8_t *)api.object_unbox(spatial)) {
    uint8_t off = 0;
    void *set_args[1] = {&off};
    instance_call(atom, "set_use3dPositioning", set_args, 1);
    if (instance_bool(atom, "get_use3dPositioning") == 0)
      L_INFO("sound: shared SE channel set to 2D compatibility (BGM preserved)");
    else {
      static int warned;
      if (!warned++) L_WARN("sound: failed to disable shared SE distance attenuation");
    }
  }
}

static void sound_snapshot(void) {
  if (!game_image) return;
  MonoImage *cri = api.image_loaded("Assembly-CSharp-firstpass");
  L_INFO("sound: BGM volume %.3f SE volume %.3f requests %u",
         unbox_float(call_in(game_image, "", "SoundManager", "get_BgmVolume", NULL, NULL, 0)),
         unbox_float(call_in(game_image, "", "SoundManager", "get_SeVolume", NULL, NULL, 0)), sound_requests);
  for (int id = 0; id < 3; id++) {
    void *args[1] = {&id};
    MonoObject *source = call_in(game_image, "", "SoundManager", "GetSource", NULL, args, 1);
    MonoObject *atom = sound_atom_source(source);
    char cue[96];
    string_of(instance_call(atom, "get_cueName", NULL, 0), cue, sizeof(cue));
    MonoObject *time = instance_call(atom, "get_time", NULL, 0);
    L_INFO("sound: channel %d cue '%s' status %d time %lld ms volume %.3f 3D %d lowLatency %d", id, cue,
      unbox_int(instance_call(atom, "get_status", NULL, 0)),
      time ? (long long)*(int64_t *)api.object_unbox(time) : -1LL,
      unbox_float(instance_call(atom, "get_volume", NULL, 0)),
      instance_bool(atom, "get_use3dPositioning"),
      instance_bool(atom, "get_androidUseLowLatencyVoicePool"));
  }
  MonoObject *sheets = call_in(game_image, "", "SoundManager", "GetCueSheetNameList", NULL, NULL, 0);
  for (int i = 0; i < array_length(sheets) && i < 24; i++) {
    MonoObject *name = array_at(sheets, i);
    char text[96];
    string_of(name, text, sizeof(text));
    void *args[1] = {name};
    MonoObject *acb = cri ? call_in(cri, "", "CriAtom", "GetAcb", NULL, args, 1) : NULL;
    L_INFO("sound: bank '%s' registered 1 loaded %d", text, acb != NULL);
  }
}

// Read Mono's existing explicit-request statistic; do not detour the
// collector, skip requests, or replace Mono's profiler callbacks. Calls that
// bypass mono_gc_collect are deliberately not labeled automatic by subtraction.
static int gc_manual_requests(uint32_t *count) {
  static uintptr_t *slot;
  static uint32_t *counter;
  static uintptr_t checked_stats;
  static int rejected;
  if (rejected) return 0;
  if (!slot) {
    so_module *mono = so_find_module("libmono.so");
    static const uint32_t expected[] = {0xe92d4800,0xe28db004,0xe24dd008,0xe50b0008,0xe59f301c,0xe08f3003,0xe5933000,0xe593205c,0xe2822001,0xe583205c,0xeb05fc27,0xe24bd004,0xe8bd8800,0x00275e60};
    if (!mono) return 0;
    if (mono->end < mono->base + 0x3b4f74 ||
        so_symbol(mono, "mono_gc_collect") != mono->base + 0x13f0f4 ||
        memcmp((void *)(mono->base + 0x13f0f4), expected, sizeof(expected))) {
      rejected = 1;
      L_WARN("gc: explicit-request layout differs; attribution unavailable");
      return 0;
    }
    slot = (uintptr_t *)(mono->base + 0x3b4f70);
  }
  uintptr_t stats = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
  if (!stats || (stats & 3)) return 0;
  if (stats != checked_stats) {
    SceKernelMemBlockInfo info = {.size = sizeof(info)};
    if (sceKernelGetMemBlockInfoByRange((void *)stats, 0x60, &info) < 0) return 0;
    counter = (uint32_t *)(stats + 0x5c);
    checked_stats = stats;
    L_INFO("gc: explicit-request counter attached (read-only)");
  }
  *count = __atomic_load_n(counter, __ATOMIC_RELAXED);
  return 1;
}

static void jit_frame_reset(void) {
  jit_window_methods += jit_frame_methods;
  jit_window_us += jit_frame_us;
  if (jit_frame_longest_us > jit_window_longest_us) {
    jit_window_longest_us = jit_frame_longest_us;
    jit_window_longest = jit_frame_longest;
  }
  jit_frame_methods = 0;
  jit_frame_us = jit_frame_longest_us = 0;
  jit_frame_longest = NULL;
}

// Native counters do not allocate managed objects or force collections.
void mono_perf_tick(uint64_t render_us, uint64_t frame_us, unsigned frame) {
  static int previous_count, initialized;
  static unsigned collections, slow_with_gc, slow_without_gc, spikes;
  static uint32_t previous_manual, manual_total;
  static int manual_initialized;
  uint32_t manual = 0, manual_delta = 0;
  int manual_available = gc_manual_requests(&manual);
  if (manual_available) {
    if (manual_initialized) manual_delta = manual - previous_manual;
    previous_manual = manual;
    manual_initialized = 1;
    manual_total += manual_delta;
  } else manual_initialized = 0;
  static uint64_t worst_frame_us, worst_extra_us;
  static unsigned clone_calls, resource_calls, image_calls, unload_calls, managed_calls;
  static gc_trace_sample worst_gc;
  work_sample work;
  work_trace_take(&work);
  clone_calls += work.counter[WORK_CLONE].calls;
  resource_calls += work.counter[WORK_RESOURCE].calls;
  image_calls += work.counter[WORK_IMAGE].calls;
  unload_calls += work.counter[WORK_UNLOAD].calls;
  managed_calls += work.counter[WORK_MANAGED].calls;
  gl_perf_sample graphics;
  gl_perf_take(&graphics);
  gc_trace_sample samples[16];
  unsigned nsamples = gc_trace_read(samples);
  for (unsigned i = 0; i < nsamples; i++) {
    if (samples[i].total_us > worst_gc.total_us) worst_gc = samples[i];
    uint64_t start = ((uint64_t)samples[i].start_hi << 32) | samples[i].start_lo;
    char label[128];
    snprintf(label, sizeof(label), "stop_us=%u mark_us=%u reclaim_us=%u restart_us=%u", samples[i].stop_us,
             samples[i].mark_us, samples[i].reclaim_us, samples[i].restart_us);
    observe_span_at(OBS_GC, label, start, start + samples[i].total_us, 0, 0, 0);
  }
  if (frame_us > worst_frame_us) worst_frame_us = frame_us;
  uint64_t extra_us = frame_us >= render_us ? frame_us - render_us : 0;
  if (extra_us > worst_extra_us) worst_extra_us = extra_us;
  if (!api.gc_count) { observe_frame(render_us, frame_us, 0); jit_frame_reset(); return; }
  int count = api.gc_count(0);
  unsigned delta = initialized && count >= previous_count ? count - previous_count : 0;
  initialized = 1;
  previous_count = count;
  collections += delta;
  observe_frame(render_us, frame_us, delta > nsamples ? delta : nsamples);
  if (frame_us > 100000 && (++spikes <= 8 || (spikes & (spikes - 1)) == 0)) {
    L_INFO("perf: spike frame %u render %.2f ms outside render %.2f ms GC %u traces %u",
           frame, render_us / 1000.0, extra_us / 1000.0, delta, nsamples);
    L_INFO("perf: graphics draw %.2f texture %.2f present %.2f link %.2f ms",
           graphics.draw_us / 1000.0, graphics.texture_us / 1000.0,
           graphics.present_us / 1000.0, graphics.link_us / 1000.0);
    L_INFO("perf: explicit GC requests %u available %d", manual_delta, manual_available);
    if (jit_frame_methods) {
      char label[160];
      jit_method_label(jit_frame_longest, label, sizeof(label));
      L_INFO("perf: main jit %.2f ms methods %u longest %.2f ms %s", jit_frame_us / 1000.0, jit_frame_methods,
             jit_frame_longest_us / 1000.0, label);
    }
    for (unsigned k = 0; k < WORK_KINDS; k++) {
      work_counter *c = &work.counter[k];
      if (k == WORK_MANAGED) {
        char label[160];
        jit_method_label((MonoMethod *)c->caller, label, sizeof(label));
        L_INFO("perf: main managed-callbacks %.2f ms calls %u longest %.2f ms %s", c->us / 1000.0,
               c->calls, c->longest_us / 1000.0, label);
        continue;
      }
      so_module *m = so_module_at(c->caller);
      L_INFO("perf: main %s %.2f ms calls %u bytes %llu longest %.2f ms caller %s+0x%X",
             k == WORK_IO ? "file-io" : k == WORK_WAIT ? "wait" : k == WORK_CLONE ? "instantiate" : k == WORK_RESOURCE ? "resources-load" : k == WORK_IMAGE ? "www-texture" : k == WORK_META ? "filesystem-meta" : k == WORK_CACHE ? "cache-lookup" : "unload-unused", c->us / 1000.0, c->calls,
             (unsigned long long)c->bytes, c->longest_us / 1000.0,
             m ? m->name : "native", (unsigned)(m ? c->caller - m->base : c->caller));
    }
  }
  if (render_us > 50000) {
    if (delta) slow_with_gc++;
    else slow_without_gc++;
  }
  if (frame % 600 == 0) {
    L_INFO("perf: GC collections %u slow with GC %u without GC %u heap %llu used %llu KiB",
           collections, slow_with_gc, slow_without_gc,
           (unsigned long long)api.gc_heap() / 1024, (unsigned long long)api.gc_used() / 1024);
    L_INFO("perf: full frame max %.2f ms outside-render max %.2f ms; worst GC %.2f ms stop %.2f mark %.2f reclaim %.2f restart %.2f",
           worst_frame_us / 1000.0, worst_extra_us / 1000.0, worst_gc.total_us / 1000.0,
           worst_gc.stop_us / 1000.0, worst_gc.mark_us / 1000.0,
           worst_gc.reclaim_us / 1000.0, worst_gc.restart_us / 1000.0);
    L_INFO("gc: explicit requests in window %u available %d", manual_total, manual_available);
    L_INFO("perf: asset call coverage instantiate %u resources-load %u www-texture %u unload-unused %u", clone_calls, resource_calls, image_calls, unload_calls);
    L_INFO("perf: managed callback coverage %u outer calls", managed_calls);
    L_INFO("ui: buddy idle skips %u active forwards %u", managed_trace_idle_skipped(), managed_trace_idle_forwarded());
    L_INFO("ui: geometry bulk appends %u original fallbacks %u", ui_geometry_fast_calls(), ui_geometry_fallback_calls());
    L_INFO("stage: Rijndael256 native blocks %u original fallbacks %u", rijndael256_fast_calls(), rijndael256_fallback_calls());
    managed_calls = 0;
    clone_calls = resource_calls = image_calls = unload_calls = 0;
    manual_total = 0;
    native_heap_sample native = native_heap_available();
    L_INFO("gc: budget desired %u active %u conflict %d native available %u chunks %u reserved-tail %u KiB",
           gc_policy_desired(), gc_policy_applied(), gc_policy_conflict(),
           (unsigned)(native.total_free / 1024), (unsigned)(native.chunks_free / 1024), (unsigned)(native.unused_reserve / 1024));
    audio_diagnostics(); // native-only: measure clipping throughout gameplay
    if (jit_window_methods) {
      char label[160];
      jit_method_label(jit_window_longest, label, sizeof(label));
      L_INFO("jit: window main %u methods %.2f ms longest %.2f ms %s; other threads %u methods", jit_window_methods,
             jit_window_us / 1000.0, jit_window_longest_us / 1000.0, label,
             __atomic_exchange_n(&jit_other_methods, 0, __ATOMIC_RELAXED));
    }
    jit_window_methods = 0;
    jit_window_us = jit_window_longest_us = 0;
    jit_window_longest = NULL;
    collections = slow_with_gc = slow_without_gc = spikes = 0;
    worst_frame_us = worst_extra_us = 0;
    memset(&worst_gc, 0, sizeof(worst_gc));
  }
  jit_frame_reset();
}

static MonoObject *find_all(const char *cls) {
  MonoClass *k = api.class_from_name(unity_image, "UnityEngine", cls);
  if (!k) return NULL;
  void *type_arg[1] = {api.type_get_object(api.domain_get(), api.class_get_type(k))};
  return call("Object", "FindObjectsOfType", NULL, type_arg, 1);
}

// Boxed structs (Vector3, Rect, Bounds) are float fields after the header.
static float boxed_f(MonoObject *o, int i) { return o ? ((float *)((char *)o + 8))[i] : -9999.0f; }

static int as_bool(MonoObject *o) { return o ? *(uint8_t *)((char *)o + 8) : -1; }

static void dump_skins(void) {
  MonoObject *skins = find_all("SkinnedMeshRenderer");
  int count = array_length(skins);
  L_INFO("probe: skinned renderers %d", count);
  for (int i = 0; i < count && i < 32; ++i) {
    MonoObject *r = array_at(skins, i);
    char name[96];
    string_of(call("Object", "get_name", r, NULL, 0), name, sizeof(name));
    MonoObject *mesh = call("SkinnedMeshRenderer", "get_sharedMesh", r, NULL, 0);
    MonoObject *bones = call("SkinnedMeshRenderer", "get_bones", r, NULL, 0);
    MonoObject *mat = call("Renderer", "get_sharedMaterial", r, NULL, 0);
    MonoObject *shader = mat ? call("Material", "get_shader", mat, NULL, 0) : NULL;
    char shader_name[96];
    string_of(shader ? call("Object", "get_name", shader, NULL, 0) : NULL, shader_name, sizeof(shader_name));
    L_INFO("probe: skin '%s' enabled %d visible %d vertices %d bones %d shader '%s' supported %d", name,
      as_bool(call("Renderer", "get_enabled", r, NULL, 0)),
      as_bool(call("Renderer", "get_isVisible", r, NULL, 0)),
      mesh ? unbox_int(call("Mesh", "get_vertexCount", mesh, NULL, 0)) : -1,
      array_length(bones), shader_name,
      shader ? as_bool(call("Shader", "get_isSupported", shader, NULL, 0)) : -1);
    if (mat) {
      void *texture_args[1] = {api.string_new(api.domain_get(), "_DifTex")};
      if (as_bool(call("Material", "HasProperty", mat, texture_args, 1)) == 1) {
        MonoObject *tex = call("Material", "GetTexture", mat, texture_args, 1);
        char texture_name[96];
        string_of(tex ? call("Object", "get_name", tex, NULL, 0) : NULL, texture_name, sizeof(texture_name));
        L_INFO("probe: skin diffuse '%s' %dx%d", texture_name,
          tex ? unbox_int(call("Texture", "get_width", tex, NULL, 0)) : 0,
          tex ? unbox_int(call("Texture", "get_height", tex, NULL, 0)) : 0);
      }
    }
  }
  L_INFO("probe: audio listener volume %.3f paused %d sources %d",
    unbox_float(call("AudioListener", "get_volume", NULL, NULL, 0)),
    as_bool(call("AudioListener", "get_pause", NULL, NULL, 0)), array_length(find_all("AudioSource")));
}

static double unbox_double(MonoObject *o) { return o ? *(double *)((char *)o + 8) : -9999.0; }

// Float math through Mono's icalls (-> the loader's soft-float libm shims) and
// Unity's own float paths, against known answers.
static void math_selftest(MonoObject *cam) {
  MonoImage *corlib = api.get_corlib();
  double d1 = 2.0, d2 = 10.0, d3 = 1.0;
  void *a1[1] = {&d1}, *a2[2] = {&d1, &d2}, *a3[2] = {&d3, &d3};
  L_INFO("probe: Math.Sqrt(2)=%.6f Math.Sin(2)=%.6f Math.Cos(2)=%.6f Math.Pow(2,10)=%.3f Math.Atan2(1,1)=%.6f "
         "Math.Log(2)=%.6f Math.Exp(2)=%.6f",
         unbox_double(call_in(corlib, "System", "Math", "Sqrt", NULL, a1, 1)),
         unbox_double(call_in(corlib, "System", "Math", "Sin", NULL, a1, 1)),
         unbox_double(call_in(corlib, "System", "Math", "Cos", NULL, a1, 1)),
         unbox_double(call_in(corlib, "System", "Math", "Pow", NULL, a2, 2)),
         unbox_double(call_in(corlib, "System", "Math", "Atan2", NULL, a3, 2)),
         unbox_double(call_in(corlib, "System", "Math", "Log", NULL, a1, 1)),
         unbox_double(call_in(corlib, "System", "Math", "Exp", NULL, a1, 1)));
  float f1 = 1.7f, f2 = 2.0f;
  void *b1[1] = {&f1}, *b2[1] = {&f2};
  L_INFO("probe: Mathf.Floor(1.7)=%.3f Mathf.Round(1.7)=%.3f Mathf.Sqrt(2)=%.6f Mathf.Sin(2)=%.6f RoundToInt(1.7)=%d",
         unbox_float(call("Mathf", "Floor", NULL, b1, 1)), unbox_float(call("Mathf", "Round", NULL, b1, 1)),
         unbox_float(call("Mathf", "Sqrt", NULL, b2, 1)), unbox_float(call("Mathf", "Sin", NULL, b2, 1)),
         unbox_int(call("Mathf", "RoundToInt", NULL, b1, 1)));
  if (cam) {
    float vp[3] = {0.5f, 0.5f, 0.0f}, sp[3] = {480.0f, 272.0f, 0.0f};
    void *c1[1] = {vp}, *c2[1] = {sp};
    MonoObject *w1 = call("Camera", "ViewportToWorldPoint", cam, c1, 1);
    MonoObject *w2 = call("Camera", "ScreenToWorldPoint", cam, c2, 1);
    MonoObject *rect = call("Camera", "get_rect", cam, NULL, 0);
    L_INFO("probe: cam.ViewportToWorldPoint(.5,.5,0)=(%.3f, %.3f, %.3f) ScreenToWorldPoint(480,272,0)=(%.3f, %.3f, "
           "%.3f) rect (%.3f, %.3f, %.3f, %.3f)",
           boxed_f(w1, 0), boxed_f(w1, 1), boxed_f(w1, 2), boxed_f(w2, 0), boxed_f(w2, 1), boxed_f(w2, 2),
           boxed_f(rect, 0), boxed_f(rect, 1), boxed_f(rect, 2), boxed_f(rect, 3));
  }
}

void mono_probe(void) {
  if (probe_disabled)
    return;
  if (!unity_image && !probe_init()) {
    L_WARN("probe: UnityEngine image not available, probe disabled");
    probe_disabled = 1;
    return;
  }

  char level[64];
  string_of(call("Application", "get_loadedLevelName", NULL, NULL, 0), level, sizeof(level));
  int loading = unbox_int(call("Application", "get_isLoadingLevel", NULL, NULL, 0)) & 0xff;
  float scale = unbox_float(call("Time", "get_timeScale", NULL, NULL, 0));

  MonoClass *go = api.class_from_name(unity_image, "UnityEngine", "GameObject");
  void *type_arg[1] = {api.type_get_object(api.domain_get(), api.class_get_type(go))};
  int objects = array_length(call("Object", "FindObjectsOfType", NULL, type_arg, 1));

  MonoObject *cams = call("Camera", "get_allCameras", NULL, NULL, 0);
  int ncams = array_length(cams);
  L_INFO("probe: level '%s' loading %d timeScale %.2f gameObjects %d cameras %d screen %dx%d", level, loading,
         scale, objects, ncams, unbox_int(call("Screen", "get_width", NULL, NULL, 0)),
         unbox_int(call("Screen", "get_height", NULL, NULL, 0)));
  // Tweens and NGUI's RealTime move the title UI from these clocks.
  L_INFO("probe: Time.time %.3f deltaTime %.4f realtimeSinceStartup %.3f frameCount %d",
         unbox_float(call("Time", "get_time", NULL, NULL, 0)), unbox_float(call("Time", "get_deltaTime", NULL, NULL, 0)),
         unbox_float(call("Time", "get_realtimeSinceStartup", NULL, NULL, 0)),
         unbox_int(call("Time", "get_frameCount", NULL, NULL, 0)));
  static char previous_level[64];
  if (objects > 0 && strcmp(previous_level, level) != 0) {
    strncpy(previous_level, level, sizeof(previous_level));
    // Full scene enumeration produced hundreds of managed calls and log
    // writes during the same transitions that already stress the GC.
    // The focused skin/camera snapshot below is sufficient here.
  }
  dump_skins();
  audio_diagnostics();
  sound_snapshot();
  static int tested;
  if (!tested++)
    math_selftest(ncams > 0 ? array_at(cams, 0) : NULL);
  for (int i = 0; i < ncams && i < 8; i++) {
    MonoObject *cam = array_at(cams, i);
    char name[64];
    string_of(call("Object", "get_name", cam, NULL, 0), name, sizeof(name));
    int mask = unbox_int(call("Camera", "get_cullingMask", cam, NULL, 0));
    int clear = unbox_int(call("Camera", "get_clearFlags", cam, NULL, 0));
    float depth = unbox_float(call("Camera", "get_depth", cam, NULL, 0));
    int target = call("Camera", "get_targetTexture", cam, NULL, 0) != NULL;
    L_INFO("probe:   camera '%s' cullingMask 0x%X clearFlags %d depth %.1f renderTexture %d", name, mask, clear,
           depth, target);
    MonoObject *t = call("Component", "get_transform", cam, NULL, 0);
    MonoObject *pos = t ? call("Transform", "get_position", t, NULL, 0) : NULL;
    MonoObject *px = call("Camera", "get_pixelRect", cam, NULL, 0);
    L_INFO("probe:     enabled %d ortho %d size %.3f near %.3f far %.3f aspect %.3f fov %.1f pixelRect (%.0f,%.0f %.0fx%.0f) "
           "pos (%.3f, %.3f, %.3f)",
           as_bool(call("Behaviour", "get_enabled", cam, NULL, 0)), as_bool(call("Camera", "get_orthographic", cam, NULL, 0)),
           unbox_float(call("Camera", "get_orthographicSize", cam, NULL, 0)),
           unbox_float(call("Camera", "get_nearClipPlane", cam, NULL, 0)),
           unbox_float(call("Camera", "get_farClipPlane", cam, NULL, 0)),
           unbox_float(call("Camera", "get_aspect", cam, NULL, 0)), unbox_float(call("Camera", "get_fieldOfView", cam, NULL, 0)),
           boxed_f(px, 0), boxed_f(px, 1), boxed_f(px, 2), boxed_f(px, 3), boxed_f(pos, 0), boxed_f(pos, 1),
           boxed_f(pos, 2));
  }
}
// --- Port settings (src/port_menu.c) ----------------------------------------
// Runs on the Unity thread between frames.

// Keeps Unity on the quality level and texture size chosen in the port
// settings. The game's own SetQualityLevel calls (level 0 for Light Mode, 1
// for Normal, at the title screen) are redirected to that level by the
// texture budget wrapper; this also covers the level Unity starts with.
void mono_port_quality_apply(void) {
  static int (*quality_get)(void);
  quality_level_forced = port_unity_quality_level();
  if (!texture_budget_ready) texture_budget_init();
  if (texture_budget_ready != 1) return;
  if (!quality_get) {
    so_module *mono = so_find_module("libmono.so"), *unity = so_find_module("libunity.so");
    void *(*lookup)(MonoMethod *) = (void *)so_symbol(mono, "mono_lookup_internal_call");
    MonoClass *klass = api.class_from_name(unity_image, "UnityEngine", "QualitySettings");
    MonoMethod *get = klass ? api.get_method(klass, "GetQualityLevel", 0) : NULL;
    void *fn = lookup && get ? lookup(get) : NULL;
    if (!fn || so_module_at((uintptr_t)fn) != unity) return;
    quality_get = fn;
  }
  int level = port_unity_quality_level(), current = quality_get();
  if (current != level) {
    quality_level_with_texture_budget(level, 1);
    L_INFO("port: Unity quality level %d -> %d", current, quality_get());
  }
  int limit = port_texture_limit(), before = texture_limit_native_get();
  if (before != limit) {
    texture_limit_native_set(limit);
    bundle_mips_set_limit(texture_limit_native_get());
    L_INFO("port: masterTextureLimit %d -> %d", before, texture_limit_native_get());
  }
}

void mono_probe_tick(unsigned frame) {
  static char last_level[64];
  if (!unity_image && !probe_init()) return;
  char level[64];
  string_of(call("Application", "get_loadedLevelName", NULL, NULL, 0), level, sizeof(level));
  int changed = strcmp(last_level, level) != 0;
  // A full snapshot enumerates scene objects and invokes dozens of managed
  // methods. Keep only the transition snapshot during active gameplay.
  int playing = strcmp(level, "s_playingstage") == 0;
  gc_playing_scene = playing;
  if (changed || (!playing && frame == 900)) {
    if (strcmp(last_level, level) != 0) sound_requests = 0;
    strncpy(last_level, level, sizeof(last_level));
    mono_probe();
  }
}

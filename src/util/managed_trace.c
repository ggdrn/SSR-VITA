// Time Unity -> Mono callback boundaries without enabling enter/leave or
// allocation profiling in generated managed code. Names are read later,
// only when reporting a slow frame; the wrapper never allocates or logs.
#include "managed_trace.h"
#include "work_trace.h"
#include "log.h"
#include "../loader/so_util.h"

static void *(*invoke_original)(void *, void *, void **, void **);
static void *(*array_original)(void *, void *, void *, void **);
// Only the work_trace owner (render thread) touches depth. Worker calls
// immediately forward, including when nested inside a main-thread callback.
static unsigned depth;
static void *focused_method;
static void *idle_method;
static int (*idle_filter)(void *);
static unsigned idle_skipped, idle_forwarded;
static void *menu_methods[4];
static int menu_ready, menu_mode, full_sample = 1;
// Only the render thread schedules capture. Callback readers use full_sample.
static unsigned recovery_frames;
void managed_trace_focus(void *method) {
  __atomic_store_n(&focused_method, method, __ATOMIC_RELAXED);
  if (!method) recovery_frames = 0;
}
void managed_trace_idle_method(void *method, int (*idle)(void *)) {
  if (!method || !idle || __atomic_load_n(&idle_method, __ATOMIC_ACQUIRE)) return;
  idle_filter = idle;
  __atomic_store_n(&idle_method, method, __ATOMIC_RELEASE);
}
void managed_trace_menu_methods(void *panel, void *cell, void *camera, void *init) {
  if (!panel || !cell || !camera || !init || __atomic_load_n(&menu_ready, __ATOMIC_ACQUIRE)) return;
  menu_methods[0] = panel; menu_methods[1] = cell; menu_methods[2] = camera; menu_methods[3] = init;
  __atomic_store_n(&menu_ready, 1, __ATOMIC_RELEASE);
}
void managed_trace_menu_mode(int enabled) {
  __atomic_store_n(&menu_mode, enabled != 0, __ATOMIC_RELAXED);
}
void managed_trace_frame_begin(unsigned frame) {
  int recovery = recovery_frames != 0;
  if (recovery) recovery_frames--;
  __atomic_store_n(&full_sample, recovery || (frame & 31u) == 0, __ATOMIC_RELAXED);
}
void managed_trace_frame_end(unsigned long long render_us) {
  if (__atomic_load_n(&focused_method, __ATOMIC_RELAXED) && render_us > 100000)
    recovery_frames = 120;
}
unsigned managed_trace_idle_skipped(void) { return __atomic_load_n(&idle_skipped, __ATOMIC_RELAXED); }
unsigned managed_trace_idle_forwarded(void) { return __atomic_load_n(&idle_forwarded, __ATOMIC_RELAXED); }
static int skip_idle(void *method, void *self, void **exception) {
  if (method != __atomic_load_n(&idle_method, __ATOMIC_ACQUIRE) || !idle_filter) return 0;
  if (!idle_filter(self)) {
    __atomic_fetch_add(&idle_forwarded, 1, __ATOMIC_RELAXED);
    return 0;
  }
  __atomic_fetch_add(&idle_skipped, 1, __ATOMIC_RELAXED);
  if (exception) *exception = NULL; // void Update returns null, successful invoke clears the exception
  return 1;
}
static int measure_method(void *method) {
  void *focus = __atomic_load_n(&focused_method, __ATOMIC_RELAXED);
  if (focus) return method == focus || __atomic_load_n(&full_sample, __ATOMIC_RELAXED);
  if (!__atomic_load_n(&menu_ready, __ATOMIC_ACQUIRE) || !__atomic_load_n(&menu_mode, __ATOMIC_RELAXED) ||
      __atomic_load_n(&full_sample, __ATOMIC_RELAXED)) return 1;
  return method == menu_methods[0] || method == menu_methods[1] || method == menu_methods[2] || method == menu_methods[3];
}
static void *traced_invoke(void *method, void *self, void **args, void **exception) {
  if (skip_idle(method, self, exception)) return NULL;
  if (!measure_method(method)) return invoke_original(method, self, args, exception);
  uint64_t start = work_trace_begin();
  if (!start) return invoke_original(method, self, args, exception);
  int outer = depth++ == 0;
  void *result = invoke_original(method, self, args, exception);
  depth--;
  if (outer) work_trace_end(WORK_MANAGED, start, 0, (uintptr_t)method);
  return result;
}
static void *traced_array(void *method, void *self, void *args, void **exception) {
  if (skip_idle(method, self, exception)) return NULL;
  if (!measure_method(method)) return array_original(method, self, args, exception);
  uint64_t start = work_trace_begin();
  if (!start) return array_original(method, self, args, exception);
  int outer = depth++ == 0;
  void *result = array_original(method, self, args, exception);
  depth--;
  if (outer) work_trace_end(WORK_MANAGED, start, 0, (uintptr_t)method);
  return result;
}
void managed_trace_install(void) {
  so_module *mono = so_find_module("libmono.so"), *unity = so_find_module("libunity.so");
  if (!mono || !unity) return;
  invoke_original = (void *)so_symbol(mono, "mono_runtime_invoke");
  array_original = (void *)so_symbol(mono, "mono_runtime_invoke_array");
  if (!invoke_original || !array_original ||
      so_module_at((uintptr_t)invoke_original) != mono || so_module_at((uintptr_t)array_original) != mono) {
    L_WARN("perf: managed callback timing unavailable; Mono imports preserved");
    return;
  }
  int n = so_rebind_import(unity, "mono_runtime_invoke", (uintptr_t)traced_invoke);
  n += so_rebind_import(unity, "mono_runtime_invoke_array", (uintptr_t)traced_array);
  L_INFO("perf: managed callback timing installed in %d Unity import slots (code generation unchanged)", n);
}

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
void managed_trace_focus(void *method) { __atomic_store_n(&focused_method, method, __ATOMIC_RELAXED); }
static void *traced_invoke(void *method, void *self, void **args, void **exception) {
  void *focus = __atomic_load_n(&focused_method, __ATOMIC_RELAXED);
  if (focus && method != focus) return invoke_original(method, self, args, exception);
  uint64_t start = work_trace_begin();
  if (!start) return invoke_original(method, self, args, exception);
  int outer = depth++ == 0;
  void *result = invoke_original(method, self, args, exception);
  depth--;
  if (outer) work_trace_end(WORK_MANAGED, start, 0, (uintptr_t)method);
  return result;
}
static void *traced_array(void *method, void *self, void *args, void **exception) {
  void *focus = __atomic_load_n(&focused_method, __ATOMIC_RELAXED);
  if (focus && method != focus) return array_original(method, self, args, exception);
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

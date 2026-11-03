#!/usr/bin/env python3
"""Exercise real Unity/Mono wrappers: argument/return/exception transparency."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
src = '\n'.join(line for line in (root / 'src/util/managed_trace.c').read_text().splitlines() if not line.startswith('#include'))
harness = r'''
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include "util/work_trace.h"
typedef struct { int id; } so_module;
static so_module mono = {1}, unity = {2};
static int owner = 1, nested, worker_nested, calls, array_calls, spans, slots, unavailable;
static uintptr_t measured_method;
static void *expected_self = (void *)20, *expected_args = (void *)30;
static void *traced_array(void *, void *, void *, void **);
#define L_WARN(...) ((void)0)
#define L_INFO(...) ((void)0)
static uint64_t work_trace_begin_stub(void) { return owner ? 100 : 0; }
static void work_trace_end_stub(unsigned kind, uint64_t start, uint64_t bytes, uintptr_t method) {
 assert(kind == WORK_MANAGED && start == 100 && bytes == 0); spans++; measured_method = method;
}
#define work_trace_begin work_trace_begin_stub
#define work_trace_end work_trace_end_stub
static void *real_array(void *method, void *self, void *args, void **exception) {
 assert(method == (void *)11 && self == expected_self && args == expected_args && exception);
 array_calls++; *exception = (void *)50; return (void *)60;
}
static void *real_invoke(void *method, void *self, void **args, void **exception) {
 assert(method == (void *)10 && self == expected_self && (void *)args == expected_args && exception);
 calls++;
 if (nested) {
   if (worker_nested) owner = 0;
   void *inner_exc = NULL;
   assert(traced_array((void *)11, self, args, &inner_exc) == (void *)60 && inner_exc == (void *)50);
   owner = 1;
 }
 *exception = (void *)51; return (void *)61;
}
static so_module *so_find_module(const char *n) { return !strcmp(n, "libmono.so") ? &mono : &unity; }
static uintptr_t so_symbol(so_module *m, const char *n) {
 assert(m == &mono);
 if (unavailable) return 0;
 return (uintptr_t)(!strcmp(n, "mono_runtime_invoke") ? (void *)real_invoke : (void *)real_array);
}
static so_module *so_module_at(uintptr_t p) { return &mono; }
static int so_rebind_import(so_module *m, const char *n, uintptr_t p) { assert(m == &unity && p); slots++; return 1; }
'''
harness += r'''
static int idle_fixture;
static int idle_test(void *self) { return self == expected_self && idle_fixture; }
'''
harness += src
harness += r'''
int main(void) {
 unavailable = 1; managed_trace_install(); assert(slots == 0);
 unavailable = 0; managed_trace_install(); assert(slots == 2);
 void *exception = NULL;
 assert(traced_invoke((void *)10, expected_self, expected_args, &exception) == (void *)61);
 assert(exception == (void *)51 && calls == 1 && spans == 1 && measured_method == 10 && depth == 0);
 assert(traced_array((void *)11, expected_self, expected_args, &exception) == (void *)60);
 assert(exception == (void *)50 && array_calls == 1 && spans == 2 && measured_method == 11);
 owner = 0;
 traced_invoke((void *)10, expected_self, expected_args, &exception);
 assert(calls == 2 && spans == 2 && depth == 0);
 owner = 1; nested = 1;
 traced_invoke((void *)10, expected_self, expected_args, &exception);
 assert(calls == 3 && array_calls == 2 && spans == 3 && measured_method == 10 && depth == 0);
 worker_nested = 1;
 traced_invoke((void *)10, expected_self, expected_args, &exception);
 assert(calls == 4 && array_calls == 3 && spans == 4 && measured_method == 10 && depth == 0);
 nested = 0; worker_nested = 0;
 managed_trace_frame_begin(1);managed_trace_focus((void *)11);
 traced_invoke((void *)10, expected_self, expected_args, &exception);
 assert(calls == 5 && spans == 4 && depth == 0);
 traced_array((void *)11, expected_self, expected_args, &exception);
 assert(array_calls == 4 && spans == 5 && measured_method == 11);
 managed_trace_focus(NULL);
 // Incomplete menu setup leaves full coverage; configured menu timing samples
 // unrelated callbacks once per 32 frames and always forwards all game calls.
 unsigned old_spans=spans;
 managed_trace_menu_mode(1);managed_trace_frame_begin(1);
 managed_trace_menu_methods(NULL,(void *)11,(void *)12,(void *)13);
 traced_invoke((void *)10,expected_self,expected_args,&exception);assert(spans==old_spans+1);
 managed_trace_menu_methods((void *)11,(void *)12,(void *)13,(void *)14);
 old_spans=spans;
 traced_invoke((void *)10,expected_self,expected_args,&exception);assert(spans==old_spans);
 traced_array((void *)11,expected_self,expected_args,&exception);assert(spans==old_spans+1);
 managed_trace_frame_begin(32);
 traced_invoke((void *)10,expected_self,expected_args,&exception);assert(spans==old_spans+2);
 managed_trace_focus((void *)11);
 traced_invoke((void *)10,expected_self,expected_args,&exception);assert(spans==old_spans+3);
 // Focused gameplay keeps periodic full samples and a bounded recovery window.
 managed_trace_frame_begin(33);
 old_spans=spans;
 traced_invoke((void *)10,expected_self,expected_args,&exception);assert(spans==old_spans);
 managed_trace_frame_end(100000);assert(!recovery_frames);
 managed_trace_frame_end(100001);assert(recovery_frames==120);
 for(unsigned f=1;f<=120;f++) {
   managed_trace_frame_begin(f);
   assert(measure_method((void *)10));
 }
 assert(!recovery_frames);managed_trace_frame_begin(121);
 assert(!measure_method((void *)10));
 managed_trace_frame_begin(128);assert(measure_method((void *)10));
 managed_trace_frame_end(500000);managed_trace_frame_begin(129);
 assert(recovery_frames==119);
 managed_trace_frame_end(500000);assert(recovery_frames==120);
 // Worker callbacks still forward through work_trace_begin's owner guard.
 owner=0;traced_invoke((void *)10,expected_self,expected_args,&exception);
 assert(spans==old_spans && depth==0);owner=1;
 traced_array((void *)11,expected_self,expected_args,&exception);assert(spans==old_spans+1);
 managed_trace_focus(NULL);managed_trace_menu_mode(0);managed_trace_frame_begin(1);
 assert(!recovery_frames);
 managed_trace_frame_end(500000);assert(!recovery_frames);
 traced_invoke((void *)10,expected_self,expected_args,&exception);assert(spans==old_spans+2);
 // Only the validated method can bypass Mono, and only while its predicate
 // says the original void callback has no work. Both invoke variants agree.
 managed_trace_idle_method(NULL,idle_test);assert(!idle_method);
 managed_trace_idle_method((void *)10,idle_test);
 managed_trace_idle_method((void *)11,idle_test);assert(idle_method==(void *)10);
 unsigned old_calls=calls, old_arrays=array_calls;old_spans=spans;
 idle_fixture=1;exception=(void *)99;
 assert(!traced_invoke((void *)10,expected_self,expected_args,&exception));assert(!exception);
 exception=(void *)99;
 assert(!traced_array((void *)10,expected_self,expected_args,&exception));assert(!exception);
 assert(!traced_invoke((void *)10,expected_self,expected_args,NULL));
 assert(calls==old_calls && array_calls==old_arrays && spans==old_spans && !depth);
 assert(managed_trace_idle_skipped()==3 && managed_trace_idle_forwarded()==0);
 // An unrelated method retains argument, return and exception behavior.
 traced_array((void *)11,expected_self,expected_args,&exception);
 assert(array_calls==old_arrays+1 && exception==(void *)50);
 idle_fixture=0;
 assert(traced_invoke((void *)10,expected_self,expected_args,&exception)==(void *)61);
 assert(exception==(void *)51 && calls==old_calls+1 && managed_trace_idle_forwarded()==1);
 puts("PASS: Mono args/results/exceptions, worker/nested accounting, menu/race sampling, 120-frame recovery/retrigger/exit and validated idle dispatch; missing API keeps imports");
}
'''
with tempfile.TemporaryDirectory() as d:
 p = Path(d); (p / 'test.c').write_text(harness)
 subprocess.run(['cc', '-fsanitize=address,undefined', '-I', str(root / 'src'), str(p / 'test.c'), '-o', str(p / 'test')], check=True)
 subprocess.run([str(p / 'test')], check=True)

// Stall diagnostics. Blocking bridges record where each thread waits (bridge,
// object, guest caller); a watchdog thread dumps every thread's state when the
// render loop stops producing frames for STALL_SECONDS, which is how deadlocks
// show up in the log when no thread is left to report them.

#include "bridges.h"
#include "../loader/so_util.h"
#include "../util/log.h"

#include <stdio.h>
#include <psp2/kernel/threadmgr.h>

#define MAX_NOTES 128
#define STALL_SECONDS 10

typedef struct {
  volatile SceUID uid;
  const char *volatile where;
  const void *volatile obj;
  volatile uintptr_t caller;
  volatile int waiting;
} wait_note;

static wait_note notes[MAX_NOTES];
volatile unsigned watchdog_frames;

void signal_watchdog_tick(void);

static wait_note *my_note(void) {
  SceUID self = sceKernelGetThreadId();
  for (int i = 0; i < MAX_NOTES; i++) {
    if (notes[i].uid == self)
      return &notes[i];
  }
  for (int i = 0; i < MAX_NOTES; i++) {
    if (!notes[i].uid && __sync_bool_compare_and_swap(&notes[i].uid, 0, self))
      return &notes[i];
  }
  return NULL;
}

void thread_wait_begin(const char *where, const void *obj, uintptr_t caller) {
  wait_note *n = my_note();
  if (n) {
    n->where = where;
    n->obj = obj;
    n->caller = caller;
    n->waiting = 1;
  }
}

void thread_wait_end(void) {
  wait_note *n = my_note();
  if (n)
    n->waiting = 0;
}

static void describe(uintptr_t addr, char *buf, size_t size) {
  so_module *m = so_module_at(addr);
  if (m)
    snprintf(buf, size, "%s+0x%X", m->name, addr - m->base);
  else
    snprintf(buf, size, "0x%08X", addr);
}

static void dump_threads(void) {
  L_WARN("watchdog: no new frame for %d s, thread states:", STALL_SECONDS);
  for (int i = 0; i < MAX_NOTES; i++) {
    wait_note *n = &notes[i];
    if (!n->uid)
      continue;
    SceKernelThreadInfo info;
    info.size = sizeof(info);
    if (sceKernelGetThreadInfo(n->uid, &info) < 0)
      continue;
    char caller[64], wait_name[32] = "";
    describe(n->caller, caller, sizeof(caller));
    if (info.waitType & 0x20) {
      SceKernelSemaInfo sema;
      sema.size = sizeof(sema);
      if (sceKernelGetSemaInfo(info.waitId, &sema) >= 0)
        snprintf(wait_name, sizeof(wait_name), " (%s)", sema.name);
    }
    L_WARN("  %-24s uid %08X status %u wait 0x%X/%08X%s | %s %s %p from %s", info.name, n->uid, info.status,
           info.waitType, info.waitId, wait_name, n->waiting ? "in" : "last", n->where ? n->where : "-", n->obj,
           caller);
  }
}

static int watchdog_thread(SceSize args, void *argp) {
  unsigned last_frames = 0;
  int still = 0, dumped = 0;
  for (;;) {
    sceKernelDelayThread(1000 * 1000);
    signal_watchdog_tick();
    if (watchdog_frames != last_frames) {
      last_frames = watchdog_frames;
      still = 0;
      dumped = 0;
    } else if (++still >= STALL_SECONDS && !dumped && last_frames) {
      dump_threads();
      dumped = 1;
    }
  }
  return 0;
}

void watchdog_start(void) {
  SceUID t = sceKernelCreateThread("srr_watchdog", watchdog_thread, 0x10000100 - 20, 0x4000, 0, 0, NULL);
  sceKernelStartThread(t, 0, NULL);
}

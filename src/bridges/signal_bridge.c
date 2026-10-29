// POSIX signals on top of kubridge exception handlers.
// Mono relies on SIGSEGV to turn null dereferences into NullReferenceException,
// so CPU aborts are converted into a Linux-style siginfo/ucontext, the guest
// handler runs, and the (possibly modified) registers are written back.
// Cross-thread signals are queued and delivered cooperatively in blocking
// bridges; this also implements Mono/Boehm GC suspension and restart.

#include "bridges.h"
#include "../loader/so_util.h"
#include "../util/log.h"

#include <kubridge.h>
#include <string.h>

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <pthread.h>

int *__errno_bionic(void);
uintptr_t pthread_bridge_entry_of(uint32_t thread);
SceUID pthread_bridge_uid_of(uint32_t id);

#define B_SIGILL 4
#define B_SIGBUS 7
#define B_SIGSEGV 11
#define B_NSIG 65

#define B_SA_SIGINFO 0x4

typedef struct {
  uintptr_t handler;  // sa_handler / sa_sigaction
  uint32_t mask;
  int flags;
  uintptr_t restorer;
} bionic_sigaction;

typedef struct {
  int si_signo;
  int si_errno;
  int si_code;
  uintptr_t si_addr;
  uint8_t pad[128 - 16];
} bionic_siginfo;

typedef struct {
  uint32_t trap_no, error_code, oldmask;
  uint32_t r[11];  // arm_r0..arm_r10
  uint32_t fp, ip, sp, lr, pc, cpsr;
  uint32_t fault_address;
} bionic_sigcontext;

typedef struct {
  uint32_t uc_flags;
  uintptr_t uc_link;
  struct {
    uintptr_t ss_sp;
    int ss_flags;
    size_t ss_size;
  } uc_stack;
  bionic_sigcontext uc_mcontext;
  uint32_t uc_sigmask;
  uint8_t pad[128 - 4];
  uint32_t uc_regspace[128];
} bionic_ucontext;

static bionic_sigaction actions[B_NSIG];

int sigaction_bridge(int sig, const bionic_sigaction *act, bionic_sigaction *old) {
  if (sig <= 0 || sig >= B_NSIG)
    return -1;
  if (old)
    *old = actions[sig];
  if (act) {
    actions[sig] = *act;
    L_DEBUG("sigaction(%d) handler=%p flags=0x%x", sig, (void *)act->handler, act->flags);
  }
  return 0;
}

uintptr_t bsd_signal_bridge(int sig, uintptr_t handler) {
  if (sig <= 0 || sig >= B_NSIG)
    return (uintptr_t)-1;
  uintptr_t old = actions[sig].handler;
  actions[sig].handler = handler;
  actions[sig].flags = 0;
  return old;
}

static int deliver(int sig, bionic_siginfo *info, bionic_ucontext *uc) {
  uintptr_t h = actions[sig].handler;
  if (h == 0 || h == 1)  // SIG_DFL / SIG_IGN
    return 0;
  if (actions[sig].flags & B_SA_SIGINFO)
    ((void (*)(int, bionic_siginfo *, void *))h)(sig, info, uc);
  else
    ((void (*)(int))h)(sig);
  return 1;
}

int raise_bridge(int sig) {
  bionic_siginfo info;
  memset(&info, 0, sizeof(info));
  info.si_signo = sig;
  if (!deliver(sig, &info, NULL))
    L_WARN("raise(%d) with no handler", sig);
  return 0;
}

int kill_bridge(int pid, int sig) {
  if (sig == 0)
    return 0;
  return raise_bridge(sig);
}

// --- cross-thread signals (cooperative delivery) ----------------------------
// Boehm GC stops the world with pthread_kill(SIGPWR) and waits for each thread
// to acknowledge from its signal handler, then resumes them with SIGXCPU while
// they sit in sigsuspend(). The Vita can't interrupt another thread, so a
// signal sent to another thread is queued and that thread runs the handler
// the next time it passes through a blocking bridge (mutex, condition
// variable, semaphore, sleep), which is where idle threads spend their time.

typedef struct {
  uint32_t thread;
  volatile uint32_t pending;  // bit n = signal n (signals < 32)
  uint32_t active;           // currently executing handlers on this thread
  uint64_t queued_at;         // when the oldest pending signal was queued (us)
  int stale_warned;
  SceUID wake;              // restart event for sigsuspend; zero until allocated
} sig_slot;

#define MAX_SIG_THREADS 128
static sig_slot sig_slots[MAX_SIG_THREADS];
static volatile int total_pending;
static SceKernelLwMutexWork sig_lock;

static sig_slot *slot_for(uint32_t thread, int create) {
  sig_slot *free_slot = NULL;
  for (int i = 0; i < MAX_SIG_THREADS; i++) {
    if (sig_slots[i].thread == thread)
      return &sig_slots[i];
    if (!sig_slots[i].thread && !free_slot)
      free_slot = &sig_slots[i];
  }
  if (create && free_slot) {
    free_slot->thread = thread;
    free_slot->wake = sceKernelCreateSema("sig_wake", 0, 0, 1, NULL);
  }
  return create ? free_slot : NULL;
}

static uint32_t self_id(void) { return (uint32_t)sceKernelGetThreadId(); }

static void deliver_now(int sig) {
  bionic_siginfo info;
  memset(&info, 0, sizeof(info));
  info.si_signo = sig;
  info.si_code = -6;  // SI_TKILL
  bionic_ucontext uc;
  memset(&uc, 0, sizeof(uc));
  uc.uc_mcontext.sp = (uint32_t)__builtin_frame_address(0);
  deliver(sig, &info, &uc);
}

// Runs this thread's pending signals that are not blocked by `mask`.
static void warn_stale(void);

void signal_watchdog_tick(void) {
  if (total_pending) {
    sceKernelLockLwMutex(&sig_lock, 1, NULL);
    warn_stale();
    sceKernelUnlockLwMutex(&sig_lock, 1);
  }
}

// Reports threads that haven't picked up a queued signal for 3 s: they are
// busy outside any bridge and will stall the GC.
static void warn_stale(void) {
  uint64_t now = sceKernelGetProcessTimeWide();
  for (int i = 0; i < MAX_SIG_THREADS; i++) {
    sig_slot *s = &sig_slots[i];
    if (s->pending && !s->stale_warned && now - s->queued_at > 3000000) {
      s->stale_warned = 1;
      SceKernelThreadInfo info;
      info.size = sizeof(info);
      int ok = sceKernelGetThreadInfo(s->thread, &info) >= 0;
      uintptr_t entry = pthread_bridge_entry_of(s->thread);
      SceKernelSemaInfo sema;
      sema.size = sizeof(sema);
      const char *wait_name = "?";
      if (ok && (info.waitType & 0x20) && sceKernelGetSemaInfo(info.waitId, &sema) >= 0)
        wait_name = sema.name;
      so_module *m = so_module_at(entry);
      // status 1/2 = running/ready (busy outside any bridge); 8 = blocked in
      // a kernel wait the bridges don't cover (waitType/waitId say which).
      L_WARN("signals: thread %p (%s) hasn't handled signals 0x%X for 3 s: status %u, waitType 0x%X, waitId 0x%X "
             "(%s), started at %s+0x%X",
             (void *)s->thread, ok ? info.name : "?", s->pending, ok ? info.status : 0, ok ? info.waitType : 0,
             ok ? info.waitId : 0, wait_name, m ? m->name : "?", m ? entry - m->base : entry);
    }
  }
}

static int deliver_pending(uint32_t mask) {
  if (!total_pending)
    return 0;
  sceKernelLockLwMutex(&sig_lock, 1, NULL);
  sig_slot *slot = slot_for(self_id(), 0);
  uint32_t todo = slot ? slot->pending & ~(mask | slot->active) : 0;
  // A suspend handler waits inside sigsuspend for a later resume signal.
  // Removing a whole batch here hid resume signals from that nested wait.
  // Pick one signal only, with Boehm's SIGPWR stop before SIGXCPU resume.
  int sig = todo & (1u << 30) ? 30 : (todo ? __builtin_ctz(todo) : 0);
  uint32_t old_active = slot ? slot->active : 0;
  if (sig) {
    slot->pending &= ~(1u << sig);
    slot->active |= 1u << sig;
    total_pending--;
  }
  sceKernelUnlockLwMutex(&sig_lock, 1);
  if (!sig) return 0;
  deliver_now(sig);
  sceKernelLockLwMutex(&sig_lock, 1, NULL);
  slot->active = old_active;
  sceKernelUnlockLwMutex(&sig_lock, 1);
  return 1;
}

int signal_pending_self(void) {
  if (!total_pending)
    return 0;
  sceKernelLockLwMutex(&sig_lock, 1, NULL);
  sig_slot *slot = slot_for(self_id(), 0);
  int r = slot && slot->pending;
  sceKernelUnlockLwMutex(&sig_lock, 1);
  return r;
}

int signal_poll(void) { return deliver_pending(0); }

// Accepts a pthread_t or a kernel thread UID (Mono uses tkill(gettid())).
int pthread_kill_bridge(uint32_t target, int sig) {
  if (sig == 0)
    return 0;
  uint32_t thread = (uint32_t)pthread_bridge_uid_of(target);
  if (!thread) {
    L_WARN("pthread_kill(%p, %d): unknown thread", (void *)target, sig);
    return 3;  // ESRCH
  }
  if (thread == self_id()) {
    deliver_now(sig);
    return 0;
  }
  // Boehm GC treats ESRCH as "thread already gone" and stops waiting for it;
  // queueing a signal for a finished thread would hang the collection.
  SceKernelThreadInfo info;
  info.size = sizeof(info);
  if (sceKernelGetThreadInfo(thread, &info) < 0 ||
      (info.status & (SCE_THREAD_DORMANT | SCE_THREAD_KILLED | SCE_THREAD_DEAD))) {
    L_DEBUG("pthread_kill(%p, %d): thread has exited", (void *)thread, sig);
    return 3;  // ESRCH
  }
  if (sig >= 32 || actions[sig].handler <= 1) {
    L_WARN("pthread_kill(%p, %d): no handler, ignored", (void *)thread, sig);
    return 0;
  }
  sceKernelLockLwMutex(&sig_lock, 1, NULL);
  sig_slot *slot = slot_for(thread, 1);
  if (slot && !(slot->pending & (1u << sig))) {
    if (!slot->pending) {
      slot->queued_at = sceKernelGetProcessTimeWide();
      slot->stale_warned = 0;
    }
    slot->pending |= 1u << sig;
    total_pending++;
  }
  // The pending bit is the predicate. A binary semaphore only wakes the
  // sleeper; coalesced wakeups and a wake before WaitSema are both safe.
  if (slot && slot->wake > 0) sceKernelSignalSema(slot->wake, 1);
  sceKernelUnlockLwMutex(&sig_lock, 1);
  return slot ? 0 : 11;  // EAGAIN when the table is full
}

int tkill_bridge(int tid, int sig) { return pthread_kill_bridge(tid, sig); }

int sigprocmask_bridge(int how, const uint32_t *set, uint32_t *old) {
  if (old)
    *old = 0;
  return 0;
}

// Waits until a signal outside `mask` has been handled (EINTR).
// Linux sigset_t uses bit (n - 1) for signal n; pending sets use bit n.
int sigsuspend_bridge(const uint32_t *mask) {
  uint32_t blocked = mask ? *mask << 1 : 0;
  WAIT_BEGIN("sigsuspend", mask);
  sceKernelLockLwMutex(&sig_lock, 1, NULL);
  sig_slot *slot = slot_for(self_id(), 1);
  SceUID wake = slot ? slot->wake : -1;
  sceKernelUnlockLwMutex(&sig_lock, 1);
  while (!deliver_pending(blocked)) {
    // Suspended GC threads used to wake 10,000 times per second each.
    // Wait for an actual signal; the timeout keeps allocation failure safe.
    SceUInt timeout = 5000;
    if (wake > 0) sceKernelWaitSema(wake, 1, &timeout);
    else sceKernelDelayThread(timeout);
  }
  thread_wait_end();
  *__errno_bionic() = 4;  // EINTR
  return -1;
}

// --- CPU exception -> signal ------------------------------------------------

static void log_crash(KuKernelExceptionContext *ctx, const char *what) {
  so_module *m = so_module_at(ctx->pc);
  so_module *lm = so_module_at(ctx->lr);
  L_ERROR("%s at PC=0x%08X (%s+0x%X) LR=0x%08X (%s+0x%X) FAR=0x%08X FSR=0x%08X", what, ctx->pc,
          m ? m->name : "?", m ? ctx->pc - m->base : 0, ctx->lr, lm ? lm->name : "?", lm ? ctx->lr - lm->base : 0,
          ctx->FAR, ctx->FSR);
  L_ERROR("r0=%08X r1=%08X r2=%08X r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X", ctx->r0, ctx->r1, ctx->r2, ctx->r3,
          ctx->r4, ctx->r5, ctx->r6, ctx->r7);
  L_ERROR("r8=%08X r9=%08X r10=%08X r11=%08X r12=%08X sp=%08X cpsr=%08X", ctx->r8, ctx->r9, ctx->r10, ctx->r11,
          ctx->r12, ctx->sp, ctx->SPSR);
}

static void fatal_exit(void) {
  L_ERROR("exiting after unrecoverable fault");
  sceKernelExitProcess(0);
  for (;;)
    sceKernelDelayThread(1000000);
}

static void handle_exception(KuKernelExceptionContext *ctx, int sig, const char *what) {
  // A guest handler that returns without fixing the fault (Unity's crash
  // reporter chains back to the default action) would re-fault forever.
  static uint32_t last_pc, last_far;
  static int repeats;
  if (ctx->pc == last_pc && ctx->FAR == last_far) {
    repeats++;
  } else {
    last_pc = ctx->pc;
    last_far = ctx->FAR;
    repeats = 0;
  }

  if (!actions[sig].handler || actions[sig].handler == 1 || repeats >= 3) {
    L_ERROR("signals: fatal %s (%s)", what,
            repeats >= 3 ? "the guest handler resumed at the same fault 3 times" : "no guest handler installed");
    log_crash(ctx, what);
    // Exiting from inside the exception handler doesn't stop the process;
    // resume the faulting thread in fatal_exit() instead.
    uintptr_t target = (uintptr_t)fatal_exit;
    ctx->pc = target & ~1;
    ctx->SPSR = (target & 1) ? (ctx->SPSR | 0x20) : (ctx->SPSR & ~0x20);
    ctx->SPSR &= ~0x0600FC00;  // clear Thumb IT state bits
    return;
  }

  // Unity's crash reporter can itself fault while chaining to a null handler.
  // Preserve the first native context before entering the guest reporter.
  if (repeats == 0) log_crash(ctx, what);

  bionic_siginfo info;
  memset(&info, 0, sizeof(info));
  info.si_signo = sig;
  info.si_code = 1;  // SEGV_MAPERR
  info.si_addr = ctx->FAR;

  bionic_ucontext uc;
  memset(&uc, 0, sizeof(uc));
  uint32_t *regs = &ctx->r0;
  for (int i = 0; i < 11; i++)
    uc.uc_mcontext.r[i] = regs[i];
  uc.uc_mcontext.fp = ctx->r11;
  uc.uc_mcontext.ip = ctx->r12;
  uc.uc_mcontext.sp = ctx->sp;
  uc.uc_mcontext.lr = ctx->lr;
  uc.uc_mcontext.pc = ctx->pc;
  uc.uc_mcontext.cpsr = ctx->SPSR;
  uc.uc_mcontext.fault_address = ctx->FAR;
  memcpy(uc.uc_regspace, ctx->vfpRegisters, sizeof(ctx->vfpRegisters));

  static int traced;
  int trace = traced++ < 40;
  uint32_t fault_pc = ctx->pc;

  deliver(sig, &info, &uc);

  if (trace) {
    so_module *m = so_module_at(fault_pc);
    so_module *hm = so_module_at(actions[sig].handler);
    so_module *rm = so_module_at(uc.uc_mcontext.pc);
    L_INFO("signals: %s sig %d at pc %08X (%s+0x%X) far %08X -> handler %s+0x%X -> resume pc %08X (%s+0x%X) sp %08X",
           what, sig, fault_pc, m ? m->name : "?", m ? fault_pc - m->base : 0, ctx->FAR, hm ? hm->name : "?",
           hm ? actions[sig].handler - hm->base : 0, uc.uc_mcontext.pc, rm ? rm->name : "?",
           rm ? uc.uc_mcontext.pc - rm->base : 0, uc.uc_mcontext.sp);
  }

  for (int i = 0; i < 11; i++)
    regs[i] = uc.uc_mcontext.r[i];
  ctx->r11 = uc.uc_mcontext.fp;
  ctx->r12 = uc.uc_mcontext.ip;
  ctx->sp = uc.uc_mcontext.sp;
  ctx->lr = uc.uc_mcontext.lr;
  ctx->pc = uc.uc_mcontext.pc;
  ctx->SPSR = uc.uc_mcontext.cpsr;
}

static void on_data_abort(KuKernelExceptionContext *ctx) { handle_exception(ctx, B_SIGSEGV, "Data abort"); }
static void on_prefetch_abort(KuKernelExceptionContext *ctx) { handle_exception(ctx, B_SIGSEGV, "Prefetch abort"); }
static void on_undefined(KuKernelExceptionContext *ctx) { handle_exception(ctx, B_SIGILL, "Undefined instruction"); }

void signal_init(void) {
  sceKernelCreateLwMutex(&sig_lock, "signals", SCE_KERNEL_MUTEX_ATTR_RECURSIVE, 0, NULL);
  KuKernelExceptionHandlerOpt opt = {.size = sizeof(opt)};
  int r = kuKernelRegisterExceptionHandler(KU_KERNEL_EXCEPTION_TYPE_DATA_ABORT, on_data_abort, NULL, &opt);
  kuKernelRegisterExceptionHandler(KU_KERNEL_EXCEPTION_TYPE_PREFETCH_ABORT, on_prefetch_abort, NULL, &opt);
  kuKernelRegisterExceptionHandler(KU_KERNEL_EXCEPTION_TYPE_UNDEFINED_INSTRUCTION, on_undefined, NULL, &opt);
  if (r < 0)
    L_WARN("kubridge exception handlers unavailable (0x%08X): update kubridge.skprx", r);
  else
    L_INFO("signals: kubridge exception handlers registered");
}

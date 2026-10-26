// POSIX signals on top of kubridge exception handlers.
// Mono relies on SIGSEGV to turn null dereferences into NullReferenceException,
// so CPU aborts are converted into a Linux-style siginfo/ucontext, the guest
// handler runs, and the (possibly modified) registers are written back.
// Asynchronous signals between threads (pthread_kill) have no Vita equivalent;
// they are logged and acknowledged without delivery.

#include "bridges.h"
#include "../loader/so_util.h"
#include "../util/log.h"

#include <kubridge.h>
#include <string.h>

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

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

int pthread_kill_bridge(uint32_t thread, int sig) {
  if (sig == 0)
    return 0;
  static int warned;
  if (warned++ < 8)
    L_WARN("pthread_kill(%p, %d): cross-thread signals are not deliverable", (void *)thread, sig);
  return 0;
}

int tkill_bridge(int tid, int sig) { return pthread_kill_bridge(tid, sig); }

int sigprocmask_bridge(int how, const uint32_t *set, uint32_t *old) {
  if (old)
    *old = 0;
  return 0;
}

int sigsuspend_bridge(const uint32_t *mask) {
  L_WARN("sigsuspend() would block forever; returning");
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

static void handle_exception(KuKernelExceptionContext *ctx, int sig, const char *what) {
  if (!actions[sig].handler || actions[sig].handler == 1) {
    log_crash(ctx, what);
    sceKernelExitProcess(0);
    return;
  }

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

  deliver(sig, &info, &uc);

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
  KuKernelExceptionHandlerOpt opt = {.size = sizeof(opt)};
  int r = kuKernelRegisterExceptionHandler(KU_KERNEL_EXCEPTION_TYPE_DATA_ABORT, on_data_abort, NULL, &opt);
  kuKernelRegisterExceptionHandler(KU_KERNEL_EXCEPTION_TYPE_PREFETCH_ABORT, on_prefetch_abort, NULL, &opt);
  kuKernelRegisterExceptionHandler(KU_KERNEL_EXCEPTION_TYPE_UNDEFINED_INSTRUCTION, on_undefined, NULL, &opt);
  if (r < 0)
    L_WARN("kubridge exception handlers unavailable (0x%08X): update kubridge.skprx", r);
  else
    L_INFO("signals: kubridge exception handlers registered");
}

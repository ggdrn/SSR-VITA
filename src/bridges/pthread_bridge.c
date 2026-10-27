// Bionic pthread ABI on top of VitaSDK pthread/SceKernel primitives.
// Bionic keeps mutex/cond/once/sem in a single 32-bit word (zero = statically
// initialised), so we lazily allocate the real object and store its pointer.

#include "bridges.h"
#include "../util/log.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <psp2/kernel/cpu.h>
#include <psp2/kernel/error.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

int *__errno_bionic(void);
int clock_gettime_bridge(int clk, struct timespec *ts);

#define B_EBUSY 16
#define B_EINVAL 22
#define B_ETIMEDOUT 110

#define MUTEX_INIT_NORMAL 0x0000
#define MUTEX_INIT_RECURSIVE 0x4000
#define MUTEX_INIT_ERRORCHECK 0x8000

static SceKernelLwMutexWork init_lock;

static void global_lock(void) { sceKernelLockLwMutex(&init_lock, 1, NULL); }
static void global_unlock(void) { sceKernelUnlockLwMutex(&init_lock, 1); }

// --- mutex ------------------------------------------------------------------

static int is_static_mutex(uint32_t v) {
  return v == MUTEX_INIT_NORMAL || v == MUTEX_INIT_RECURSIVE || v == MUTEX_INIT_ERRORCHECK;
}

static pthread_mutex_t *mutex_create(void) {
  pthread_mutex_t *m = malloc(sizeof(pthread_mutex_t));
  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  // Every mutex is recursive: some bionic code relocks "normal" mutexes from
  // the same thread in error paths, and recursion costs nothing otherwise.
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(m, &attr);
  pthread_mutexattr_destroy(&attr);
  return m;
}

static pthread_mutex_t *mutex_get(volatile uint32_t *bm) {
  uint32_t v = *bm;
  if (!is_static_mutex(v))
    return (pthread_mutex_t *)v;
  global_lock();
  v = *bm;
  if (is_static_mutex(v)) {
    pthread_mutex_t *m = mutex_create();
    __sync_synchronize();
    *bm = (uint32_t)m;
    v = (uint32_t)m;
  }
  global_unlock();
  return (pthread_mutex_t *)v;
}

int pthread_mutexattr_init_bridge(uint32_t *attr) {
  *attr = 0;
  return 0;
}
int pthread_mutexattr_destroy_bridge(uint32_t *attr) { return 0; }
int pthread_mutexattr_settype_bridge(uint32_t *attr, int type) {
  *attr = type;
  return 0;
}

int pthread_mutex_init_bridge(uint32_t *bm, const uint32_t *attr) {
  *bm = (uint32_t)mutex_create();
  return 0;
}

int pthread_mutex_destroy_bridge(uint32_t *bm) {
  if (!is_static_mutex(*bm)) {
    pthread_mutex_destroy((pthread_mutex_t *)*bm);
    free((void *)*bm);
  }
  *bm = 0;
  return 0;
}

// Blocking waits run in short slices so a thread parked here still answers
// signals queued by pthread_kill() (see signal_bridge.c). Without this, Mono's
// GC waits forever for threads blocked on its allocation lock.
// 20 ms: short enough for GC stop-the-world, long enough not to burn CPU
// with hundreds of wakeups per second across idle threads.
#define SLICE_NS (20 * 1000 * 1000)

static void deadline_after(struct timespec *ts, int64_t ns) {
  clock_gettime(CLOCK_REALTIME, ts);
  ns += ts->tv_nsec;
  ts->tv_sec += ns / 1000000000LL;
  ts->tv_nsec = ns % 1000000000LL;
}

static int ts_before(const struct timespec *a, const struct timespec *b) {
  return a->tv_sec < b->tv_sec || (a->tv_sec == b->tv_sec && a->tv_nsec < b->tv_nsec);
}

int pthread_mutex_lock_bridge(uint32_t *bm) {
  pthread_mutex_t *m = mutex_get(bm);
  if (pthread_mutex_trylock(m) == 0)
    return 0;
  WAIT_BEGIN("mutex_lock", bm);
  for (;;) {
    signal_poll();
    struct timespec ts;
    deadline_after(&ts, SLICE_NS);
    int r = pthread_mutex_timedlock(m, &ts);
    if (r != ETIMEDOUT) {
      thread_wait_end();
      return r;
    }
  }
}
int pthread_mutex_unlock_bridge(uint32_t *bm) {
  int r = pthread_mutex_unlock(mutex_get(bm));
  signal_poll();
  return r;
}
int pthread_mutex_trylock_bridge(uint32_t *bm) {
  if (pthread_mutex_trylock(mutex_get(bm)) == 0)
    return 0;
  signal_poll();  // trylock spinners (Boehm's GC_lock) must still take signals
  return B_EBUSY;
}

// --- condition variables ----------------------------------------------------

static pthread_cond_t *cond_get(volatile uint32_t *bc) {
  uint32_t v = *bc;
  if (v)
    return (pthread_cond_t *)v;
  global_lock();
  v = *bc;
  if (!v) {
    pthread_cond_t *c = malloc(sizeof(pthread_cond_t));
    pthread_cond_init(c, NULL);
    __sync_synchronize();
    *bc = (uint32_t)c;
    v = (uint32_t)c;
  }
  global_unlock();
  return (pthread_cond_t *)v;
}

int pthread_cond_init_bridge(uint32_t *bc, const void *attr) {
  pthread_cond_t *c = malloc(sizeof(pthread_cond_t));
  pthread_cond_init(c, NULL);
  *bc = (uint32_t)c;
  return 0;
}

int pthread_cond_destroy_bridge(uint32_t *bc) {
  if (*bc) {
    pthread_cond_destroy((pthread_cond_t *)*bc);
    free((void *)*bc);
  }
  *bc = 0;
  return 0;
}

// Waits one slice (or until `target`, CLOCK_REALTIME, if sooner) and returns.
// An expired slice is reported as a spurious wakeup (allowed by POSIX) so the
// caller rechecks its predicate: a wakeup lost by pthread-embedded's timed
// wait, or signalled outside the mutex, then costs one slice instead of
// hanging the thread forever. Queued signals run with the mutex released.
static int cond_wait_until(uint32_t *bc, uint32_t *bm, const struct timespec *target) {
  pthread_cond_t *c = cond_get(bc);
  pthread_mutex_t *m = mutex_get(bm);
  struct timespec slice;
  deadline_after(&slice, SLICE_NS);
  if (target && ts_before(target, &slice))
    slice = *target;
  int r = pthread_cond_timedwait(c, m, &slice);
  if (r == 0)
    return 0;
  if (r != ETIMEDOUT)
    return r;
  if (signal_pending_self()) {
    pthread_mutex_unlock(m);
    signal_poll();
    pthread_mutex_lock(m);
  }
  if (target) {
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    if (!ts_before(&now, target))
      return B_ETIMEDOUT;
  }
  return 0;
}

int pthread_cond_wait_bridge(uint32_t *bc, uint32_t *bm) {
  WAIT_BEGIN("cond_wait", bc);
  int r = cond_wait_until(bc, bm, NULL);
  thread_wait_end();
  return r;
}
int pthread_cond_signal_bridge(uint32_t *bc) {
  signal_poll();
  return pthread_cond_signal(cond_get(bc));
}
int pthread_cond_broadcast_bridge(uint32_t *bc) {
  signal_poll();
  return pthread_cond_broadcast(cond_get(bc));
}

// Waits until `abstime` measured on bionic clock `clk`.
static int cond_timedwait(uint32_t *bc, uint32_t *bm, const struct timespec *abstime, int clk) {
  struct timespec now, target;
  clock_gettime_bridge(clk, &now);
  int64_t rel_ns = (int64_t)(abstime->tv_sec - now.tv_sec) * 1000000000LL + (abstime->tv_nsec - now.tv_nsec);
  if (rel_ns < 0)
    rel_ns = 0;
  clock_gettime(CLOCK_REALTIME, &target);
  int64_t ns = target.tv_nsec + rel_ns;
  target.tv_sec += ns / 1000000000LL;
  target.tv_nsec = ns % 1000000000LL;
  return cond_wait_until(bc, bm, &target);
}

int pthread_cond_timedwait_bridge(uint32_t *bc, uint32_t *bm, const struct timespec *t) {
  WAIT_BEGIN("cond_timedwait", bc);
  int r = cond_timedwait(bc, bm, t, 0);
  thread_wait_end();
  return r;
}
int pthread_cond_timedwait_monotonic_np_bridge(uint32_t *bc, uint32_t *bm, const struct timespec *t) {
  return cond_timedwait(bc, bm, t, 1);
}

// --- threads ----------------------------------------------------------------

typedef struct {
  uint32_t flags;
  void *stack_base;
  size_t stack_size;
  size_t guard_size;
  int32_t sched_policy;
  int32_t sched_priority;
} bionic_attr;

#define B_ATTR_DETACHED 1
#define MIN_STACK (512 * 1024)

typedef struct thread_slot {
  pthread_t thread;
  SceUID uid;
  void *(*func)(void *);
  void *arg;
} thread_slot;

#define MAX_THREADS 128
static thread_slot threads[MAX_THREADS];

// Accepts either the pthread_t or the kernel thread UID.
static thread_slot *slot_find(pthread_t t) {
  for (int i = 0; i < MAX_THREADS; i++) {
    if (threads[i].func && (threads[i].thread == t || (threads[i].uid && threads[i].uid == (SceUID)t)))
      return &threads[i];
  }
  return NULL;
}

static void *thread_trampoline(void *arg) {
  thread_slot *s = arg;
  s->uid = sceKernelGetThreadId();
  // Let game threads run on any user core, so one busy-waiting thread can't
  // starve the thread it waits for.
  sceKernelChangeThreadCpuAffinityMask(s->uid, SCE_KERNEL_CPU_MASK_USER_ALL);
  return s->func(s->arg);
}

int pthread_attr_init_bridge(bionic_attr *a) {
  memset(a, 0, sizeof(*a));
  a->stack_size = 1024 * 1024;
  return 0;
}
int pthread_attr_destroy_bridge(bionic_attr *a) { return 0; }
int pthread_attr_setdetachstate_bridge(bionic_attr *a, int state) {
  if (state)
    a->flags |= B_ATTR_DETACHED;
  else
    a->flags &= ~B_ATTR_DETACHED;
  return 0;
}
int pthread_attr_getdetachstate_bridge(const bionic_attr *a, int *state) {
  *state = (a->flags & B_ATTR_DETACHED) ? 1 : 0;
  return 0;
}
int pthread_attr_setstacksize_bridge(bionic_attr *a, size_t size) {
  a->stack_size = size;
  return 0;
}
int pthread_attr_setschedparam_bridge(bionic_attr *a, const int *param) {
  a->sched_priority = *param;
  return 0;
}
int pthread_attr_setschedpolicy_bridge(bionic_attr *a, int policy) {
  a->sched_policy = policy;
  return 0;
}
int pthread_attr_getstack_bridge(const bionic_attr *a, void **addr, size_t *size) {
  *addr = a->stack_base;
  *size = a->stack_size;
  return 0;
}

int pthread_create_bridge(pthread_t *out, const bionic_attr *battr, void *(*func)(void *), void *arg) {
  thread_slot *s = NULL;
  global_lock();
  for (int i = 0; i < MAX_THREADS; i++) {
    if (!threads[i].func) {
      s = &threads[i];
      s->func = func;
      break;
    }
  }
  global_unlock();
  if (!s) {
    L_ERROR("pthread_create: thread table full");
    return 11;  // EAGAIN
  }
  s->arg = arg;
  s->uid = 0;

  pthread_attr_t attr;
  pthread_attr_init(&attr);
  size_t stack = battr && battr->stack_size > MIN_STACK ? battr->stack_size : MIN_STACK;
  pthread_attr_setstacksize(&attr, stack);
  if (battr && (battr->flags & B_ATTR_DETACHED))
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  int r = pthread_create(&s->thread, &attr, thread_trampoline, s);
  pthread_attr_destroy(&attr);
  if (r != 0) {
    L_ERROR("pthread_create failed: %d", r);
    s->func = NULL;
    return r;
  }
  *out = s->thread;
  L_DEBUG("pthread_create -> %p, entry %p", (void *)s->thread, func);
  return 0;
}

// pthread_t (a pthread-embedded handle) and the kernel thread UID that Mono
// gets from gettid() are different values; signals are keyed by UID.
#define MAX_SELF_MAP 64
static struct {
  pthread_t thread;
  SceUID uid;
} self_map[MAX_SELF_MAP];

static void remember_self(void) {
  pthread_t t = pthread_self();
  SceUID uid = sceKernelGetThreadId();
  for (int i = 0; i < MAX_SELF_MAP; i++) {
    if (self_map[i].thread == t)
      return;
    if (!self_map[i].thread) {
      self_map[i].thread = t;
      self_map[i].uid = uid;
      return;
    }
  }
}

SceUID pthread_bridge_uid_of(uint32_t id) {
  SceKernelThreadInfo info;
  info.size = sizeof(info);
  if (sceKernelGetThreadInfo((SceUID)id, &info) >= 0)
    return (SceUID)id;  // already a kernel UID (gettid/tkill)
  thread_slot *s = slot_find((pthread_t)id);
  if (s && s->uid)
    return s->uid;
  for (int i = 0; i < MAX_SELF_MAP; i++) {
    if (self_map[i].thread == (pthread_t)id)
      return self_map[i].uid;
  }
  return 0;
}

// Guest start routine of a thread created through pthread_create (for logs).
uintptr_t pthread_bridge_entry_of(uint32_t thread) {
  thread_slot *s = slot_find((pthread_t)thread);
  return s ? (uintptr_t)s->func : 0;
}

int pthread_getattr_np_bridge(pthread_t t, bionic_attr *a) {
  memset(a, 0, sizeof(*a));
  SceUID uid = 0;
  if (t == pthread_self()) {
    uid = sceKernelGetThreadId();
  } else {
    thread_slot *s = slot_find(t);
    if (s)
      uid = s->uid;
  }
  SceKernelThreadInfo info;
  memset(&info, 0, sizeof(info));
  info.size = sizeof(info);
  if (uid && sceKernelGetThreadInfo(uid, &info) >= 0) {
    a->stack_base = info.stack;
    a->stack_size = info.stackSize;
  }
  return 0;
}

int pthread_detach_bridge(pthread_t t) { return pthread_detach(t); }
// Waits for the thread's end in slices (taking queued signals) before the
// real join, which then returns immediately.
int pthread_join_bridge(pthread_t t, void **ret) {
  WAIT_BEGIN("join", (void *)t);
  thread_slot *s = slot_find(t);
  for (;;) {
    SceUID uid = s ? s->uid : (SceUID)t;
    if (uid) {
      SceUInt timeout = SLICE_NS / 1000;
      int r = sceKernelWaitThreadEnd(uid, NULL, &timeout);
      if (r != (int)SCE_KERNEL_ERROR_WAIT_TIMEOUT)
        break;
    } else {
      sceKernelDelayThread(SLICE_NS / 1000);  // trampoline hasn't recorded the UID yet
    }
    signal_poll();
  }
  thread_wait_end();
  return pthread_join(t, ret);
}
void pthread_exit_bridge(void *ret) { pthread_exit(ret); }
pthread_t pthread_self_bridge(void) {
  remember_self();
  return pthread_self();
}
int pthread_equal_bridge(pthread_t a, pthread_t b) { return a == b; }

int pthread_getschedparam_bridge(pthread_t t, int *policy, int *param) {
  *policy = 0;
  *param = 0;
  return 0;
}
int pthread_setschedparam_bridge(pthread_t t, int policy, const int *param) { return 0; }
int sched_get_priority_min_bridge(int policy) { return 0; }
int sched_get_priority_max_bridge(int policy) { return 0; }
int sched_yield_bridge(void) {
  signal_poll();
  sceKernelDelayThread(0);
  return 0;
}

// --- keys / once ------------------------------------------------------------

int pthread_key_create_bridge(uint32_t *key, void (*dtor)(void *)) {
  pthread_key_t k;
  int r = pthread_key_create(&k, dtor);
  *key = (uint32_t)k;
  return r;
}
int pthread_key_delete_bridge(uint32_t key) { return pthread_key_delete((pthread_key_t)key); }
void *pthread_getspecific_bridge(uint32_t key) {
  signal_poll();
  return pthread_getspecific((pthread_key_t)key);
}
int pthread_setspecific_bridge(uint32_t key, const void *v) { return pthread_setspecific((pthread_key_t)key, v); }

static SceKernelLwMutexWork once_lock;

int pthread_once_bridge(volatile int *once, void (*init)(void)) {
  if (*once == 2)
    return 0;
  sceKernelLockLwMutex(&once_lock, 1, NULL);  // recursive: nested once calls work
  if (*once == 0) {
    *once = 1;
    init();
    __sync_synchronize();
    *once = 2;
  }
  sceKernelUnlockLwMutex(&once_lock, 1);
  return 0;
}

// --- cleanup handlers (bionic implements push/pop as functions) -------------

typedef struct cleanup {
  struct cleanup *prev;
  void (*routine)(void *);
  void *arg;
} cleanup;

void __pthread_cleanup_push_bridge(cleanup *c, void (*routine)(void *), void *arg) {
  c->routine = routine;
  c->arg = arg;
  c->prev = NULL;
}
void __pthread_cleanup_pop_bridge(cleanup *c, int execute) {
  if (execute && c->routine)
    c->routine(c->arg);
}

// --- semaphores -------------------------------------------------------------

int sem_init_bridge(uint32_t *sem, int pshared, unsigned int value) {
  SceUID id = sceKernelCreateSema("bionic_sem", 0, value, 0x7fffffff, NULL);
  if (id < 0) {
    *__errno_bionic() = B_EINVAL;
    return -1;
  }
  *sem = id;
  return 0;
}

int sem_destroy_bridge(uint32_t *sem) {
  sceKernelDeleteSema(*sem);
  *sem = 0;
  return 0;
}

int sem_wait_bridge(uint32_t *sem) {
  WAIT_BEGIN("sem_wait", sem);
  int r;
  for (;;) {
    SceUInt timeout = SLICE_NS / 1000;
    r = sceKernelWaitSemaCB(*sem, 1, &timeout);
    if (r != (int)SCE_KERNEL_ERROR_WAIT_TIMEOUT)
      break;
    signal_poll();
  }
  thread_wait_end();
  if (r < 0) {
    *__errno_bionic() = B_EINVAL;
    return -1;
  }
  return 0;
}

int sem_trywait_bridge(uint32_t *sem) {
  if (sceKernelPollSema(*sem, 1) < 0) {
    *__errno_bionic() = 11;  // EAGAIN
    return -1;
  }
  return 0;
}

int sem_timedwait_bridge(uint32_t *sem, const struct timespec *abstime) {
  struct timespec now;
  clock_gettime_bridge(0, &now);
  int64_t us = (int64_t)(abstime->tv_sec - now.tv_sec) * 1000000LL + (abstime->tv_nsec - now.tv_nsec) / 1000;
  int r;
  do {
    SceUInt slice = us > SLICE_NS / 1000 ? SLICE_NS / 1000 : (us > 0 ? (SceUInt)us : 0);
    r = sceKernelWaitSemaCB(*sem, 1, &slice);
    us -= SLICE_NS / 1000;
    if (r == (int)SCE_KERNEL_ERROR_WAIT_TIMEOUT)
      signal_poll();
  } while (r == (int)SCE_KERNEL_ERROR_WAIT_TIMEOUT && us > 0);
  if (r == (int)SCE_KERNEL_ERROR_WAIT_TIMEOUT) {
    *__errno_bionic() = B_ETIMEDOUT;
    return -1;
  }
  if (r < 0) {
    *__errno_bionic() = B_EINVAL;
    return -1;
  }
  return 0;
}

int sem_post_bridge(uint32_t *sem) {
  signal_poll();
  sceKernelSignalSema(*sem, 1);
  return 0;
}

int sem_getvalue_bridge(uint32_t *sem, int *val) {
  SceKernelSemaInfo info;
  info.size = sizeof(info);
  sceKernelGetSemaInfo(*sem, &info);
  *val = info.currentCount;
  return 0;
}

void pthread_bridge_init(void) {
  sceKernelCreateLwMutex(&init_lock, "pthread_init", SCE_KERNEL_MUTEX_ATTR_RECURSIVE, 0, NULL);
  sceKernelCreateLwMutex(&once_lock, "pthread_once", SCE_KERNEL_MUTEX_ATTR_RECURSIVE, 0, NULL);
}

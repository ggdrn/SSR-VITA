#include "log.h"
#include "../config.h"

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/clib.h>
#include <psp2/message_dialog.h>
#include <vitaGL.h>
#include <stdio.h>
#include <string.h>

static SceUID log_fd = -1;
static SceKernelLwMutexWork log_lock, log_io_lock;
#define LOG_RING_SIZE (128 * 1024)
static char log_ring[LOG_RING_SIZE];
static unsigned log_head, log_tail, log_buffered, log_dropped, log_io_ms;

// Called with log_lock held. Never wait for storage in buffered mode.
static void output_locked(const char *buf, unsigned len) {
  if (__atomic_load_n(&log_buffered, __ATOMIC_ACQUIRE)) {
    if (len > LOG_RING_SIZE - (log_head - log_tail)) { __atomic_fetch_add(&log_dropped, 1, __ATOMIC_RELAXED); return; }
    unsigned first = LOG_RING_SIZE - log_head % LOG_RING_SIZE;
    if (first > len) first = len;
    memcpy(log_ring + log_head % LOG_RING_SIZE, buf, first);
    memcpy(log_ring, buf + first, len - first);
    log_head += len;
    return;
  }
  sceKernelLockLwMutex(&log_io_lock, 1, NULL);
  sceClibPrintf("%s", buf);
  if (log_fd >= 0) sceIoWrite(log_fd, buf, len);
  sceKernelUnlockLwMutex(&log_io_lock, 1);
}
void log_enable_buffered(void) {
  sceKernelLockLwMutex(&log_lock, 1, NULL);
  __atomic_store_n(&log_buffered, 1, __ATOMIC_RELEASE);
  sceKernelUnlockLwMutex(&log_lock, 1);
}
// Only the consumer/final shutdown takes log_io_lock. File I/O is always
// outside log_lock, so render/audio/network producers only enqueue bytes.
static unsigned drain_locked(unsigned budget) {
  char buf[16384]; unsigned total = 0;
  while (total < budget) {
    sceKernelLockLwMutex(&log_lock, 1, NULL);
    unsigned n = log_head - log_tail;
    if (n > sizeof(buf)) n = sizeof(buf);
    if (n > budget - total) n = budget - total;
    // Publish complete lines even if an application exits during draining.
    while (n && log_ring[(log_tail + n - 1) % LOG_RING_SIZE] != '\n') n--;
    if (!n) {
      sceKernelUnlockLwMutex(&log_lock, 1); break;
    }
    unsigned first = LOG_RING_SIZE - log_tail % LOG_RING_SIZE;
    if (first > n) first = n;
    memcpy(buf, log_ring + log_tail % LOG_RING_SIZE, first);
    memcpy(buf + first, log_ring, n - first);
    log_tail += n;
    sceKernelUnlockLwMutex(&log_lock, 1);
    uint64_t start = sceKernelGetProcessTimeWide();
    unsigned written = 0;
    while (log_fd >= 0 && written < n) {
      int ret = sceIoWrite(log_fd, buf + written, n - written);
      if (ret <= 0) { __atomic_fetch_add(&log_dropped, 1, __ATOMIC_RELAXED); break; }
      written += ret;
    }
    __atomic_fetch_add(&log_io_ms, (unsigned)((sceKernelGetProcessTimeWide() - start) / 1000), __ATOMIC_RELAXED);
    total += n;
  }
  return total;
}
void log_flush_pending(unsigned budget) {
  sceKernelLockLwMutex(&log_io_lock, 1, NULL);
  if (__atomic_load_n(&log_buffered, __ATOMIC_ACQUIRE)) drain_locked(budget);
  sceKernelUnlockLwMutex(&log_io_lock, 1);
}
void log_finish(void) {
  if (!__atomic_load_n(&log_buffered, __ATOMIC_ACQUIRE)) return;
  sceKernelLockLwMutex(&log_io_lock, 1, NULL);
  drain_locked(LOG_RING_SIZE + 16384);
  sceKernelLockLwMutex(&log_lock, 1, NULL);
  if (log_tail != log_head) __atomic_fetch_add(&log_dropped, 1, __ATOMIC_RELAXED);
  log_tail = log_head;
  __atomic_store_n(&log_buffered, 0, __ATOMIC_RELEASE);
  sceKernelUnlockLwMutex(&log_lock, 1);
  sceKernelUnlockLwMutex(&log_io_lock, 1);
}
unsigned log_dropped_records(void) { return __atomic_load_n(&log_dropped, __ATOMIC_RELAXED); }
unsigned log_storage_ms(void) { return __atomic_load_n(&log_io_ms, __ATOMIC_RELAXED); }

void log_init(void) {
  sceKernelCreateLwMutex(&log_lock, "log", 2, 0, NULL);
  sceKernelCreateLwMutex(&log_io_lock, "log_io", 2, 0, NULL);
  sceIoMkdir(DATA_PATH, 0777);
  log_fd = sceIoOpen(LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
}

void log_vprintf(const char *fmt, va_list ap) {
  char buf[1024];
  int len = vsnprintf(buf, sizeof(buf), fmt, ap);
  if (len < 0)
    return;
  if (len >= (int)sizeof(buf) - 1)
    len = sizeof(buf) - 2;
  if (len == 0 || buf[len - 1] != '\n') {
    buf[len++] = '\n';
    buf[len] = 0;
  }
  static char previous[1024];
  static unsigned repeats;
  sceKernelLockLwMutex(&log_lock, 1, NULL);
  if (strcmp(previous, buf) == 0) {
    ++repeats;
    if ((repeats & (repeats - 1)) != 0) {
      sceKernelUnlockLwMutex(&log_lock, 1);
      return;
    }
    len = snprintf(buf, sizeof(buf), "[INFO] previous message repeated %u times\n", repeats);
  } else {
    if (repeats) {
      char summary[96];
      int n = snprintf(summary, sizeof(summary), "[INFO] previous message total repeats %u\n", repeats);
      output_locked(summary, n);
    }
    strcpy(previous, buf);
    repeats = 0;
  }
  output_locked(buf, len);
  sceKernelUnlockLwMutex(&log_lock, 1);
}

void log_printf(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  log_vprintf(fmt, ap);
  va_end(ap);
}

void fatal_error(const char *fmt, ...) {
  char msg[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);

  log_printf("[FATAL] %s", msg);
  log_finish();
  sceKernelLockLwMutex(&log_io_lock, 1, NULL);
  if (log_fd >= 0) { sceIoClose(log_fd); log_fd = -1; }
  sceKernelUnlockLwMutex(&log_io_lock, 1);

  // vitaGL may not be up yet; bring up the minimum to show a dialog.
  vglInit(0);

  SceMsgDialogUserMessageParam user;
  memset(&user, 0, sizeof(user));
  user.buttonType = SCE_MSG_DIALOG_BUTTON_TYPE_OK;
  user.msg = (SceChar8 *)msg;

  SceMsgDialogParam param;
  sceMsgDialogParamInit(&param);
  param.mode = SCE_MSG_DIALOG_MODE_USER_MSG;
  param.userMsgParam = &user;
  sceMsgDialogInit(&param);

  while (sceMsgDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED) {
    glClear(GL_COLOR_BUFFER_BIT);
    vglSwapBuffers(GL_TRUE);
  }
  sceMsgDialogTerm();
  sceKernelExitProcess(0);
  while (1)
    ;
}

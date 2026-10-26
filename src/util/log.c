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
static SceKernelLwMutexWork log_lock;

void log_init(void) {
  sceKernelCreateLwMutex(&log_lock, "log", 2, 0, NULL);
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
  if (buf[len - 1] != '\n') {
    buf[len++] = '\n';
    buf[len] = 0;
  }
  sceClibPrintf("%s", buf);
  if (log_fd >= 0) {
    sceKernelLockLwMutex(&log_lock, 1, NULL);
    sceIoWrite(log_fd, buf, len);
    sceKernelUnlockLwMutex(&log_lock, 1);
  }
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
  if (log_fd >= 0)
    sceIoClose(log_fd);

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

// libandroid pieces: AAssetManager (served straight from the APK), native
// window, looper, sensors and configuration.

#include "bridges.h"
#include "../util/apk.h"
#include "../util/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/kernel/threadmgr.h>

int input_looper_poll(void **out_data);

// --- AAssetManager ----------------------------------------------------------

#define FAKE_ASSET_MANAGER ((void *)0xA55E7001)

typedef struct {
  const apk_entry *entry;
  uint8_t *data;   // inflated copy for compressed entries
  uint32_t pos;
} AAsset;

void *AAssetManager_fromJava_bridge(void *env, void *obj) { return FAKE_ASSET_MANAGER; }

AAsset *AAssetManager_open_bridge(void *mgr, const char *name, int mode) {
  char path[512];
  snprintf(path, sizeof(path), "assets/%s", name);
  const apk_entry *e = apk_find(path);
  if (!e) {
    L_DEBUG("AAssetManager_open(%s): not found", name);
    return NULL;
  }
  AAsset *a = calloc(1, sizeof(AAsset));
  a->entry = e;
  if (e->method != 0) {
    a->data = apk_extract(e, NULL);
    if (!a->data) {
      free(a);
      return NULL;
    }
  }
  return a;
}

int AAsset_read_bridge(AAsset *a, void *buf, size_t count) {
  uint32_t left = a->entry->size - a->pos;
  if (count > left)
    count = left;
  if (count == 0)
    return 0;
  if (a->data)
    memcpy(buf, a->data + a->pos, count);
  else if (apk_read_stored(a->entry, a->pos, buf, count) < 0)
    return -1;
  a->pos += count;
  return count;
}

int32_t AAsset_seek_bridge(AAsset *a, int32_t off, int whence) {
  int64_t pos = whence == 0 ? off : whence == 1 ? (int64_t)a->pos + off : (int64_t)a->entry->size + off;
  if (pos < 0 || pos > a->entry->size)
    return -1;
  a->pos = pos;
  return pos;
}

int32_t AAsset_getLength_bridge(AAsset *a) { return a->entry->size; }
int32_t AAsset_getRemainingLength_bridge(AAsset *a) { return a->entry->size - a->pos; }

void AAsset_close_bridge(AAsset *a) {
  free(a->data);
  free(a);
}

// --- AConfiguration ---------------------------------------------------------

void *AConfiguration_new_bridge(void) { return calloc(1, 64); }
void AConfiguration_delete_bridge(void *c) { free(c); }
void AConfiguration_fromAssetManager_bridge(void *c, void *mgr) {}

// --- ANativeWindow ----------------------------------------------------------

#define FAKE_WINDOW ((void *)0x3141D001)

void *ANativeWindow_fromSurface_bridge(void *env, void *surface) { return FAKE_WINDOW; }
void ANativeWindow_acquire_bridge(void *w) {}
void ANativeWindow_release_bridge(void *w) {}
int ANativeWindow_setBuffersGeometry_bridge(void *w, int width, int height, int format) { return 0; }

// --- ALooper ----------------------------------------------------------------

#define FAKE_LOOPER ((void *)0x100B0001)

void *ALooper_forThread_bridge(void) { return FAKE_LOOPER; }
void *ALooper_prepare_bridge(int opts) { return FAKE_LOOPER; }
void ALooper_wake_bridge(void *l) {}

int ALooper_pollAll_bridge(int timeout_ms, int *fd, int *events, void **data) {
  int r = input_looper_poll(data);
  if (r >= 0) {
    if (fd) *fd = 0;
    if (events) *events = 1;
    return r;
  }
  if (r == -2)
    return r;
  if (timeout_ms > 0)
    sceKernelDelayThread(timeout_ms * 1000 > 16000 ? 16000 : timeout_ms * 1000);
  return -3;  // ALOOPER_POLL_TIMEOUT
}

// --- ASensor (no sensors exposed; Unity treats them as unavailable) -------

#define FAKE_SENSOR_MANAGER ((void *)0x5E750001)
#define FAKE_SENSOR_QUEUE ((void *)0x5E750002)

void *ASensorManager_getInstance_bridge(void) { return FAKE_SENSOR_MANAGER; }
int ASensorManager_getSensorList_bridge(void *m, const void ***list) {
  static const void *empty[1] = {NULL};
  if (list)
    *list = empty;
  return 0;
}
const void *ASensorManager_getDefaultSensor_bridge(void *m, int type) { return NULL; }
void *ASensorManager_createEventQueue_bridge(void *m, void *looper, int ident, void *cb, void *data) {
  return FAKE_SENSOR_QUEUE;
}
int ASensorManager_destroyEventQueue_bridge(void *m, void *q) { return 0; }
int ASensorEventQueue_enableSensor_bridge(void *q, const void *s) { return -1; }
int ASensorEventQueue_disableSensor_bridge(void *q, const void *s) { return 0; }
int ASensorEventQueue_setEventRate_bridge(void *q, const void *s, int us) { return 0; }
int ASensorEventQueue_hasEvents_bridge(void *q) { return 0; }
int ASensorEventQueue_getEvents_bridge(void *q, void *events, size_t count) { return 0; }
const char *ASensor_getName_bridge(const void *s) { return "none"; }
const char *ASensor_getVendor_bridge(const void *s) { return "Sony"; }
int ASensor_getType_bridge(const void *s) { return 0; }
int ASensor_getMinDelay_bridge(const void *s) { return 0; }
SOFTFP float ASensor_getResolution_bridge(const void *s) { return 0.0f; }

// --- libmain watchdog helpers (C++ symbols imported by libunity) -----------

void ANativeActivity_feedWatchdog_bridge(void) {}
void ANativeActivity_enableWatchdog_bridge(int enable) {}
void ANativeActivity_setFirstLevelHasBeenLoaded_bridge(int loaded) {
  L_INFO("unity: first level loaded");
}
int ANativeActivity_forwardMotionEventToDalvik_bridge(void *event, void *env) { return 0; }

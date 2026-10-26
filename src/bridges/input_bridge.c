// Vita front touch + buttons -> Android NDK input events (AInputQueue).
// Unity reads input through AInputQueue/AMotionEvent/AKeyEvent, so we keep a
// small ring of synthetic events that the looper reports as ready.
//
// Sonic Runners is touch-only: Cross/Circle also inject a tap in the middle of
// the screen (jump), and Start/Select map to the Android BACK key.

#include "bridges.h"
#include "../config.h"
#include "../util/log.h"

#include <string.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/touch.h>

#define AINPUT_EVENT_TYPE_KEY 1
#define AINPUT_EVENT_TYPE_MOTION 2
#define AINPUT_SOURCE_TOUCHSCREEN 0x1002
#define AINPUT_SOURCE_GAMEPAD 0x401
#define AKEY_EVENT_ACTION_DOWN 0
#define AKEY_EVENT_ACTION_UP 1
#define AMOTION_EVENT_ACTION_DOWN 0
#define AMOTION_EVENT_ACTION_UP 1
#define AMOTION_EVENT_ACTION_MOVE 2
#define AMOTION_EVENT_ACTION_POINTER_DOWN 5
#define AMOTION_EVENT_ACTION_POINTER_UP 6
#define AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT 8

#define AKEYCODE_BACK 4
#define AKEYCODE_DPAD_UP 19
#define AKEYCODE_DPAD_DOWN 20
#define AKEYCODE_DPAD_LEFT 21
#define AKEYCODE_DPAD_RIGHT 22
#define AKEYCODE_BUTTON_A 96
#define AKEYCODE_BUTTON_B 97
#define AKEYCODE_BUTTON_X 99
#define AKEYCODE_BUTTON_Y 100
#define AKEYCODE_BUTTON_L1 102
#define AKEYCODE_BUTTON_R1 103
#define AKEYCODE_BUTTON_START 108
#define AKEYCODE_BUTTON_SELECT 109

#define MAX_POINTERS 6
#define QUEUE_SIZE 64

typedef struct {
  int id;
  float x, y;
} pointer;

typedef struct {
  int type;
  int source;
  int action;
  int keycode;
  int pointer_count;
  pointer pointers[MAX_POINTERS];
  int64_t event_time;  // ns
  int64_t down_time;
} input_event;

static input_event queue[QUEUE_SIZE];
static int q_head, q_tail;
static SceKernelLwMutexWork q_lock;

// Events handed to the game stay valid until finishEvent; keep a separate pool.
static input_event in_flight[QUEUE_SIZE];
static int in_flight_idx;

static int64_t now_ns(void) { return (int64_t)sceKernelGetProcessTimeWide() * 1000; }

static void push_event(const input_event *e) {
  sceKernelLockLwMutex(&q_lock, 1, NULL);
  int next = (q_tail + 1) % QUEUE_SIZE;
  if (next != q_head) {
    queue[q_tail] = *e;
    q_tail = next;
  }
  sceKernelUnlockLwMutex(&q_lock, 1);
}

int input_queue_pending(void) { return q_head != q_tail; }

// --- AInputQueue ------------------------------------------------------------

#define FAKE_QUEUE ((void *)0x1A9E0001)

typedef int (*ALooper_callbackFunc)(int fd, int events, void *data);
static int attached_ident = -1;
static ALooper_callbackFunc attached_cb;
static void *attached_data;

void *ANativeActivity_getInputQueue_bridge(void) { return FAKE_QUEUE; }

void AInputQueue_attachLooper_bridge(void *q, void *looper, int ident, ALooper_callbackFunc cb, void *data) {
  attached_ident = ident;
  attached_cb = cb;
  attached_data = data;
  L_INFO("input: queue attached (ident %d, callback %p)", ident, cb);
}

void AInputQueue_detachLooper_bridge(void *q) { attached_ident = -1; attached_cb = NULL; }

int AInputQueue_getEvent_bridge(void *q, input_event **out) {
  sceKernelLockLwMutex(&q_lock, 1, NULL);
  if (q_head == q_tail) {
    sceKernelUnlockLwMutex(&q_lock, 1);
    return -1;
  }
  input_event *e = &in_flight[in_flight_idx];
  in_flight_idx = (in_flight_idx + 1) % QUEUE_SIZE;
  *e = queue[q_head];
  q_head = (q_head + 1) % QUEUE_SIZE;
  sceKernelUnlockLwMutex(&q_lock, 1);
  *out = e;
  return 0;
}

int AInputQueue_preDispatchEvent_bridge(void *q, input_event *e) { return 0; }
void AInputQueue_finishEvent_bridge(void *q, input_event *e, int handled) {}

// Called by ALooper_pollAll: returns the ident when input is ready.
int input_looper_poll(void **out_data) {
  if (attached_ident < 0 && !attached_cb)
    return -3;  // ALOOPER_POLL_TIMEOUT
  if (!input_queue_pending())
    return -3;
  if (attached_cb) {
    attached_cb(0, 1, attached_data);
    return -2;  // ALOOPER_POLL_CALLBACK
  }
  if (out_data)
    *out_data = attached_data;
  return attached_ident;
}

// --- AInputEvent / AKeyEvent / AMotionEvent ---------------------------------

int AInputEvent_getType_bridge(const input_event *e) { return e->type; }
int AInputEvent_getSource_bridge(const input_event *e) { return e->source; }
int AInputEvent_getDeviceId_bridge(const input_event *e) { return e->type == AINPUT_EVENT_TYPE_KEY ? 2 : 1; }

int AKeyEvent_getAction_bridge(const input_event *e) { return e->action; }
int AKeyEvent_getKeyCode_bridge(const input_event *e) { return e->keycode; }
int AKeyEvent_getMetaState_bridge(const input_event *e) { return 0; }

int AMotionEvent_getAction_bridge(const input_event *e) { return e->action; }
int AMotionEvent_getFlags_bridge(const input_event *e) { return 0; }
int AMotionEvent_getMetaState_bridge(const input_event *e) { return 0; }
int AMotionEvent_getButtonState_bridge(const input_event *e) { return 0; }
int AMotionEvent_getEdgeFlags_bridge(const input_event *e) { return 0; }
int64_t AMotionEvent_getDownTime_bridge(const input_event *e) { return e->down_time; }
int64_t AMotionEvent_getEventTime_bridge(const input_event *e) { return e->event_time; }
SOFTFP float AMotionEvent_getXPrecision_bridge(const input_event *e) { return 1.0f; }
SOFTFP float AMotionEvent_getYPrecision_bridge(const input_event *e) { return 1.0f; }
size_t AMotionEvent_getPointerCount_bridge(const input_event *e) { return e->pointer_count; }
int AMotionEvent_getPointerId_bridge(const input_event *e, size_t i) { return e->pointers[i].id; }
int AMotionEvent_getToolType_bridge(const input_event *e, size_t i) { return 1; }  // FINGER
SOFTFP float AMotionEvent_getX_bridge(const input_event *e, size_t i) { return e->pointers[i].x; }
SOFTFP float AMotionEvent_getY_bridge(const input_event *e, size_t i) { return e->pointers[i].y; }
SOFTFP float AMotionEvent_getRawX_bridge(const input_event *e, size_t i) { return e->pointers[i].x; }
SOFTFP float AMotionEvent_getRawY_bridge(const input_event *e, size_t i) { return e->pointers[i].y; }
SOFTFP float AMotionEvent_getPressure_bridge(const input_event *e, size_t i) { return 1.0f; }
SOFTFP float AMotionEvent_getSize_bridge(const input_event *e, size_t i) { return 0.1f; }
SOFTFP float AMotionEvent_getTouchMajor_bridge(const input_event *e, size_t i) { return 8.0f; }
SOFTFP float AMotionEvent_getTouchMinor_bridge(const input_event *e, size_t i) { return 8.0f; }
SOFTFP float AMotionEvent_getToolMajor_bridge(const input_event *e, size_t i) { return 8.0f; }
SOFTFP float AMotionEvent_getToolMinor_bridge(const input_event *e, size_t i) { return 8.0f; }
SOFTFP float AMotionEvent_getOrientation_bridge(const input_event *e, size_t i) { return 0.0f; }
SOFTFP float AMotionEvent_getAxisValue_bridge(const input_event *e, int axis, size_t i) {
  return axis == 0 ? e->pointers[i].x : axis == 1 ? e->pointers[i].y : 0.0f;
}
size_t AMotionEvent_getHistorySize_bridge(const input_event *e) { return 0; }
int64_t AMotionEvent_getHistoricalEventTime_bridge(const input_event *e, size_t h) { return e->event_time; }
SOFTFP float AMotionEvent_getHistoricalX_bridge(const input_event *e, size_t i, size_t h) { return e->pointers[i].x; }
SOFTFP float AMotionEvent_getHistoricalY_bridge(const input_event *e, size_t i, size_t h) { return e->pointers[i].y; }
SOFTFP float AMotionEvent_getHistoricalPressure_bridge(const input_event *e, size_t i, size_t h) { return 1.0f; }
SOFTFP float AMotionEvent_getHistoricalSize_bridge(const input_event *e, size_t i, size_t h) { return 0.1f; }

// --- Vita polling -----------------------------------------------------------

typedef struct {
  int active;
  int id;  // Android pointer id
  float x, y;
} touch_slot;

static touch_slot slots[MAX_POINTERS];
static uint32_t prev_buttons;
static int64_t touch_down_time;
static int next_pointer_id;

// Virtual tap pointer driven by Cross/Circle.
#define VIRTUAL_TAP_SLOT (MAX_POINTERS - 1)

static void build_motion(input_event *e, int action) {
  memset(e, 0, sizeof(*e));
  e->type = AINPUT_EVENT_TYPE_MOTION;
  e->source = AINPUT_SOURCE_TOUCHSCREEN;
  e->action = action;
  e->event_time = now_ns();
  e->down_time = touch_down_time;
  for (int i = 0; i < MAX_POINTERS; i++) {
    if (slots[i].active) {
      pointer *p = &e->pointers[e->pointer_count++];
      p->id = slots[i].id;
      p->x = slots[i].x;
      p->y = slots[i].y;
    }
  }
}

static int pointer_index(int slot) {
  int idx = 0;
  for (int i = 0; i < slot; i++)
    idx += slots[i].active;
  return idx;
}

static int active_count(void) {
  int n = 0;
  for (int i = 0; i < MAX_POINTERS; i++)
    n += slots[i].active;
  return n;
}

static void pointer_down(int slot, float x, float y) {
  int was_empty = active_count() == 0;
  if (was_empty)
    touch_down_time = now_ns();
  slots[slot].active = 1;
  slots[slot].id = next_pointer_id++ & 0xff;
  slots[slot].x = x;
  slots[slot].y = y;
  input_event e;
  int action = was_empty ? AMOTION_EVENT_ACTION_DOWN
                         : AMOTION_EVENT_ACTION_POINTER_DOWN | (pointer_index(slot) << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
  build_motion(&e, action);
  push_event(&e);
}

static void pointer_up(int slot) {
  input_event e;
  int last = active_count() == 1;
  int action = last ? AMOTION_EVENT_ACTION_UP
                    : AMOTION_EVENT_ACTION_POINTER_UP | (pointer_index(slot) << AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
  build_motion(&e, action);
  push_event(&e);
  slots[slot].active = 0;
}

static void push_key(int keycode, int down) {
  input_event e;
  memset(&e, 0, sizeof(e));
  e.type = AINPUT_EVENT_TYPE_KEY;
  e.source = AINPUT_SOURCE_GAMEPAD;
  e.action = down ? AKEY_EVENT_ACTION_DOWN : AKEY_EVENT_ACTION_UP;
  e.keycode = keycode;
  e.event_time = e.down_time = now_ns();
  push_event(&e);
}

static const struct {
  uint32_t vita;
  int android;
} key_map[] = {
    {SCE_CTRL_CROSS, AKEYCODE_BUTTON_A},       {SCE_CTRL_CIRCLE, AKEYCODE_BUTTON_B},
    {SCE_CTRL_SQUARE, AKEYCODE_BUTTON_X},      {SCE_CTRL_TRIANGLE, AKEYCODE_BUTTON_Y},
    {SCE_CTRL_LTRIGGER, AKEYCODE_BUTTON_L1},   {SCE_CTRL_RTRIGGER, AKEYCODE_BUTTON_R1},
    {SCE_CTRL_UP, AKEYCODE_DPAD_UP},           {SCE_CTRL_DOWN, AKEYCODE_DPAD_DOWN},
    {SCE_CTRL_LEFT, AKEYCODE_DPAD_LEFT},       {SCE_CTRL_RIGHT, AKEYCODE_DPAD_RIGHT},
    {SCE_CTRL_START, AKEYCODE_BACK},           {SCE_CTRL_SELECT, AKEYCODE_BACK},
};

void input_poll(void) {
  // Touch: front panel is 1920x1088.
  SceTouchData touch;
  if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1) >= 0) {
    int seen[MAX_POINTERS - 1] = {0};
    for (int r = 0; r < touch.reportNum && r < MAX_POINTERS - 1; r++) {
      int slot = touch.report[r].id % (MAX_POINTERS - 1);
      float x = touch.report[r].x * SCREEN_W / 1920.0f;
      float y = touch.report[r].y * SCREEN_H / 1088.0f;
      seen[slot] = 1;
      if (!slots[slot].active) {
        pointer_down(slot, x, y);
      } else if (slots[slot].x != x || slots[slot].y != y) {
        slots[slot].x = x;
        slots[slot].y = y;
        input_event e;
        build_motion(&e, AMOTION_EVENT_ACTION_MOVE);
        push_event(&e);
      }
    }
    for (int s = 0; s < MAX_POINTERS - 1; s++) {
      if (slots[s].active && !seen[s])
        pointer_up(s);
    }
  }

  SceCtrlData pad;
  sceCtrlPeekBufferPositive(0, &pad, 1);
  uint32_t changed = pad.buttons ^ prev_buttons;
  for (size_t i = 0; i < sizeof(key_map) / sizeof(key_map[0]); i++) {
    if (changed & key_map[i].vita)
      push_key(key_map[i].android, pad.buttons & key_map[i].vita);
  }

  int tap = pad.buttons & (SCE_CTRL_CROSS | SCE_CTRL_CIRCLE);
  int prev_tap = prev_buttons & (SCE_CTRL_CROSS | SCE_CTRL_CIRCLE);
  if (tap && !prev_tap)
    pointer_down(VIRTUAL_TAP_SLOT, SCREEN_W / 2.0f, SCREEN_H / 2.0f);
  else if (!tap && prev_tap && slots[VIRTUAL_TAP_SLOT].active)
    pointer_up(VIRTUAL_TAP_SLOT);

  prev_buttons = pad.buttons;
}

void input_init(void) {
  sceKernelCreateLwMutex(&q_lock, "input", 0, 0, NULL);
  sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);
  sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
}

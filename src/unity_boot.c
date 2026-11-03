#include "util/observe.h"
#include "util/work_trace.h"
#include "util/managed_trace.h"
// Replays what the Java side of a Unity 4.x Android player does:
//   System.loadLibrary("main")        -> libmain JNI_OnLoad
//   NativeLoader.load(libDir)         -> libmain loads libmono/libunity
//   new UnityPlayer(activity)         -> initJni, nativeFile, ...
//   GL thread                         -> nativeRecreateGfxState, nativeRender loop
// Natives are looked up by name from RegisterNatives, and their signatures are
// logged so the sequence can be adjusted from a device log.

#include "config.h"
#include "bridges/bridges.h"
#include "jni/jni.h"
#include "loader/so_util.h"
#include "util/log.h"
#include "port_menu.h"

#include <string.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>

#define UNITY_PLAYER "com/unity3d/player/UnityPlayer"
#define NATIVE_LOADER "com/unity3d/player/NativeLoader"

void audio_fmod_start(void);
void mono_probe_tick(unsigned frame);
void mono_perf_tick(uint64_t render_us, uint64_t frame_us, unsigned frame);
void mono_audio_tick(void);
void mono_menu_fix(void);
void mono_texture_budget_tick(void);
void mono_gc_policy_tick(void);
void mono_observe_tick(void);
void mono_input_tick(void);
void audio_fmod_stop(void);

static void *env;
static jobj *player;

static void *native(const char *cls, const char *name) {
  void *fn = jni_find_native(cls, name);
  if (!fn)
    L_WARN("unity: native %s.%s not registered", cls, name);
  return fn;
}

static void call_void(const char *name) {
  void (*fn)(void *, jobj *) = native(UNITY_PLAYER, name);
  if (fn) {
    L_INFO("unity: %s()", name);
    uint64_t start = observe_now();
    fn(env, player);
    observe_span(OBS_BOOT, name, start, 0, 0, 0);
  }
}

static void call_bool_arg(const char *name, int v) {
  void (*fn)(void *, jobj *, int) = native(UNITY_PLAYER, name);
  if (fn) {
    L_INFO("unity: %s(%d)", name, v);
    fn(env, player, v);
  }
}

volatile int unity_resize_requested;

// The Java surface callback reports the window size; without it Unity renders
// into a tiny default viewport (the grey square in the bottom-left corner).
void unity_resize(void) {
  void (*resize)(void *, jobj *, int, int, int, int) = native(UNITY_PLAYER, "nativeResize");
  if (resize) {
    L_INFO("unity: nativeResize(%d, %d)", SCREEN_W, SCREEN_H);
    resize(env, player, SCREEN_W, SCREEN_H, SCREEN_W, SCREEN_H);
  }
}

// What Android's surfaceChanged does: hand Unity the surface again, then the
// size. A resize alone (triggerResizeCall) makes Unity release its EGL
// surface and wait for a new one; without it, it stops rendering the scene.
static void unity_surface_changed(void) {
  static jobj *surface;
  if (!surface)
    surface = jni_new_object("android/view/Surface");
  const char *sig = jni_native_signature(UNITY_PLAYER, "nativeRecreateGfxState");
  void *recreate = native(UNITY_PLAYER, "nativeRecreateGfxState");
  if (recreate) {
    L_INFO("unity: nativeRecreateGfxState%s", sig ? sig : "");
    if (sig && strncmp(sig, "(I", 2) == 0)
      ((void (*)(void *, jobj *, int, jobj *))recreate)(env, player, 0, surface);
    else
      ((void (*)(void *, jobj *, jobj *))recreate)(env, player, surface);
  }
  unity_resize();
}

static int jni_onload(const char *lib) {
  so_module *m = so_find_module(lib);
  int (*onload)(void *, void *) = m ? (void *)so_symbol(m, "JNI_OnLoad") : NULL;
  if (!onload)
    return -1;
  uint64_t start = observe_now();
  int v = onload(jni_get_vm(), NULL);
  observe_span(OBS_BOOT, lib, start, 0, v, 0);
  L_INFO("unity: %s JNI_OnLoad -> 0x%X", lib, v);
  return v;
}

int unity_run(void) {
  env = jni_get_env();
  player = jni_new_object(UNITY_PLAYER);

  // 1. libmain registers NativeLoader.load and loads the engine through it.
  jni_onload("libmain.so");
  uint8_t (*loader_load)(void *, jobj *, jobj *) = native(NATIVE_LOADER, "load");
  if (loader_load) {
    int ok = loader_load(env, jni_find_class(NATIVE_LOADER), jni_new_string(ANDROID_APK_DIR "/lib"));
    L_INFO("unity: NativeLoader.load -> %d", ok);
  } else {
    // Fallback: initialise libunity ourselves.
    jni_onload("libunity.so");
  }
  jni_onload("libcri_ware_unity.so");

  // 2. UnityPlayer construction.
  void (*initJni)(void *, jobj *, jobj *) = native(UNITY_PLAYER, "initJni");
  if (initJni) {
    L_INFO("unity: initJni()");
    uint64_t start = observe_now();
    initJni(env, player, jni_new_object("com/unity3d/player/UnityPlayerNativeActivity"));
    observe_span(OBS_BOOT, "unity.initJni", start, 0, 0, 0);
  }
  void (*nativeFile)(void *, jobj *, jobj *) = native(UNITY_PLAYER, "nativeFile");
  if (nativeFile) {
    L_INFO("unity: nativeFile(%s)", fs_apk_android_path());
    uint64_t start = observe_now();
    nativeFile(env, player, jni_new_string(fs_apk_android_path()));
    observe_span(OBS_BOOT, "unity.nativeFile", start, 0, 0, 0);
  }

  // UnityPlayer's constructor hands Unity the WWW class; without it the first
  // WWW request dereferences a null constructor ID.
  void (*initWWW)(void *, jobj *, jobj *) = native(UNITY_PLAYER, "nativeInitWWW");
  if (initWWW) {
    L_INFO("unity: nativeInitWWW()");
    initWWW(env, player, jni_find_class("com/unity3d/player/WWW"));
  }

  // 3. Surface + resume.
  unity_surface_changed();
  call_void("nativeResume");
  call_bool_arg("nativeFocusChanged", 1);

  // 4. Audio device (FMOD Java pump).
  int (*fmodInitJni)(void *, jobj *) = native("org/fmod/FMODAudioDevice", "fmodInitJni");
  if (fmodInitJni) {
    jobj *dev = jni_new_object("org/fmod/FMODAudioDevice");
    L_INFO("unity: fmodInitJni -> %d", fmodInitJni(env, dev));
  }
  audio_fmod_start();

  // 5. Render loop.
  uint8_t (*render)(void *, jobj *) = native(UNITY_PLAYER, "nativeRender");
  if (!render)
    fatal_error("Unity did not register nativeRender.\nSee " LOG_PATH);
  log_heap_usage("before render loop");
  L_INFO("unity: entering render loop");
  observe_mark("render-loop.begin");
  unsigned frame = 0;
  void (*set_input_string)(void *, jobj *, jobj *) = native(UNITY_PLAYER, "nativeSetInputString");
  void (*soft_input_closed)(void *, jobj *) = native(UNITY_PLAYER, "nativeSoftInputClosed");
  for (;;) {
    observe_begin_frame(frame + 1);
    work_trace_frame_begin();
    managed_trace_frame_begin(frame + 1);
    uint64_t frame_start = sceKernelGetProcessTimeWide();
    mono_input_tick();
    // The port menu (L + R + START) and the system keyboard own the controls
    // while they are open.
    if (!port_menu_tick() && !ime_running())
      input_poll();
    const char *typed = ime_poll();
    if (typed) {
      if (set_input_string)
        set_input_string(env, player, jni_new_string(typed));
      if (soft_input_closed)
        soft_input_closed(env, player);
    }
    if (unity_resize_requested) {
      unity_resize_requested = 0;
      unity_surface_changed();
    }
    if (frame < 300 && frame % 30 == 1) mono_menu_fix();
    if (frame % 30 == 1) {
      mono_texture_budget_tick();
      mono_port_quality_apply();
      mono_gc_policy_tick();
    }
    uint64_t render_start = sceKernelGetProcessTimeWide();
    if (!render(env, player)) {
      L_INFO("unity: nativeRender returned false after %u frames", frame);
      break;
    }
    uint64_t render_us = sceKernelGetProcessTimeWide() - render_start;
    managed_trace_frame_end(render_us);
    static uint64_t total_us, worst_us;
    static unsigned slow_frames;
    total_us += render_us;
    if (render_us > worst_us) worst_us = render_us;
    if (render_us > 50000) slow_frames++;
    watchdog_frames = ++frame;
    if (frame % 600 == 0) {
      L_INFO("perf: 600 renders avg %.2f ms max %.2f ms >50ms %u", total_us / 600000.0, worst_us / 1000.0, slow_frames);
      total_us = worst_us = 0;
      slow_frames = 0;
    }
    if (frame == 1 || frame % 600 == 0)
      L_INFO("unity: frame %u", frame);
    if (frame % 15 == 1) mono_observe_tick();
    if (frame % 30 == 1) mono_audio_tick();
    if (frame % 300 == 0)
      mono_probe_tick(frame);
    mono_perf_tick(render_us, sceKernelGetProcessTimeWide() - frame_start, frame);
  }
  call_void("nativeDone");
  // Let the audio thread leave its wait loop before the process goes away.
  audio_fmod_stop();
  sceKernelDelayThread(50 * 1000);
  return 0;
}

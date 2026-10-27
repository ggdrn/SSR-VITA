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

#include <string.h>
#include <psp2/kernel/threadmgr.h>

#define UNITY_PLAYER "com/unity3d/player/UnityPlayer"
#define NATIVE_LOADER "com/unity3d/player/NativeLoader"

void audio_fmod_start(void);
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
    fn(env, player);
  }
}

static void call_bool_arg(const char *name, int v) {
  void (*fn)(void *, jobj *, int) = native(UNITY_PLAYER, name);
  if (fn) {
    L_INFO("unity: %s(%d)", name, v);
    fn(env, player, v);
  }
}

static int jni_onload(const char *lib) {
  so_module *m = so_find_module(lib);
  int (*onload)(void *, void *) = m ? (void *)so_symbol(m, "JNI_OnLoad") : NULL;
  if (!onload)
    return -1;
  int v = onload(jni_get_vm(), NULL);
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
    initJni(env, player, jni_new_object("com/unity3d/player/UnityPlayerNativeActivity"));
  }
  void (*nativeFile)(void *, jobj *, jobj *) = native(UNITY_PLAYER, "nativeFile");
  if (nativeFile) {
    L_INFO("unity: nativeFile(%s)", fs_apk_android_path());
    nativeFile(env, player, jni_new_string(fs_apk_android_path()));
  }

  // UnityPlayer's constructor hands Unity the WWW class; without it the first
  // WWW request dereferences a null constructor ID.
  void (*initWWW)(void *, jobj *, jobj *) = native(UNITY_PLAYER, "nativeInitWWW");
  if (initWWW) {
    L_INFO("unity: nativeInitWWW()");
    initWWW(env, player, jni_find_class("com/unity3d/player/WWW"));
  }

  // 3. Surface + resume.
  const char *sig = jni_native_signature(UNITY_PLAYER, "nativeRecreateGfxState");
  void *recreate = native(UNITY_PLAYER, "nativeRecreateGfxState");
  if (recreate) {
    jobj *surface = jni_new_object("android/view/Surface");
    L_INFO("unity: nativeRecreateGfxState%s", sig ? sig : "");
    if (sig && strncmp(sig, "(I", 2) == 0)
      ((void (*)(void *, jobj *, int, jobj *))recreate)(env, player, 0, surface);
    else
      ((void (*)(void *, jobj *, jobj *))recreate)(env, player, surface);
  }
  call_void("nativeResume");
  call_bool_arg("nativeFocusChanged", 1);

  // 4. Audio device (FMOD Java pump).
  int (*fmodInitJni)(void *, jobj *) = (void *)so_symbol_global("Java_org_fmod_FMODAudioDevice_fmodInitJni");
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
  unsigned frame = 0;
  for (;;) {
    input_poll();
    if (!render(env, player)) {
      L_INFO("unity: nativeRender returned false after %u frames", frame);
      break;
    }
    watchdog_frames = ++frame;
    if (frame == 1 || frame % 600 == 0)
      L_INFO("unity: frame %u", frame);
  }
  call_void("nativeDone");
  // Let the audio thread leave its wait loop before the process goes away.
  audio_fmod_stop();
  sceKernelDelayThread(50 * 1000);
  return 0;
}

// Emulated Android/Java APIs the game calls through JNI.
// Anything not here falls back to the permissive defaults in jni.c and shows up
// in the log as "jni: unimplemented ...".

#include "jni.h"
#include "../bridges/bridges.h"
#include "../config.h"
#include "../util/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/apputil.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/system_param.h>

void audio_fmod_start(void);
void audio_fmod_stop(void);

#define RET_VOID ({ jvalue _r; _r.j = 0; _r; })
#define RET_INT(v) ({ jvalue _r; _r.j = 0; _r.i = (v); _r; })
#define RET_BOOL(v) ({ jvalue _r; _r.j = 0; _r.z = (v); _r; })
#define RET_OBJ(v) ({ jvalue _r; _r.j = 0; _r.l = (v); _r; })
#define RET_STR(s) RET_OBJ(jni_new_string(s))

static jobj *file_object(const char *path) {
  jobj *f = jni_new_object("java/io/File");
  f->native = strdup(path);
  return f;
}

// --- Context / Activity -----------------------------------------------------

static jvalue ctx_getPackageName(jobj *self, const jvalue *a) { return RET_STR(PACKAGE_NAME); }
static jvalue ctx_getPackageCodePath(jobj *self, const jvalue *a) { return RET_STR(fs_apk_android_path()); }
static jvalue ctx_getFilesDir(jobj *self, const jvalue *a) { return RET_OBJ(file_object(ANDROID_DATA_DIR "/files")); }
static jvalue ctx_getCacheDir(jobj *self, const jvalue *a) { return RET_OBJ(file_object(ANDROID_DATA_DIR "/cache")); }
static jvalue ctx_getExternalFilesDir(jobj *self, const jvalue *a) {
  return RET_OBJ(file_object(ANDROID_DATA_DIR "/files"));
}
static jvalue env_getExternalStorageDirectory(jobj *self, const jvalue *a) {
  return RET_OBJ(file_object(ANDROID_SDCARD));
}
static jvalue env_getExternalStorageState(jobj *self, const jvalue *a) { return RET_STR("mounted"); }

static jvalue file_getPath(jobj *self, const jvalue *a) {
  return RET_STR(self && self->native ? (const char *)self->native : ANDROID_DATA_DIR "/files");
}

static jvalue ctx_getApplicationInfo(jobj *self, const jvalue *a) {
  return RET_OBJ(jni_new_object("android/content/pm/ApplicationInfo"));
}

static jvalue ret_true(jobj *self, const jvalue *a) { return RET_BOOL(1); }
static jvalue ret_false(jobj *self, const jvalue *a) { return RET_BOOL(0); }
static jvalue ret_zero(jobj *self, const jvalue *a) { return RET_INT(0); }
static jvalue ret_void(jobj *self, const jvalue *a) { return RET_VOID; }
static jvalue ret_null(jobj *self, const jvalue *a) { return RET_OBJ(NULL); }

// --- Display ----------------------------------------------------------------

static jvalue disp_getWidth(jobj *self, const jvalue *a) { return RET_INT(SCREEN_W); }
static jvalue disp_getHeight(jobj *self, const jvalue *a) { return RET_INT(SCREEN_H); }

// --- Locale -----------------------------------------------------------------

static const char *vita_language(void) {
  int lang = SCE_SYSTEM_PARAM_LANG_ENGLISH_US;
  sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, &lang);
  switch (lang) {
  case SCE_SYSTEM_PARAM_LANG_JAPANESE: return "ja";
  case SCE_SYSTEM_PARAM_LANG_FRENCH: return "fr";
  case SCE_SYSTEM_PARAM_LANG_SPANISH: return "es";
  case SCE_SYSTEM_PARAM_LANG_GERMAN: return "de";
  case SCE_SYSTEM_PARAM_LANG_ITALIAN: return "it";
  case SCE_SYSTEM_PARAM_LANG_PORTUGUESE_PT:
  case SCE_SYSTEM_PARAM_LANG_PORTUGUESE_BR: return "pt";
  case SCE_SYSTEM_PARAM_LANG_RUSSIAN: return "ru";
  case SCE_SYSTEM_PARAM_LANG_KOREAN: return "ko";
  case SCE_SYSTEM_PARAM_LANG_CHINESE_T:
  case SCE_SYSTEM_PARAM_LANG_CHINESE_S: return "zh";
  default: return "en";
  }
}

static jvalue locale_getLanguage(jobj *self, const jvalue *a) { return RET_STR(vita_language()); }
static jvalue locale_getCountry(jobj *self, const jvalue *a) { return RET_STR("US"); }
static jvalue locale_toString(jobj *self, const jvalue *a) {
  static char buf[16];
  snprintf(buf, sizeof(buf), "%s_US", vita_language());
  return RET_STR(buf);
}

// --- Settings / telephony ---------------------------------------------------

static jvalue secure_getString(jobj *self, const jvalue *a) {
  const char *key = jni_string_utf8(a[1].l);
  if (strcmp(key, "android_id") == 0)
    return RET_STR("5a17a5f0c0ffee01");
  return RET_STR("");
}
static jvalue tel_getDeviceId(jobj *self, const jvalue *a) { return RET_STR("358240051111110"); }

// --- SharedPreferences (Unity PlayerPrefs) ---------------------------------
// Stored as "type key=value" lines in ux0:data/srr/files/shared_prefs/<name>.txt

typedef struct pref {
  char *key;
  char type;  // 'i', 'f', 's', 'b', 'l'
  char *value;
  struct pref *next;
} pref;

typedef struct {
  char *path;
  pref *head;
} prefs_file;

static pref *pref_find(prefs_file *pf, const char *key) {
  for (pref *p = pf->head; p; p = p->next) {
    if (strcmp(p->key, key) == 0)
      return p;
  }
  return NULL;
}

static void pref_set(prefs_file *pf, const char *key, char type, const char *value) {
  pref *p = pref_find(pf, key);
  if (!p) {
    p = calloc(1, sizeof(pref));
    p->key = strdup(key);
    p->next = pf->head;
    pf->head = p;
  } else {
    free(p->value);
  }
  p->type = type;
  p->value = strdup(value);
}

static void pref_remove(prefs_file *pf, const char *key) {
  for (pref **pp = &pf->head; *pp; pp = &(*pp)->next) {
    if (strcmp((*pp)->key, key) == 0) {
      pref *p = *pp;
      *pp = p->next;
      free(p->key);
      free(p->value);
      free(p);
      return;
    }
  }
}

// Escapes newlines/backslashes so values survive the line format.
static void write_escaped(FILE *f, const char *s) {
  for (; *s; s++) {
    if (*s == '\n') fputs("\\n", f);
    else if (*s == '\\') fputs("\\\\", f);
    else fputc(*s, f);
  }
}

static char *unescape(char *s) {
  char *w = s;
  for (char *r = s; *r; r++) {
    if (*r == '\\' && r[1]) {
      r++;
      *w++ = *r == 'n' ? '\n' : *r;
    } else {
      *w++ = *r;
    }
  }
  *w = 0;
  return s;
}

static void prefs_save(prefs_file *pf) {
  char tmp[300];
  snprintf(tmp, sizeof(tmp), "%s.tmp", pf->path);
  FILE *f = fopen(tmp, "w");
  if (!f) {
    L_ERROR("prefs: cannot write %s", tmp);
    return;
  }
  for (pref *p = pf->head; p; p = p->next) {
    fprintf(f, "%c ", p->type);
    write_escaped(f, p->key);
    fputc('=', f);
    write_escaped(f, p->value);
    fputc('\n', f);
  }
  fclose(f);
  sceIoRemove(pf->path);
  sceIoRename(tmp, pf->path);
}

static void prefs_load(prefs_file *pf) {
  FILE *f = fopen(pf->path, "r");
  if (!f)
    return;
  static char line[16384];
  while (fgets(line, sizeof(line), f)) {
    size_t n = strlen(line);
    if (n && line[n - 1] == '\n')
      line[--n] = 0;
    if (n < 3 || line[1] != ' ')
      continue;
    char *eq = line + 2;
    // Split on the first '=' that is not escaped.
    while (*eq && *eq != '=') {
      if (*eq == '\\' && eq[1]) eq++;
      eq++;
    }
    if (!*eq)
      continue;
    *eq = 0;
    pref_set(pf, unescape(line + 2), line[0], unescape(eq + 1));
  }
  fclose(f);
}

#define MAX_PREFS_FILES 16
static prefs_file prefs_files[MAX_PREFS_FILES];
static int num_prefs_files;

static prefs_file *prefs_open(const char *name) {
  char path[256];
  snprintf(path, sizeof(path), FILES_PATH "/shared_prefs/%s.txt", name);
  for (int i = 0; i < num_prefs_files; i++) {
    if (strcmp(prefs_files[i].path, path) == 0)
      return &prefs_files[i];
  }
  if (num_prefs_files >= MAX_PREFS_FILES)
    return &prefs_files[0];
  prefs_file *pf = &prefs_files[num_prefs_files++];
  pf->path = strdup(path);
  pf->head = NULL;
  sceIoMkdir(FILES_PATH "/shared_prefs", 0777);
  prefs_load(pf);
  L_INFO("prefs: opened %s", path);
  return pf;
}

static prefs_file *prefs_of(jobj *self) { return self && self->native ? self->native : prefs_open("default"); }

static jvalue ctx_getSharedPreferences(jobj *self, const jvalue *a) {
  jobj *o = jni_new_object("android/content/SharedPreferences");
  o->native = prefs_open(jni_string_utf8(a[0].l));
  return RET_OBJ(o);
}

static jvalue prefs_edit(jobj *self, const jvalue *a) {
  jobj *o = jni_new_object("android/content/SharedPreferences$Editor");
  o->native = prefs_of(self);
  return RET_OBJ(o);
}

static jvalue prefs_contains(jobj *self, const jvalue *a) {
  return RET_BOOL(pref_find(prefs_of(self), jni_string_utf8(a[0].l)) != NULL);
}

static jvalue prefs_getInt(jobj *self, const jvalue *a) {
  pref *p = pref_find(prefs_of(self), jni_string_utf8(a[0].l));
  return RET_INT(p ? atoi(p->value) : a[1].i);
}

static jvalue prefs_getLong(jobj *self, const jvalue *a) {
  pref *p = pref_find(prefs_of(self), jni_string_utf8(a[0].l));
  jvalue r;
  r.j = p ? strtoll(p->value, NULL, 10) : a[1].j;
  return r;
}

static jvalue prefs_getBoolean(jobj *self, const jvalue *a) {
  pref *p = pref_find(prefs_of(self), jni_string_utf8(a[0].l));
  return RET_BOOL(p ? atoi(p->value) != 0 : a[1].z);
}

static jvalue prefs_getFloat(jobj *self, const jvalue *a) {
  pref *p = pref_find(prefs_of(self), jni_string_utf8(a[0].l));
  jvalue r;
  r.j = 0;
  r.f = p ? strtof(p->value, NULL) : a[1].f;
  return r;
}

static jvalue prefs_getString(jobj *self, const jvalue *a) {
  pref *p = pref_find(prefs_of(self), jni_string_utf8(a[0].l));
  if (p)
    return RET_STR(p->value);
  return RET_OBJ(a[1].l);
}

static jvalue editor_putInt(jobj *self, const jvalue *a) {
  char buf[32];
  snprintf(buf, sizeof(buf), "%d", a[1].i);
  pref_set(prefs_of(self), jni_string_utf8(a[0].l), 'i', buf);
  return RET_OBJ(self);
}

static jvalue editor_putLong(jobj *self, const jvalue *a) {
  char buf[32];
  snprintf(buf, sizeof(buf), "%lld", a[1].j);
  pref_set(prefs_of(self), jni_string_utf8(a[0].l), 'l', buf);
  return RET_OBJ(self);
}

static jvalue editor_putBoolean(jobj *self, const jvalue *a) {
  pref_set(prefs_of(self), jni_string_utf8(a[0].l), 'b', a[1].z ? "1" : "0");
  return RET_OBJ(self);
}

static jvalue editor_putFloat(jobj *self, const jvalue *a) {
  char buf[48];
  snprintf(buf, sizeof(buf), "%.9g", a[1].f);
  pref_set(prefs_of(self), jni_string_utf8(a[0].l), 'f', buf);
  return RET_OBJ(self);
}

static jvalue editor_putString(jobj *self, const jvalue *a) {
  pref_set(prefs_of(self), jni_string_utf8(a[0].l), 's', jni_string_utf8(a[1].l));
  return RET_OBJ(self);
}

static jvalue editor_remove(jobj *self, const jvalue *a) {
  pref_remove(prefs_of(self), jni_string_utf8(a[0].l));
  return RET_OBJ(self);
}

static jvalue editor_clear(jobj *self, const jvalue *a) {
  prefs_file *pf = prefs_of(self);
  while (pf->head)
    pref_remove(pf, pf->head->key);
  return RET_OBJ(self);
}

static jvalue editor_commit(jobj *self, const jvalue *a) {
  prefs_save(prefs_of(self));
  return RET_BOOL(1);
}

// --- Unity player -----------------------------------------------------------

static jvalue unity_loadLibrary(jobj *self, const jvalue *a) {
  L_INFO("unity: loadLibrary(%s)", jni_string_utf8(a[0].l));
  return RET_BOOL(1);
}

static jvalue unity_getInternetReachability(jobj *self, const jvalue *a) {
  return RET_INT(2);  // ReachableViaLocalAreaNetwork (Wi-Fi)
}

static jvalue fmod_start(jobj *self, const jvalue *a) {
  audio_fmod_start();
  return RET_VOID;
}

static jvalue fmod_stop(jobj *self, const jvalue *a) {
  audio_fmod_stop();
  return RET_VOID;
}

static jvalue audio_getProperty(jobj *self, const jvalue *a) {
  const char *k = jni_string_utf8(a[0].l);
  if (strstr(k, "SAMPLE_RATE"))
    return RET_STR("48000");
  if (strstr(k, "FRAMES_PER_BUFFER"))
    return RET_STR("1024");
  return RET_STR("");
}

static jvalue audio_getSampleRate(jobj *self, const jvalue *a) { return RET_INT(48000); }
static jvalue audio_getFrameCount(jobj *self, const jvalue *a) { return RET_INT(1024); }

// --- reflection / class loading ---------------------------------------------
// Unity's AndroidJavaObject resolves members through ReflectionHelper and
// then JNI FromReflectedMethod/Field; plugins are located with
// ClassLoader.findLibrary before Mono's DllImport opens them.

static jvalue reflection_getMethodID(jobj *self, const jvalue *a) {
  jobj *cls = a[0].l;
  jobj *m = jni_new_object("java/lang/reflect/Method");
  m->native = jni_method_id(cls ? cls->cls : "java/lang/Object", jni_string_utf8(a[1].l), jni_string_utf8(a[2].l));
  return RET_OBJ(m);
}

static jvalue reflection_getConstructorID(jobj *self, const jvalue *a) {
  jobj *cls = a[0].l;
  jobj *m = jni_new_object("java/lang/reflect/Constructor");
  m->native = jni_method_id(cls ? cls->cls : "java/lang/Object", "<init>", jni_string_utf8(a[1].l));
  return RET_OBJ(m);
}

static jvalue reflection_getFieldID(jobj *self, const jvalue *a) {
  jobj *cls = a[0].l;
  jobj *f = jni_new_object("java/lang/reflect/Field");
  f->native = jni_field_id(cls ? cls->cls : "java/lang/Object", jni_string_utf8(a[1].l), jni_string_utf8(a[2].l));
  return RET_OBJ(f);
}

static jvalue class_forName(jobj *self, const jvalue *a) {
  char name[256];
  strncpy(name, jni_string_utf8(a[0].l), sizeof(name) - 1);
  name[sizeof(name) - 1] = 0;
  for (char *p = name; *p; p++)
    if (*p == '.')
      *p = '/';
  return RET_OBJ(jni_find_class(name));
}

static jvalue class_getName(jobj *self, const jvalue *a) {
  char name[256];
  strncpy(name, self ? self->cls : "java.lang.Object", sizeof(name) - 1);
  name[sizeof(name) - 1] = 0;
  for (char *p = name; *p; p++)
    if (*p == '/')
      *p = '.';
  return RET_STR(name);
}

static jvalue object_getClass(jobj *self, const jvalue *a) {
  return RET_OBJ(jni_find_class(self ? self->cls : "java/lang/Object"));
}

static jvalue classloader_findLibrary(jobj *self, const jvalue *a) {
  char path[256];
  snprintf(path, sizeof(path), ANDROID_APK_DIR "/lib/lib%s.so", jni_string_utf8(a[0].l));
  return RET_STR(path);
}

static jvalue string_equals(jobj *self, const jvalue *a) {
  return RET_BOOL(strcmp(jni_string_utf8(self), jni_string_utf8(a[0].l)) == 0);
}

static jvalue get_static_activity(jobj *self, const jvalue *a) {
  return RET_OBJ(jni_new_object("com/unity3d/player/UnityPlayerNativeActivity"));
}

void java_impl_init(void) {
  // Context / Activity (registered for any class: the game asks on several)
  jni_register_method(NULL, "getPackageName", NULL, ctx_getPackageName);
  jni_register_method(NULL, "getPackageCodePath", NULL, ctx_getPackageCodePath);
  jni_register_method(NULL, "getPackageResourcePath", NULL, ctx_getPackageCodePath);
  jni_register_method(NULL, "getFilesDir", NULL, ctx_getFilesDir);
  jni_register_method(NULL, "getCacheDir", NULL, ctx_getCacheDir);
  jni_register_method(NULL, "getExternalFilesDir", NULL, ctx_getExternalFilesDir);
  jni_register_method(NULL, "getExternalCacheDir", NULL, ctx_getCacheDir);
  jni_register_method(NULL, "getExternalStorageDirectory", NULL, env_getExternalStorageDirectory);
  jni_register_method(NULL, "getExternalStorageState", NULL, env_getExternalStorageState);
  jni_register_method(NULL, "getApplicationInfo", NULL, ctx_getApplicationInfo);
  jni_register_method(NULL, "getSharedPreferences", NULL, ctx_getSharedPreferences);
  jni_register_method("java/io/File", "getPath", NULL, file_getPath);
  jni_register_method("java/io/File", "getAbsolutePath", NULL, file_getPath);
  jni_register_method("java/io/File", "getCanonicalPath", NULL, file_getPath);
  jni_register_method("java/io/File", "toString", NULL, file_getPath);
  jni_register_method(NULL, "getInstallerPackageName", NULL, ret_null);
  jni_register_method(NULL, "getRequestedOrientation", NULL, ret_zero);

  jni_register_field_string("android/content/pm/ApplicationInfo", "sourceDir", fs_apk_android_path());
  jni_register_field_string("android/content/pm/ApplicationInfo", "dataDir", ANDROID_DATA_DIR);
  jni_register_field_string("android/content/pm/ApplicationInfo", "nativeLibraryDir", ANDROID_APK_DIR "/lib");
  jni_register_field_int("android/content/pm/ApplicationInfo", "flags", 0);

  // Display
  jni_register_method(NULL, "getWidth", "()I", disp_getWidth);
  jni_register_method(NULL, "getHeight", "()I", disp_getHeight);
  jni_register_method(NULL, "getRawWidth", NULL, disp_getWidth);
  jni_register_method(NULL, "getRawHeight", NULL, disp_getHeight);
  jni_register_method(NULL, "getRotation", NULL, ret_zero);
  jni_register_method(NULL, "getMetrics", NULL, ret_void);
  jni_register_method(NULL, "getRealMetrics", NULL, ret_void);
  jni_register_field_int("android/util/DisplayMetrics", "widthPixels", SCREEN_W);
  jni_register_field_int("android/util/DisplayMetrics", "heightPixels", SCREEN_H);
  jni_register_field_int("android/util/DisplayMetrics", "densityDpi", 240);
  jni_register_field_float("android/util/DisplayMetrics", "density", 1.5f);
  jni_register_field_float("android/util/DisplayMetrics", "scaledDensity", 1.5f);
  jni_register_field_float("android/util/DisplayMetrics", "xdpi", 220.0f);
  jni_register_field_float("android/util/DisplayMetrics", "ydpi", 220.0f);

  // Build info
  jni_register_field_string("android/os/Build", "MODEL", "PS Vita");
  jni_register_field_string("android/os/Build", "MANUFACTURER", "Sony");
  jni_register_field_string("android/os/Build", "BRAND", "Sony");
  jni_register_field_string("android/os/Build", "DEVICE", "vita");
  jni_register_field_string("android/os/Build", "PRODUCT", "vita");
  jni_register_field_string("android/os/Build", "HARDWARE", "vita");
  jni_register_field_string("android/os/Build", "CPU_ABI", "armeabi-v7a");
  jni_register_field_string("android/os/Build", "CPU_ABI2", "armeabi");
  jni_register_field_string("android/os/Build", "FINGERPRINT", "sony/vita/vita:4.4.2/KOT49H/1:user/release-keys");
  jni_register_field_string("android/os/Build$VERSION", "RELEASE", "4.4.2");
  jni_register_field_string("android/os/Build$VERSION", "INCREMENTAL", "1");
  jni_register_field_int("android/os/Build$VERSION", "SDK_INT", 19);

  // Locale / ids
  jni_register_method("java/util/Locale", "getLanguage", NULL, locale_getLanguage);
  jni_register_method("java/util/Locale", "getCountry", NULL, locale_getCountry);
  jni_register_method("java/util/Locale", "toString", NULL, locale_toString);
  jni_register_method("android/provider/Settings$Secure", "getString", NULL, secure_getString);
  jni_register_method("android/telephony/TelephonyManager", "getDeviceId", NULL, tel_getDeviceId);

  // Power / misc
  jni_register_method(NULL, "isHeld", NULL, ret_false);
  jni_register_method(NULL, "acquire", NULL, ret_void);
  jni_register_method(NULL, "release", NULL, ret_void);
  jni_register_method(NULL, "vibrate", NULL, ret_void);
  jni_register_method(NULL, "getScreenTimeout", NULL, ret_zero);

  // SharedPreferences
  jni_register_method(NULL, "edit", NULL, prefs_edit);
  jni_register_method(NULL, "contains", "(Ljava/lang/String;)Z", prefs_contains);
  jni_register_method(NULL, "getInt", "(Ljava/lang/String;I)I", prefs_getInt);
  jni_register_method(NULL, "getLong", "(Ljava/lang/String;J)J", prefs_getLong);
  jni_register_method(NULL, "getBoolean", "(Ljava/lang/String;Z)Z", prefs_getBoolean);
  jni_register_method(NULL, "getFloat", "(Ljava/lang/String;F)F", prefs_getFloat);
  jni_register_method(NULL, "getString", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;", prefs_getString);
  jni_register_method(NULL, "putInt", NULL, editor_putInt);
  jni_register_method(NULL, "putLong", NULL, editor_putLong);
  jni_register_method(NULL, "putBoolean", NULL, editor_putBoolean);
  jni_register_method(NULL, "putFloat", NULL, editor_putFloat);
  jni_register_method(NULL, "putString", NULL, editor_putString);
  jni_register_method(NULL, "remove", NULL, editor_remove);
  jni_register_method(NULL, "clear", NULL, editor_clear);
  jni_register_method(NULL, "commit", NULL, editor_commit);
  jni_register_method(NULL, "apply", NULL, editor_commit);

  // Unity player
  jni_register_method(NULL, "loadLibrary", NULL, unity_loadLibrary);
  jni_register_method(NULL, "getInternetReachability", NULL, unity_getInternetReachability);
  jni_register_method(NULL, "getSplashMode", NULL, ret_zero);
  jni_register_method(NULL, "isDebugBuild", NULL, ret_false);
  jni_register_method(NULL, "isEmbedded", NULL, ret_false);
  jni_register_method(NULL, "executeGLThreadJobs", NULL, ret_void);
  jni_register_method(NULL, "showSoftInput", NULL, ret_void);
  jni_register_method(NULL, "hideSoftInput", NULL, ret_void);
  jni_register_method(NULL, "setScreenSize", NULL, ret_void);
  jni_register_method(NULL, "showVideoPlayer", NULL, ret_false);
  jni_register_method(NULL, "startActivityIndicator", NULL, ret_void);
  jni_register_method(NULL, "stopActivityIndicator", NULL, ret_void);
  jni_register_method(NULL, "getCurrentActivity", NULL, get_static_activity);

  // Reflection / class loading
  jni_register_method(NULL, "getMethodID", NULL, reflection_getMethodID);
  jni_register_method(NULL, "getConstructorID", NULL, reflection_getConstructorID);
  jni_register_method(NULL, "getFieldID", NULL, reflection_getFieldID);
  jni_register_method("java/lang/Class", "forName", NULL, class_forName);
  jni_register_method("java/lang/Class", "getName", NULL, class_getName);
  jni_register_method(NULL, "getClass", "()Ljava/lang/Class;", object_getClass);
  jni_register_method(NULL, "findLibrary", NULL, classloader_findLibrary);
  jni_register_method("java/lang/System", "load", NULL, ret_void);
  jni_register_method("java/lang/String", "equals", NULL, string_equals);

  // Audio
  jni_register_method("org/fmod/FMODAudioDevice", "start", NULL, fmod_start);
  jni_register_method("org/fmod/FMODAudioDevice", "stop", NULL, fmod_stop);
  jni_register_method(NULL, "getProperty", "(Ljava/lang/String;)Ljava/lang/String;", audio_getProperty);
  jni_register_method(NULL, "getPrimaryOutputSamplingRate", NULL, audio_getSampleRate);
  jni_register_method(NULL, "getPrimaryOutputFrameCount", NULL, audio_getFrameCount);
}

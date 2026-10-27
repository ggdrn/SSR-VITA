// Asks the game's own C# side what it is doing, through libmono's embedding
// API: the loaded scene, whether a level is loading, time scale, how many
// GameObjects exist and what every camera renders. Runs on the main (Unity)
// thread between frames, so Unity's API is used from the thread it expects.

#include "loader/so_util.h"
#include "util/log.h"

#include <stdint.h>
#include <string.h>

typedef void MonoImage, MonoClass, MonoMethod, MonoObject, MonoDomain, MonoType;

static struct {
  MonoImage *(*image_loaded)(const char *);
  MonoClass *(*class_from_name)(MonoImage *, const char *, const char *);
  MonoMethod *(*get_method)(MonoClass *, const char *, int);
  MonoObject *(*invoke)(MonoMethod *, void *, void **, MonoObject **);
  char *(*string_to_utf8)(MonoObject *);
  void (*mono_free)(void *);
  MonoDomain *(*domain_get)(void);
  MonoType *(*class_get_type)(MonoClass *);
  MonoObject *(*type_get_object)(MonoDomain *, MonoType *);
  MonoClass *(*object_get_class)(MonoObject *);
  const char *(*class_get_name)(MonoClass *);
} api;

static MonoImage *unity_image;
static int probe_disabled;

#define SYM(field, name)                               \
  do {                                                 \
    *(void **)&api.field = (void *)so_symbol(m, name); \
    if (!api.field) {                                  \
      L_WARN("probe: %s not exported", name);          \
      return 0;                                        \
    }                                                  \
  } while (0)

static int probe_init(void) {
  so_module *m = so_find_module("libmono.so");
  if (!m)
    return 0;
  SYM(image_loaded, "mono_image_loaded");
  SYM(class_from_name, "mono_class_from_name");
  SYM(get_method, "mono_class_get_method_from_name");
  SYM(invoke, "mono_runtime_invoke");
  SYM(string_to_utf8, "mono_string_to_utf8");
  SYM(mono_free, "mono_free");
  SYM(domain_get, "mono_domain_get");
  SYM(class_get_type, "mono_class_get_type");
  SYM(type_get_object, "mono_type_get_object");
  SYM(object_get_class, "mono_object_get_class");
  SYM(class_get_name, "mono_class_get_name");
  unity_image = api.image_loaded("UnityEngine");
  if (!unity_image)
    unity_image = api.image_loaded("/vita/srr224.apk/assets/bin/Data/Managed/UnityEngine.dll");
  return unity_image != NULL;
}

// Invokes a static or instance method; returns NULL (and logs) on exceptions.
static MonoObject *call(const char *cls, const char *method, void *self, void **args, int nargs) {
  MonoClass *k = api.class_from_name(unity_image, "UnityEngine", cls);
  MonoMethod *mm = k ? api.get_method(k, method, nargs) : NULL;
  if (!mm) {
    L_WARN("probe: UnityEngine.%s.%s not found", cls, method);
    return NULL;
  }
  MonoObject *exc = NULL;
  MonoObject *r = api.invoke(mm, self, args, &exc);
  if (exc) {
    L_WARN("probe: UnityEngine.%s.%s threw %s", cls, method, api.class_get_name(api.object_get_class(exc)));
    return NULL;
  }
  return r;
}

// Boxed value types keep their value right after the MonoObject header.
static int unbox_int(MonoObject *o) { return o ? *(int32_t *)((char *)o + 8) : -1; }
static float unbox_float(MonoObject *o) { return o ? *(float *)((char *)o + 8) : -1.0f; }
static int array_length(MonoObject *a) { return a ? *(int32_t *)((char *)a + 12) : -1; }
static MonoObject *array_at(MonoObject *a, int i) { return ((MonoObject **)((char *)a + 16))[i]; }

static void string_of(MonoObject *s, char *out, size_t size) {
  if (!s) {
    strncpy(out, "(null)", size);
    return;
  }
  char *u = api.string_to_utf8(s);
  strncpy(out, u ? u : "", size - 1);
  out[size - 1] = 0;
  api.mono_free(u);
}

void mono_probe(void) {
  if (probe_disabled)
    return;
  if (!unity_image && !probe_init()) {
    L_WARN("probe: UnityEngine image not available, probe disabled");
    probe_disabled = 1;
    return;
  }

  char level[64];
  string_of(call("Application", "get_loadedLevelName", NULL, NULL, 0), level, sizeof(level));
  int loading = unbox_int(call("Application", "get_isLoadingLevel", NULL, NULL, 0)) & 0xff;
  float scale = unbox_float(call("Time", "get_timeScale", NULL, NULL, 0));

  MonoClass *go = api.class_from_name(unity_image, "UnityEngine", "GameObject");
  void *type_arg[1] = {api.type_get_object(api.domain_get(), api.class_get_type(go))};
  int objects = array_length(call("Object", "FindObjectsOfType", NULL, type_arg, 1));

  MonoObject *cams = call("Camera", "get_allCameras", NULL, NULL, 0);
  int ncams = array_length(cams);
  L_INFO("probe: level '%s' loading %d timeScale %.2f gameObjects %d cameras %d", level, loading, scale, objects,
         ncams);
  for (int i = 0; i < ncams && i < 8; i++) {
    MonoObject *cam = array_at(cams, i);
    char name[64];
    string_of(call("Object", "get_name", cam, NULL, 0), name, sizeof(name));
    int mask = unbox_int(call("Camera", "get_cullingMask", cam, NULL, 0));
    int clear = unbox_int(call("Camera", "get_clearFlags", cam, NULL, 0));
    float depth = unbox_float(call("Camera", "get_depth", cam, NULL, 0));
    int target = call("Camera", "get_targetTexture", cam, NULL, 0) != NULL;
    L_INFO("probe:   camera '%s' cullingMask 0x%X clearFlags %d depth %.1f renderTexture %d", name, mask, clear,
           depth, target);
  }
}

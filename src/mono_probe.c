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
  MonoImage *(*get_corlib)(void);
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
  SYM(get_corlib, "mono_get_corlib");
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

static MonoObject *call_in(MonoImage *img, const char *ns, const char *cls, const char *method, void *self,
                           void **args, int nargs) {
  MonoClass *k = api.class_from_name(img, ns, cls);
  MonoMethod *mm = k ? api.get_method(k, method, nargs) : NULL;
  if (!mm) {
    L_WARN("probe: %s.%s.%s not found", ns, cls, method);
    return NULL;
  }
  MonoObject *exc = NULL;
  MonoObject *r = api.invoke(mm, self, args, &exc);
  return exc ? NULL : r;
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

static MonoObject *find_all(const char *cls) {
  MonoClass *k = api.class_from_name(unity_image, "UnityEngine", cls);
  void *type_arg[1] = {api.type_get_object(api.domain_get(), api.class_get_type(k))};
  return call("Object", "FindObjectsOfType", NULL, type_arg, 1);
}

// Boxed structs (Vector3, Rect, Bounds) are float fields after the header.
static float boxed_f(MonoObject *o, int i) { return o ? ((float *)((char *)o + 8))[i] : -9999.0f; }

static int as_bool(MonoObject *o) { return o ? *(uint8_t *)((char *)o + 8) : -1; }

// One-off listing of the scene: every GameObject (active flags, layer) and
// every Renderer (enabled, visible, layer). Shows whether the title UI exists
// but is inactive, or was never created.
static void dump_scene(void) {
  MonoObject *gos = find_all("GameObject");
  int n = array_length(gos);
  L_INFO("probe: scene objects (%d):", n);
  for (int i = 0; i < n && i < 160; i++) {
    MonoObject *g = array_at(gos, i);
    char name[64];
    string_of(call("Object", "get_name", g, NULL, 0), name, sizeof(name));
    MonoObject *t = call("GameObject", "get_transform", g, NULL, 0);
    MonoObject *lp = t ? call("Transform", "get_localPosition", t, NULL, 0) : NULL;
    MonoObject *wp = t ? call("Transform", "get_position", t, NULL, 0) : NULL;
    L_INFO("probe:   go '%s' self %d hierarchy %d layer %d local (%.3g, %.3g, %.3g) world (%.3g, %.3g, %.3g)", name,
           as_bool(call("GameObject", "get_activeSelf", g, NULL, 0)),
           as_bool(call("GameObject", "get_activeInHierarchy", g, NULL, 0)),
           unbox_int(call("GameObject", "get_layer", g, NULL, 0)), boxed_f(lp, 0), boxed_f(lp, 1), boxed_f(lp, 2),
           boxed_f(wp, 0), boxed_f(wp, 1), boxed_f(wp, 2));
  }
  MonoObject *rs = find_all("Renderer");
  n = array_length(rs);
  L_INFO("probe: renderers (%d):", n);
  for (int i = 0; i < n && i < 80; i++) {
    MonoObject *r = array_at(rs, i);
    MonoObject *g = call("Component", "get_gameObject", r, NULL, 0);
    char name[64];
    string_of(g ? call("Object", "get_name", g, NULL, 0) : NULL, name, sizeof(name));
    L_INFO("probe:   renderer %s on '%s' enabled %d visible %d layer %d",
           api.class_get_name(api.object_get_class(r)), name, as_bool(call("Renderer", "get_enabled", r, NULL, 0)),
           as_bool(call("Renderer", "get_isVisible", r, NULL, 0)), g ? unbox_int(call("GameObject", "get_layer", g, NULL, 0)) : -1);
    MonoObject *b = call("Renderer", "get_bounds", r, NULL, 0);  // center xyz, extents xyz
    MonoObject *t = g ? call("GameObject", "get_transform", g, NULL, 0) : NULL;
    MonoObject *pos = t ? call("Transform", "get_position", t, NULL, 0) : NULL;
    MonoObject *scl = t ? call("Transform", "get_lossyScale", t, NULL, 0) : NULL;
    L_INFO("probe:     bounds center (%.3f, %.3f, %.3f) extents (%.3f, %.3f, %.3f) pos (%.3f, %.3f, %.3f) scale "
           "(%.5f, %.5f, %.5f)",
           boxed_f(b, 0), boxed_f(b, 1), boxed_f(b, 2), boxed_f(b, 3), boxed_f(b, 4), boxed_f(b, 5), boxed_f(pos, 0),
           boxed_f(pos, 1), boxed_f(pos, 2), boxed_f(scl, 0), boxed_f(scl, 1), boxed_f(scl, 2));
  }
}

static double unbox_double(MonoObject *o) { return o ? *(double *)((char *)o + 8) : -9999.0; }

// Float math through Mono's icalls (-> the loader's soft-float libm shims) and
// Unity's own float paths, against known answers.
static void math_selftest(MonoObject *cam) {
  MonoImage *corlib = api.get_corlib();
  double d1 = 2.0, d2 = 10.0, d3 = 1.0;
  void *a1[1] = {&d1}, *a2[2] = {&d1, &d2}, *a3[2] = {&d3, &d3};
  L_INFO("probe: Math.Sqrt(2)=%.6f Math.Sin(2)=%.6f Math.Cos(2)=%.6f Math.Pow(2,10)=%.3f Math.Atan2(1,1)=%.6f "
         "Math.Log(2)=%.6f Math.Exp(2)=%.6f",
         unbox_double(call_in(corlib, "System", "Math", "Sqrt", NULL, a1, 1)),
         unbox_double(call_in(corlib, "System", "Math", "Sin", NULL, a1, 1)),
         unbox_double(call_in(corlib, "System", "Math", "Cos", NULL, a1, 1)),
         unbox_double(call_in(corlib, "System", "Math", "Pow", NULL, a2, 2)),
         unbox_double(call_in(corlib, "System", "Math", "Atan2", NULL, a3, 2)),
         unbox_double(call_in(corlib, "System", "Math", "Log", NULL, a1, 1)),
         unbox_double(call_in(corlib, "System", "Math", "Exp", NULL, a1, 1)));
  float f1 = 1.7f, f2 = 2.0f;
  void *b1[1] = {&f1}, *b2[1] = {&f2};
  L_INFO("probe: Mathf.Floor(1.7)=%.3f Mathf.Round(1.7)=%.3f Mathf.Sqrt(2)=%.6f Mathf.Sin(2)=%.6f RoundToInt(1.7)=%d",
         unbox_float(call("Mathf", "Floor", NULL, b1, 1)), unbox_float(call("Mathf", "Round", NULL, b1, 1)),
         unbox_float(call("Mathf", "Sqrt", NULL, b2, 1)), unbox_float(call("Mathf", "Sin", NULL, b2, 1)),
         unbox_int(call("Mathf", "RoundToInt", NULL, b1, 1)));
  if (cam) {
    float vp[3] = {0.5f, 0.5f, 0.0f}, sp[3] = {480.0f, 272.0f, 0.0f};
    void *c1[1] = {vp}, *c2[1] = {sp};
    MonoObject *w1 = call("Camera", "ViewportToWorldPoint", cam, c1, 1);
    MonoObject *w2 = call("Camera", "ScreenToWorldPoint", cam, c2, 1);
    MonoObject *rect = call("Camera", "get_rect", cam, NULL, 0);
    L_INFO("probe: cam.ViewportToWorldPoint(.5,.5,0)=(%.3f, %.3f, %.3f) ScreenToWorldPoint(480,272,0)=(%.3f, %.3f, "
           "%.3f) rect (%.3f, %.3f, %.3f, %.3f)",
           boxed_f(w1, 0), boxed_f(w1, 1), boxed_f(w1, 2), boxed_f(w2, 0), boxed_f(w2, 1), boxed_f(w2, 2),
           boxed_f(rect, 0), boxed_f(rect, 1), boxed_f(rect, 2), boxed_f(rect, 3));
  }
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
  L_INFO("probe: level '%s' loading %d timeScale %.2f gameObjects %d cameras %d screen %dx%d", level, loading,
         scale, objects, ncams, unbox_int(call("Screen", "get_width", NULL, NULL, 0)),
         unbox_int(call("Screen", "get_height", NULL, NULL, 0)));
  // Tweens and NGUI's RealTime move the title UI from these clocks.
  L_INFO("probe: Time.time %.3f deltaTime %.4f realtimeSinceStartup %.3f frameCount %d",
         unbox_float(call("Time", "get_time", NULL, NULL, 0)), unbox_float(call("Time", "get_deltaTime", NULL, NULL, 0)),
         unbox_float(call("Time", "get_realtimeSinceStartup", NULL, NULL, 0)),
         unbox_int(call("Time", "get_frameCount", NULL, NULL, 0)));
  static int dumps;
  if (dumps < 2 && objects > 0) {  // once early, once after things settle
    static int calls;
    if (calls++ == 0 || calls == 5) {
      dump_scene();
      dumps++;
    }
  }
  static int tested;
  if (!tested++)
    math_selftest(ncams > 0 ? array_at(cams, 0) : NULL);
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
    MonoObject *t = call("Component", "get_transform", cam, NULL, 0);
    MonoObject *pos = t ? call("Transform", "get_position", t, NULL, 0) : NULL;
    MonoObject *px = call("Camera", "get_pixelRect", cam, NULL, 0);
    L_INFO("probe:     enabled %d ortho %d size %.3f near %.3f far %.3f aspect %.3f fov %.1f pixelRect (%.0f,%.0f %.0fx%.0f) "
           "pos (%.3f, %.3f, %.3f)",
           as_bool(call("Behaviour", "get_enabled", cam, NULL, 0)), as_bool(call("Camera", "get_orthographic", cam, NULL, 0)),
           unbox_float(call("Camera", "get_orthographicSize", cam, NULL, 0)),
           unbox_float(call("Camera", "get_nearClipPlane", cam, NULL, 0)),
           unbox_float(call("Camera", "get_farClipPlane", cam, NULL, 0)),
           unbox_float(call("Camera", "get_aspect", cam, NULL, 0)), unbox_float(call("Camera", "get_fieldOfView", cam, NULL, 0)),
           boxed_f(px, 0), boxed_f(px, 1), boxed_f(px, 2), boxed_f(px, 3), boxed_f(pos, 0), boxed_f(pos, 1),
           boxed_f(pos, 2));
  }
}

// A permissive fake JVM: enough of JNIEnv/JavaVM for Unity, Mono glue and CRI.
// Every class and method "exists"; methods without an emulated implementation
// return a neutral value (0, "", or an empty object of the declared type) and
// are logged once, so the log doubles as a to-do list of Java APIs to emulate.

#include "jni.h"
#include "../bridges/bridges.h"
#include "../util/log.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/kernel/threadmgr.h>

// --- registry ---------------------------------------------------------------

typedef struct reg_method {
  const char *cls, *name, *sig;
  jni_method_fn fn;
} reg_method;

typedef struct jmethod {
  const char *cls, *name, *sig;
  jni_method_fn fn;
  char ret;           // return type char
  const char *ret_cls;
  int warned;
} jmethod;

typedef enum { FV_NONE, FV_INT, FV_FLOAT, FV_STRING } fv_kind;

typedef struct reg_field {
  const char *cls, *name;
  fv_kind kind;
  int32_t i;
  float f;
  const char *s;
} reg_field;

typedef struct jfield {
  const char *cls, *name, *sig;
  reg_field *reg;
  jvalue value;       // storage for fields set by the game
  int has_value;
} jfield;

typedef struct native_entry {
  const char *cls, *name, *sig;
  void *fn;
} native_entry;

#define MAX_REG 512
#define MAX_IDS 2048
#define MAX_CLASSES 256
#define MAX_NATIVES 256

static reg_method reg_methods[MAX_REG];
static int num_reg_methods;
static reg_field reg_fields[MAX_REG];
static int num_reg_fields;
static jmethod *method_ids[MAX_IDS];
static int num_method_ids;
static jfield *field_ids[MAX_IDS];
static int num_field_ids;
static jobj *classes[MAX_CLASSES];
static int num_classes;
static native_entry natives[MAX_NATIVES];
static int num_natives;
static SceKernelLwMutexWork jni_lock;

static void lock(void) { sceKernelLockLwMutex(&jni_lock, 1, NULL); }
static void unlock(void) { sceKernelUnlockLwMutex(&jni_lock, 1); }

void jni_register_method(const char *cls, const char *name, const char *sig, jni_method_fn fn) {
  if (num_reg_methods < MAX_REG)
    reg_methods[num_reg_methods++] = (reg_method){cls, name, sig, fn};
}

static reg_field *add_field(const char *cls, const char *name) {
  if (num_reg_fields >= MAX_REG)
    return NULL;
  reg_field *f = &reg_fields[num_reg_fields++];
  memset(f, 0, sizeof(*f));
  f->cls = cls;
  f->name = name;
  return f;
}

void jni_register_field_int(const char *cls, const char *name, int32_t v) {
  reg_field *f = add_field(cls, name);
  if (f) f->kind = FV_INT, f->i = v;
}
void jni_register_field_float(const char *cls, const char *name, float v) {
  reg_field *f = add_field(cls, name);
  if (f) f->kind = FV_FLOAT, f->f = v;
}
void jni_register_field_string(const char *cls, const char *name, const char *v) {
  reg_field *f = add_field(cls, name);
  if (f) f->kind = FV_STRING, f->s = v;
}

// --- objects ----------------------------------------------------------------

static jobj *obj_alloc(jobj_kind kind, const char *cls) {
  jobj *o = calloc(1, sizeof(jobj));
  o->magic = JOBJ_MAGIC;
  o->kind = kind;
  o->cls = cls;
  return o;
}

static int is_obj(const void *p) { return p && ((const jobj *)p)->magic == JOBJ_MAGIC; }

jobj *jni_find_class(const char *name) {
  lock();
  for (int i = 0; i < num_classes; i++) {
    if (strcmp(classes[i]->cls, name) == 0) {
      unlock();
      return classes[i];
    }
  }
  jobj *c = obj_alloc(JO_CLASS, strdup(name));
  if (num_classes < MAX_CLASSES)
    classes[num_classes++] = c;
  unlock();
  return c;
}

jobj *jni_new_object(const char *cls) { return obj_alloc(JO_OBJECT, jni_find_class(cls)->cls); }

jobj *jni_new_string(const char *utf8) {
  jobj *o = obj_alloc(JO_STRING, "java/lang/String");
  o->str.utf8 = strdup(utf8 ? utf8 : "");
  return o;
}

const char *jni_string_utf8(jobj *s) { return is_obj(s) && s->kind == JO_STRING ? s->str.utf8 : ""; }

static jobj *new_array(const char *cls, int len, int elem_size) {
  jobj *o = obj_alloc(JO_ARRAY, cls);
  o->arr.len = len;
  o->arr.elem_size = elem_size;
  o->arr.data = calloc(len ? len : 1, elem_size);
  return o;
}

void *jni_new_direct_buffer(void *addr, int64_t capacity) {
  jobj *o = obj_alloc(JO_BYTEBUFFER, "java/nio/ByteBuffer");
  o->buf.addr = addr;
  o->buf.cap = capacity;
  return o;
}

// --- signatures -------------------------------------------------------------

static const char *skip_type(const char *p) {
  while (*p == '[')
    p++;
  if (*p == 'L') {
    while (*p && *p != ';')
      p++;
  }
  return *p ? p + 1 : p;
}

// "(...)Lfoo/Bar;" -> 'L' and "foo/Bar"
static char return_type(const char *sig, const char **cls) {
  const char *r = strchr(sig, ')');
  *cls = NULL;
  if (!r)
    return 'V';
  r++;
  if (*r == 'L') {
    size_t n = strcspn(r + 1, ";");
    char *c = malloc(n + 1);
    memcpy(c, r + 1, n);
    c[n] = 0;
    *cls = c;
  }
  return *r;
}

static int read_args_v(const char *sig, va_list ap, jvalue *out, int max) {
  const char *p = strchr(sig, '(');
  int n = 0;
  if (!p)
    return 0;
  p++;
  while (*p && *p != ')' && n < max) {
    switch (*p) {
    case 'Z': case 'B': case 'C': case 'S': case 'I':
      out[n].j = 0;
      out[n].i = va_arg(ap, int);
      break;
    case 'J': out[n].j = va_arg(ap, int64_t); break;
    case 'F': out[n].j = 0; out[n].f = (float)va_arg(ap, double); break;
    case 'D': out[n].d = va_arg(ap, double); break;
    default: out[n].j = 0; out[n].l = va_arg(ap, void *); break;
    }
    n++;
    p = skip_type(p);
  }
  return n;
}

// --- method dispatch --------------------------------------------------------

static int cls_match(const char *want, const char *have) { return !want || (have && strcmp(want, have) == 0); }

static jmethod *get_method(const char *cls, const char *name, const char *sig) {
  lock();
  for (int i = 0; i < num_method_ids; i++) {
    jmethod *m = method_ids[i];
    if (strcmp(m->name, name) == 0 && strcmp(m->sig, sig) == 0 && cls_match(m->cls, cls)) {
      unlock();
      return m;
    }
  }
  jmethod *m = calloc(1, sizeof(jmethod));
  m->cls = cls;
  m->name = strdup(name);
  m->sig = strdup(sig);
  m->ret = return_type(sig, &m->ret_cls);
  // Most specific registration wins: class + signature, then class, then any.
  int best = -1;
  for (int i = 0; i < num_reg_methods; i++) {
    reg_method *r = &reg_methods[i];
    if (strcmp(r->name, name) != 0 || !cls_match(r->cls, cls) || (r->sig && strcmp(r->sig, sig) != 0))
      continue;
    int score = (r->cls ? 2 : 0) + (r->sig ? 1 : 0);
    if (best < 0 || score > best) {
      best = score;
      m->fn = r->fn;
    }
  }
  if (num_method_ids < MAX_IDS)
    method_ids[num_method_ids++] = m;
  unlock();
  return m;
}

static jvalue invoke(jobj *self, jmethod *m, const jvalue *args) {
  if (m->fn)
    return m->fn(self, args);
  jvalue r;
  r.j = 0;
  if (!m->warned++)
    L_WARN("jni: unimplemented %s.%s%s", m->cls ? m->cls : "?", m->name, m->sig);
  if (m->ret == 'L') {
    if (m->ret_cls && strcmp(m->ret_cls, "java/lang/String") == 0)
      r.l = jni_new_string("");
    else
      r.l = jni_new_object(m->ret_cls ? m->ret_cls : "java/lang/Object");
  } else if (m->ret == '[') {
    r.l = new_array("[", 0, 4);
  }
  return r;
}

// --- JNIEnv functions -------------------------------------------------------

typedef void *JNIEnv_;
#define ENV void *env

static int32_t GetVersion(ENV) { return 0x00010006; }
static jobj *FindClass(ENV, const char *name) { return jni_find_class(name); }
static jobj *GetSuperclass(ENV, jobj *c) { return jni_find_class("java/lang/Object"); }
static uint8_t IsAssignableFrom(ENV, jobj *a, jobj *b) { return 1; }
static int32_t Throw(ENV, jobj *t) { return 0; }
static int32_t ThrowNew(ENV, jobj *c, const char *msg) {
  L_WARN("jni: ThrowNew(%s, %s)", c ? c->cls : "?", msg ? msg : "");
  return 0;
}
static jobj *ExceptionOccurred(ENV) { return NULL; }
static void ExceptionDescribe(ENV) {}
static void ExceptionClear(ENV) {}
static void FatalError(ENV, const char *msg) { L_ERROR("jni: FatalError: %s", msg); }
static int32_t PushLocalFrame(ENV, int32_t cap) { return 0; }
static jobj *PopLocalFrame(ENV, jobj *res) { return res; }
static jobj *NewGlobalRef(ENV, jobj *o) { return o; }
static void DeleteGlobalRef(ENV, jobj *o) {}
static void DeleteLocalRef(ENV, jobj *o) {}
static uint8_t IsSameObject(ENV, jobj *a, jobj *b) { return a == b; }
static jobj *NewLocalRef(ENV, jobj *o) { return o; }
static int32_t EnsureLocalCapacity(ENV, int32_t cap) { return 0; }
static jobj *AllocObject(ENV, jobj *c) { return jni_new_object(c ? c->cls : "java/lang/Object"); }

static jobj *construct(jobj *c, jmethod *m, const jvalue *args) {
  jobj *o = jni_new_object(c ? c->cls : "java/lang/Object");
  if (!m) {
    L_WARN("jni: NewObject(%s) with a null constructor ID", c ? c->cls : "?");
    return o;
  }
  if (m->fn)
    m->fn(o, args);
  return o;
}
static jobj *NewObjectV(ENV, jobj *c, jmethod *m, va_list ap) {
  jvalue a[32];
  if (m)
    read_args_v(m->sig, ap, a, 32);
  return construct(c, m, a);
}
static jobj *NewObject(ENV, jobj *c, jmethod *m, ...) {
  va_list ap;
  va_start(ap, m);
  jobj *o = NewObjectV(env, c, m, ap);
  va_end(ap);
  return o;
}
static jobj *NewObjectA(ENV, jobj *c, jmethod *m, const jvalue *a) { return construct(c, m, a); }

static jobj *GetObjectClass(ENV, jobj *o) { return jni_find_class(is_obj(o) ? o->cls : "java/lang/Object"); }
static uint8_t IsInstanceOf(ENV, jobj *o, jobj *c) { return 1; }

static jmethod *GetMethodID(ENV, jobj *c, const char *name, const char *sig) {
  return get_method(c ? c->cls : NULL, name, sig);
}

#define CALL_TYPED(Name, T, field, ATTR)                                                      \
  static ATTR T Call##Name##MethodV(ENV, jobj *o, jmethod *m, va_list ap) {                   \
    jvalue a[32];                                                                             \
    read_args_v(m->sig, ap, a, 32);                                                           \
    return invoke(o, m, a).field;                                                             \
  }                                                                                           \
  static ATTR T Call##Name##Method(ENV, jobj *o, jmethod *m, ...) {                           \
    va_list ap;                                                                               \
    va_start(ap, m);                                                                          \
    T r = Call##Name##MethodV(env, o, m, ap);                                                 \
    va_end(ap);                                                                               \
    return r;                                                                                 \
  }                                                                                           \
  static ATTR T Call##Name##MethodA(ENV, jobj *o, jmethod *m, const jvalue *a) {              \
    return invoke(o, m, a).field;                                                             \
  }                                                                                           \
  static ATTR T CallNonvirtual##Name##MethodV(ENV, jobj *o, jobj *c, jmethod *m, va_list ap) { \
    return Call##Name##MethodV(env, o, m, ap);                                                \
  }                                                                                           \
  static ATTR T CallNonvirtual##Name##Method(ENV, jobj *o, jobj *c, jmethod *m, ...) {        \
    va_list ap;                                                                               \
    va_start(ap, m);                                                                          \
    T r = Call##Name##MethodV(env, o, m, ap);                                                 \
    va_end(ap);                                                                               \
    return r;                                                                                 \
  }                                                                                           \
  static ATTR T CallNonvirtual##Name##MethodA(ENV, jobj *o, jobj *c, jmethod *m, const jvalue *a) { \
    return invoke(o, m, a).field;                                                             \
  }                                                                                           \
  static ATTR T CallStatic##Name##MethodV(ENV, jobj *c, jmethod *m, va_list ap) {             \
    return Call##Name##MethodV(env, c, m, ap);                                                \
  }                                                                                           \
  static ATTR T CallStatic##Name##Method(ENV, jobj *c, jmethod *m, ...) {                     \
    va_list ap;                                                                               \
    va_start(ap, m);                                                                          \
    T r = Call##Name##MethodV(env, c, m, ap);                                                 \
    va_end(ap);                                                                               \
    return r;                                                                                 \
  }                                                                                           \
  static ATTR T CallStatic##Name##MethodA(ENV, jobj *c, jmethod *m, const jvalue *a) {        \
    return invoke(c, m, a).field;                                                             \
  }

#define NOATTR
CALL_TYPED(Object, void *, l, NOATTR)
CALL_TYPED(Boolean, uint8_t, z, NOATTR)
CALL_TYPED(Byte, int8_t, b, NOATTR)
CALL_TYPED(Char, uint16_t, c, NOATTR)
CALL_TYPED(Short, int16_t, s, NOATTR)
CALL_TYPED(Int, int32_t, i, NOATTR)
CALL_TYPED(Long, int64_t, j, NOATTR)
CALL_TYPED(Float, float, f, SOFTFP)
CALL_TYPED(Double, double, d, SOFTFP)

static void CallVoidMethodV(ENV, jobj *o, jmethod *m, va_list ap) {
  jvalue a[32];
  read_args_v(m->sig, ap, a, 32);
  invoke(o, m, a);
}
static void CallVoidMethod(ENV, jobj *o, jmethod *m, ...) {
  va_list ap;
  va_start(ap, m);
  CallVoidMethodV(env, o, m, ap);
  va_end(ap);
}
static void CallVoidMethodA(ENV, jobj *o, jmethod *m, const jvalue *a) { invoke(o, m, a); }
static void CallNonvirtualVoidMethodV(ENV, jobj *o, jobj *c, jmethod *m, va_list ap) { CallVoidMethodV(env, o, m, ap); }
static void CallNonvirtualVoidMethod(ENV, jobj *o, jobj *c, jmethod *m, ...) {
  va_list ap;
  va_start(ap, m);
  CallVoidMethodV(env, o, m, ap);
  va_end(ap);
}
static void CallNonvirtualVoidMethodA(ENV, jobj *o, jobj *c, jmethod *m, const jvalue *a) { invoke(o, m, a); }
static void CallStaticVoidMethodV(ENV, jobj *c, jmethod *m, va_list ap) { CallVoidMethodV(env, c, m, ap); }
static void CallStaticVoidMethod(ENV, jobj *c, jmethod *m, ...) {
  va_list ap;
  va_start(ap, m);
  CallVoidMethodV(env, c, m, ap);
  va_end(ap);
}
static void CallStaticVoidMethodA(ENV, jobj *c, jmethod *m, const jvalue *a) { invoke(c, m, a); }

// --- fields -----------------------------------------------------------------

static jfield *get_field(const char *cls, const char *name, const char *sig) {
  lock();
  for (int i = 0; i < num_field_ids; i++) {
    jfield *f = field_ids[i];
    if (strcmp(f->name, name) == 0 && cls_match(f->cls, cls)) {
      unlock();
      return f;
    }
  }
  jfield *f = calloc(1, sizeof(jfield));
  f->cls = cls;
  f->name = strdup(name);
  f->sig = strdup(sig);
  for (int i = 0; i < num_reg_fields; i++) {
    if (strcmp(reg_fields[i].name, name) == 0 && cls_match(reg_fields[i].cls, cls))
      f->reg = &reg_fields[i];
  }
  if (!f->reg)
    L_DEBUG("jni: unknown field %s.%s %s", cls ? cls : "?", name, sig);
  if (num_field_ids < MAX_IDS)
    field_ids[num_field_ids++] = f;
  unlock();
  return f;
}

static jfield *GetFieldID(ENV, jobj *c, const char *name, const char *sig) { return get_field(c ? c->cls : NULL, name, sig); }

static jvalue field_value(jfield *f) {
  jvalue v;
  v.j = 0;
  if (f->has_value)
    return f->value;
  if (f->reg) {
    switch (f->reg->kind) {
    case FV_INT: v.i = f->reg->i; break;
    case FV_FLOAT: v.f = f->reg->f; break;
    case FV_STRING: v.l = jni_new_string(f->reg->s); break;
    default: break;
    }
    return v;
  }
  if (f->sig[0] == 'L')
    v.l = strncmp(f->sig, "Ljava/lang/String;", 18) == 0 ? jni_new_string("") : jni_new_object("java/lang/Object");
  return v;
}

static void field_set(jfield *f, jvalue v) {
  f->value = v;
  f->has_value = 1;
}

#define FIELD_TYPED(Name, T, field, ATTR)                                                         \
  static ATTR T Get##Name##Field(ENV, jobj *o, jfield *f) { return field_value(f).field; }        \
  static ATTR void Set##Name##Field(ENV, jobj *o, jfield *f, T v) {                               \
    jvalue j;                                                                                     \
    j.j = 0;                                                                                      \
    j.field = v;                                                                                  \
    field_set(f, j);                                                                              \
  }                                                                                               \
  static ATTR T GetStatic##Name##Field(ENV, jobj *c, jfield *f) { return field_value(f).field; }  \
  static ATTR void SetStatic##Name##Field(ENV, jobj *c, jfield *f, T v) {                         \
    jvalue j;                                                                                     \
    j.j = 0;                                                                                      \
    j.field = v;                                                                                  \
    field_set(f, j);                                                                              \
  }

FIELD_TYPED(Object, void *, l, NOATTR)
FIELD_TYPED(Boolean, uint8_t, z, NOATTR)
FIELD_TYPED(Byte, int8_t, b, NOATTR)
FIELD_TYPED(Char, uint16_t, c, NOATTR)
FIELD_TYPED(Short, int16_t, s, NOATTR)
FIELD_TYPED(Int, int32_t, i, NOATTR)
FIELD_TYPED(Long, int64_t, j, NOATTR)
FIELD_TYPED(Float, float, f, SOFTFP)
FIELD_TYPED(Double, double, d, SOFTFP)

static jmethod *GetStaticMethodID(ENV, jobj *c, const char *name, const char *sig) {
  return get_method(c ? c->cls : NULL, name, sig);
}
static jfield *GetStaticFieldID(ENV, jobj *c, const char *name, const char *sig) {
  return get_field(c ? c->cls : NULL, name, sig);
}

// --- strings ----------------------------------------------------------------

static int utf8_decode(const char *s, uint16_t *out) {
  int n = 0;
  const uint8_t *p = (const uint8_t *)s;
  while (*p) {
    uint32_t c = *p++;
    if (c >= 0xC0) {
      int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
      c &= 0x3F >> extra;
      while (extra-- && (*p & 0xC0) == 0x80)
        c = (c << 6) | (*p++ & 0x3F);
    }
    if (c >= 0x10000) {
      if (out) {
        c -= 0x10000;
        out[n] = 0xD800 | (c >> 10);
        out[n + 1] = 0xDC00 | (c & 0x3FF);
      }
      n += 2;
    } else {
      if (out)
        out[n] = c;
      n++;
    }
  }
  return n;
}

static jobj *NewString(ENV, const uint16_t *chars, int32_t len) {
  char *buf = malloc(len * 3 + 1), *p = buf;
  for (int i = 0; i < len; i++) {
    uint32_t c = chars[i];
    if (c < 0x80) *p++ = c;
    else if (c < 0x800) { *p++ = 0xC0 | (c >> 6); *p++ = 0x80 | (c & 0x3F); }
    else { *p++ = 0xE0 | (c >> 12); *p++ = 0x80 | ((c >> 6) & 0x3F); *p++ = 0x80 | (c & 0x3F); }
  }
  *p = 0;
  jobj *o = jni_new_string(buf);
  free(buf);
  return o;
}
static int32_t GetStringLength(ENV, jobj *s) { return utf8_decode(jni_string_utf8(s), NULL); }
static const uint16_t *GetStringChars(ENV, jobj *s, uint8_t *copy) {
  const char *u = jni_string_utf8(s);
  uint16_t *out = calloc(utf8_decode(u, NULL) + 1, 2);
  utf8_decode(u, out);
  if (copy) *copy = 1;
  return out;
}
static void ReleaseStringChars(ENV, jobj *s, const uint16_t *chars) { free((void *)chars); }
static jobj *NewStringUTF(ENV, const char *utf8) { return utf8 ? jni_new_string(utf8) : NULL; }
static int32_t GetStringUTFLength(ENV, jobj *s) { return strlen(jni_string_utf8(s)); }
static const char *GetStringUTFChars(ENV, jobj *s, uint8_t *copy) {
  if (copy) *copy = 0;
  return jni_string_utf8(s);
}
static void ReleaseStringUTFChars(ENV, jobj *s, const char *chars) {}
static void GetStringRegion(ENV, jobj *s, int32_t start, int32_t len, uint16_t *buf) {
  const uint16_t *all = GetStringChars(env, s, NULL);
  memcpy(buf, all + start, len * 2);
  free((void *)all);
}
static void GetStringUTFRegion(ENV, jobj *s, int32_t start, int32_t len, char *buf) {
  memcpy(buf, jni_string_utf8(s) + start, len);
  buf[len] = 0;
}
static const uint16_t *GetStringCritical(ENV, jobj *s, uint8_t *copy) { return GetStringChars(env, s, copy); }
static void ReleaseStringCritical(ENV, jobj *s, const uint16_t *c) { free((void *)c); }

// --- arrays -----------------------------------------------------------------

static int32_t GetArrayLength(ENV, jobj *a) { return is_obj(a) && a->kind == JO_ARRAY ? a->arr.len : 0; }
static jobj *NewObjectArray(ENV, int32_t len, jobj *c, jobj *init) {
  jobj *a = new_array("[L", len, sizeof(void *));
  for (int i = 0; i < len; i++)
    ((void **)a->arr.data)[i] = init;
  return a;
}
static jobj *GetObjectArrayElement(ENV, jobj *a, int32_t i) {
  return is_obj(a) && i < a->arr.len ? ((jobj **)a->arr.data)[i] : NULL;
}
static void SetObjectArrayElement(ENV, jobj *a, int32_t i, jobj *v) {
  if (is_obj(a) && i < a->arr.len)
    ((jobj **)a->arr.data)[i] = v;
}

#define ARRAY_TYPED(Name, sz)                                                                    \
  static jobj *New##Name##Array(ENV, int32_t len) { return new_array("[" #Name, len, sz); }      \
  static void *Get##Name##ArrayElements(ENV, jobj *a, uint8_t *copy) {                           \
    if (copy) *copy = 0;                                                                         \
    return is_obj(a) ? a->arr.data : NULL;                                                       \
  }                                                                                              \
  static void Release##Name##ArrayElements(ENV, jobj *a, void *e, int32_t mode) {}               \
  static void Get##Name##ArrayRegion(ENV, jobj *a, int32_t start, int32_t len, void *buf) {      \
    memcpy(buf, (char *)a->arr.data + start * sz, len * sz);                                     \
  }                                                                                              \
  static void Set##Name##ArrayRegion(ENV, jobj *a, int32_t start, int32_t len, const void *buf) { \
    memcpy((char *)a->arr.data + start * sz, buf, len * sz);                                     \
  }

ARRAY_TYPED(Boolean, 1)
ARRAY_TYPED(Byte, 1)
ARRAY_TYPED(Char, 2)
ARRAY_TYPED(Short, 2)
ARRAY_TYPED(Int, 4)
ARRAY_TYPED(Long, 8)
ARRAY_TYPED(Float, 4)
ARRAY_TYPED(Double, 8)

static void *GetPrimitiveArrayCritical(ENV, jobj *a, uint8_t *copy) {
  if (copy) *copy = 0;
  return is_obj(a) ? a->arr.data : NULL;
}
static void ReleasePrimitiveArrayCritical(ENV, jobj *a, void *p, int32_t mode) {}

// --- natives ----------------------------------------------------------------

typedef struct {
  const char *name;
  const char *sig;
  void *fn;
} JNINativeMethod;

static int32_t RegisterNatives(ENV, jobj *c, const JNINativeMethod *m, int32_t n) {
  for (int i = 0; i < n && num_natives < MAX_NATIVES; i++) {
    natives[num_natives++] = (native_entry){c ? c->cls : "?", strdup(m[i].name), strdup(m[i].sig), m[i].fn};
    L_INFO("jni: RegisterNatives %s.%s%s -> %p", c ? c->cls : "?", m[i].name, m[i].sig, m[i].fn);
  }
  return 0;
}
static int32_t UnregisterNatives(ENV, jobj *c) { return 0; }

void *jni_find_native(const char *cls, const char *name) {
  for (int i = num_natives - 1; i >= 0; i--) {
    if ((!cls || strcmp(natives[i].cls, cls) == 0) && strcmp(natives[i].name, name) == 0)
      return natives[i].fn;
  }
  return NULL;
}

const char *jni_native_signature(const char *cls, const char *name) {
  for (int i = num_natives - 1; i >= 0; i--) {
    if ((!cls || strcmp(natives[i].cls, cls) == 0) && strcmp(natives[i].name, name) == 0)
      return natives[i].sig;
  }
  return NULL;
}

// --- misc -------------------------------------------------------------------

static int32_t MonitorEnter(ENV, jobj *o) { return 0; }
static int32_t MonitorExit(ENV, jobj *o) { return 0; }
static int32_t GetJavaVM(ENV, void **vm) {
  *vm = jni_get_vm();
  return 0;
}
static jobj *NewWeakGlobalRef(ENV, jobj *o) { return o; }
static void DeleteWeakGlobalRef(ENV, jobj *o) {}
static uint8_t ExceptionCheck(ENV) { return 0; }
static jobj *NewDirectByteBuffer(ENV, void *addr, int64_t cap) { return jni_new_direct_buffer(addr, cap); }
static void *GetDirectBufferAddress(ENV, jobj *b) { return is_obj(b) && b->kind == JO_BYTEBUFFER ? b->buf.addr : NULL; }
static int64_t GetDirectBufferCapacity(ENV, jobj *b) { return is_obj(b) && b->kind == JO_BYTEBUFFER ? b->buf.cap : -1; }
static int32_t GetObjectRefType(ENV, jobj *o) { return 2; }  // JNIGlobalRefType
static jobj *DefineClass(ENV, const char *name, jobj *loader, const void *buf, int32_t len) { return NULL; }
// java.lang.reflect.Method/Field objects carry their jmethod/jfield in ->native.
static void *FromReflected(ENV, jobj *o) { return is_obj(o) ? o->native : NULL; }
static jobj *ToReflected(ENV, jobj *c, void *id, uint8_t is_static) {
  jobj *o = jni_new_object("java/lang/reflect/Method");
  o->native = id;
  return o;
}

void *jni_method_id(const char *cls, const char *name, const char *sig) {
  return get_method(jni_find_class(cls)->cls, name, sig);
}

void *jni_field_id(const char *cls, const char *name, const char *sig) {
  return get_field(jni_find_class(cls)->cls, name, sig);
}

static void unimplemented_jni(void) { L_ERROR("jni: unimplemented JNIEnv slot called from %p", __builtin_return_address(0)); }

// --- tables -----------------------------------------------------------------

#define JNI_TABLE_SIZE 233
static void *env_table[JNI_TABLE_SIZE];
static void **env_ptr = env_table;  // JNIEnv* points at a pointer to the table

static int32_t vm_DestroyJavaVM(void *vm) { return 0; }
static int32_t vm_AttachCurrentThread(void *vm, void **penv, void *args) {
  *penv = &env_ptr;
  return 0;
}
static int32_t vm_DetachCurrentThread(void *vm) { return 0; }
static int32_t vm_GetEnv(void *vm, void **penv, int32_t version) {
  *penv = &env_ptr;
  return 0;
}

static void *vm_table[8] = {NULL, NULL, NULL, vm_DestroyJavaVM, vm_AttachCurrentThread, vm_DetachCurrentThread,
                            vm_GetEnv, vm_AttachCurrentThread};
static void **vm_ptr = vm_table;

void *jni_get_env(void) { return &env_ptr; }
void *jni_get_vm(void) { return &vm_ptr; }

#define SET(i, f) env_table[i] = (void *)(f)
#define SET_CALLS(base, Kind)                     \
  SET(base + 0, Kind##ObjectMethod);              \
  SET(base + 1, Kind##ObjectMethodV);             \
  SET(base + 2, Kind##ObjectMethodA);             \
  SET(base + 3, Kind##BooleanMethod);             \
  SET(base + 4, Kind##BooleanMethodV);            \
  SET(base + 5, Kind##BooleanMethodA);            \
  SET(base + 6, Kind##ByteMethod);                \
  SET(base + 7, Kind##ByteMethodV);               \
  SET(base + 8, Kind##ByteMethodA);               \
  SET(base + 9, Kind##CharMethod);                \
  SET(base + 10, Kind##CharMethodV);              \
  SET(base + 11, Kind##CharMethodA);              \
  SET(base + 12, Kind##ShortMethod);              \
  SET(base + 13, Kind##ShortMethodV);             \
  SET(base + 14, Kind##ShortMethodA);             \
  SET(base + 15, Kind##IntMethod);                \
  SET(base + 16, Kind##IntMethodV);               \
  SET(base + 17, Kind##IntMethodA);               \
  SET(base + 18, Kind##LongMethod);               \
  SET(base + 19, Kind##LongMethodV);              \
  SET(base + 20, Kind##LongMethodA);              \
  SET(base + 21, Kind##FloatMethod);              \
  SET(base + 22, Kind##FloatMethodV);             \
  SET(base + 23, Kind##FloatMethodA);             \
  SET(base + 24, Kind##DoubleMethod);             \
  SET(base + 25, Kind##DoubleMethodV);            \
  SET(base + 26, Kind##DoubleMethodA);            \
  SET(base + 27, Kind##VoidMethod);               \
  SET(base + 28, Kind##VoidMethodV);              \
  SET(base + 29, Kind##VoidMethodA)

#define SET_FIELDS(base, Op)            \
  SET(base + 0, Op##ObjectField);       \
  SET(base + 1, Op##BooleanField);      \
  SET(base + 2, Op##ByteField);         \
  SET(base + 3, Op##CharField);         \
  SET(base + 4, Op##ShortField);        \
  SET(base + 5, Op##IntField);          \
  SET(base + 6, Op##LongField);         \
  SET(base + 7, Op##FloatField);        \
  SET(base + 8, Op##DoubleField)

#define SET_ARRAYS(base, Op, Suffix)      \
  SET(base + 0, Op##Boolean##Suffix);     \
  SET(base + 1, Op##Byte##Suffix);        \
  SET(base + 2, Op##Char##Suffix);        \
  SET(base + 3, Op##Short##Suffix);       \
  SET(base + 4, Op##Int##Suffix);         \
  SET(base + 5, Op##Long##Suffix);        \
  SET(base + 6, Op##Float##Suffix);       \
  SET(base + 7, Op##Double##Suffix)

void jni_init(void) {
  sceKernelCreateLwMutex(&jni_lock, "jni", SCE_KERNEL_MUTEX_ATTR_RECURSIVE, 0, NULL);
  for (int i = 0; i < JNI_TABLE_SIZE; i++)
    env_table[i] = (void *)unimplemented_jni;

  SET(4, GetVersion);
  SET(5, DefineClass);
  SET(6, FindClass);
  SET(7, FromReflected);
  SET(8, FromReflected);
  SET(9, ToReflected);
  SET(10, GetSuperclass);
  SET(11, IsAssignableFrom);
  SET(12, ToReflected);
  SET(13, Throw);
  SET(14, ThrowNew);
  SET(15, ExceptionOccurred);
  SET(16, ExceptionDescribe);
  SET(17, ExceptionClear);
  SET(18, FatalError);
  SET(19, PushLocalFrame);
  SET(20, PopLocalFrame);
  SET(21, NewGlobalRef);
  SET(22, DeleteGlobalRef);
  SET(23, DeleteLocalRef);
  SET(24, IsSameObject);
  SET(25, NewLocalRef);
  SET(26, EnsureLocalCapacity);
  SET(27, AllocObject);
  SET(28, NewObject);
  SET(29, NewObjectV);
  SET(30, NewObjectA);
  SET(31, GetObjectClass);
  SET(32, IsInstanceOf);
  SET(33, GetMethodID);
  SET_CALLS(34, Call);
  SET_CALLS(64, CallNonvirtual);
  SET(94, GetFieldID);
  SET_FIELDS(95, Get);
  SET_FIELDS(104, Set);
  SET(113, GetStaticMethodID);
  SET_CALLS(114, CallStatic);
  SET(144, GetStaticFieldID);
  SET_FIELDS(145, GetStatic);
  SET_FIELDS(154, SetStatic);
  SET(163, NewString);
  SET(164, GetStringLength);
  SET(165, GetStringChars);
  SET(166, ReleaseStringChars);
  SET(167, NewStringUTF);
  SET(168, GetStringUTFLength);
  SET(169, GetStringUTFChars);
  SET(170, ReleaseStringUTFChars);
  SET(171, GetArrayLength);
  SET(172, NewObjectArray);
  SET(173, GetObjectArrayElement);
  SET(174, SetObjectArrayElement);
  SET_ARRAYS(175, New, Array);
  SET_ARRAYS(183, Get, ArrayElements);
  SET_ARRAYS(191, Release, ArrayElements);
  SET_ARRAYS(199, Get, ArrayRegion);
  SET_ARRAYS(207, Set, ArrayRegion);
  SET(215, RegisterNatives);
  SET(216, UnregisterNatives);
  SET(217, MonitorEnter);
  SET(218, MonitorExit);
  SET(219, GetJavaVM);
  SET(220, GetStringRegion);
  SET(221, GetStringUTFRegion);
  SET(222, GetPrimitiveArrayCritical);
  SET(223, ReleasePrimitiveArrayCritical);
  SET(224, GetStringCritical);
  SET(225, ReleaseStringCritical);
  SET(226, NewWeakGlobalRef);
  SET(227, DeleteWeakGlobalRef);
  SET(228, ExceptionCheck);
  SET(229, NewDirectByteBuffer);
  SET(230, GetDirectBufferAddress);
  SET(231, GetDirectBufferCapacity);
  SET(232, GetObjectRefType);

  java_impl_init();
}

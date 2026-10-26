#ifndef SRR_JNI_H
#define SRR_JNI_H

#include <stdint.h>
#include <stddef.h>

typedef union {
  uint8_t z;
  int8_t b;
  uint16_t c;
  int16_t s;
  int32_t i;
  int64_t j;
  float f;
  double d;
  void *l;
} jvalue;

typedef enum { JO_CLASS = 1, JO_OBJECT, JO_STRING, JO_ARRAY, JO_BYTEBUFFER } jobj_kind;

typedef struct jobj {
  uint32_t magic;
  jobj_kind kind;
  const char *cls;          // class name, e.g. "java/io/File"
  union {
    struct {
      char *utf8;
    } str;
    struct {
      int len;
      int elem_size;
      void *data;
    } arr;
    struct {
      void *addr;
      int64_t cap;
    } buf;
  };
  void *native;             // per-object payload for emulated classes
} jobj;

#define JOBJ_MAGIC 0x4A4F424A

typedef jvalue (*jni_method_fn)(jobj *self, const jvalue *args);

// Registers an emulated Java method. `cls` may be NULL to match any class.
void jni_register_method(const char *cls, const char *name, const char *sig, jni_method_fn fn);
void jni_register_field_int(const char *cls, const char *name, int32_t v);
void jni_register_field_float(const char *cls, const char *name, float v);
void jni_register_field_string(const char *cls, const char *name, const char *v);

void jni_init(void);
void *jni_get_env(void);
void *jni_get_vm(void);

jobj *jni_new_object(const char *cls);
jobj *jni_new_string(const char *utf8);
const char *jni_string_utf8(jobj *s);
jobj *jni_find_class(const char *name);
void *jni_new_direct_buffer(void *addr, int64_t capacity);

// Natives registered by the game through RegisterNatives.
void *jni_find_native(const char *cls, const char *name);
const char *jni_native_signature(const char *cls, const char *name);

// java_impl.c
void java_impl_init(void);

#endif

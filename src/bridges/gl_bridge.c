// EGL on top of vitaGL plus GLES2 entry points that need soft-float shims or
// different answers. Unity is steered to its GLES2 renderer by refusing ES3
// contexts and only exporting GLES2 symbols.

#include "bridges.h"
#include "../config.h"
#include "../util/log.h"

#include <stdlib.h>
#include <string.h>
#include <vitaGL.h>

uintptr_t dynlib_lookup(const char *name);

#define EGL_SUCCESS 0x3000
#define EGL_BAD_MATCH 0x3009
#define EGL_BUFFER_SIZE 0x3020
#define EGL_ALPHA_SIZE 0x3021
#define EGL_BLUE_SIZE 0x3022
#define EGL_GREEN_SIZE 0x3023
#define EGL_RED_SIZE 0x3024
#define EGL_DEPTH_SIZE 0x3025
#define EGL_STENCIL_SIZE 0x3026
#define EGL_CONFIG_CAVEAT 0x3027
#define EGL_CONFIG_ID 0x3028
#define EGL_NATIVE_VISUAL_ID 0x302E
#define EGL_SAMPLES 0x3031
#define EGL_SAMPLE_BUFFERS 0x3032
#define EGL_SURFACE_TYPE 0x3033
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_CONFORMANT 0x3042
#define EGL_NONE 0x3038
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_WIDTH 0x3057
#define EGL_HEIGHT 0x3056

#define FAKE_DISPLAY ((void *)0xE61D0001)
#define FAKE_CONFIG ((void *)0xE61C0001)
#define FAKE_SURFACE ((void *)0xE6150001)
#define FAKE_CONTEXT ((void *)0xE61C7001)

static int egl_error = EGL_SUCCESS;

void *eglGetDisplay_bridge(void *native) { return FAKE_DISPLAY; }

int eglInitialize_bridge(void *dpy, int *major, int *minor) {
  if (major) *major = 1;
  if (minor) *minor = 4;
  return 1;
}

int eglTerminate_bridge(void *dpy) { return 1; }

int eglChooseConfig_bridge(void *dpy, const int *attribs, void **configs, int size, int *num) {
  if (configs && size > 0)
    configs[0] = FAKE_CONFIG;
  if (num)
    *num = 1;
  return 1;
}

int eglGetConfigAttrib_bridge(void *dpy, void *cfg, int attr, int *value) {
  switch (attr) {
  case EGL_RED_SIZE:
  case EGL_GREEN_SIZE:
  case EGL_BLUE_SIZE:
  case EGL_ALPHA_SIZE: *value = 8; break;
  case EGL_BUFFER_SIZE: *value = 32; break;
  case EGL_DEPTH_SIZE: *value = 24; break;
  case EGL_STENCIL_SIZE: *value = 8; break;
  case EGL_CONFIG_ID: *value = 1; break;
  case EGL_SURFACE_TYPE: *value = 0x4; break;      // EGL_WINDOW_BIT
  case EGL_RENDERABLE_TYPE:
  case EGL_CONFORMANT: *value = 0x4; break;        // EGL_OPENGL_ES2_BIT
  case EGL_CONFIG_CAVEAT: *value = EGL_NONE; break;
  default: *value = 0; break;
  }
  return 1;
}

void *eglCreateWindowSurface_bridge(void *dpy, void *cfg, void *win, const int *attribs) { return FAKE_SURFACE; }
int eglDestroySurface_bridge(void *dpy, void *surf) { return 1; }

void *eglCreateContext_bridge(void *dpy, void *cfg, void *share, const int *attribs) {
  for (const int *a = attribs; a && *a != EGL_NONE; a += 2) {
    if (a[0] == EGL_CONTEXT_CLIENT_VERSION && a[1] > 2) {
      L_INFO("egl: refusing GLES%d context, Unity will fall back to GLES2", a[1]);
      egl_error = EGL_BAD_MATCH;
      return NULL;
    }
  }
  return FAKE_CONTEXT;
}

int eglDestroyContext_bridge(void *dpy, void *ctx) { return 1; }
int eglMakeCurrent_bridge(void *dpy, void *draw, void *read, void *ctx) { return 1; }

int eglSwapBuffers_bridge(void *dpy, void *surf) {
  vglSwapBuffers(GL_FALSE);
  return 1;
}

int eglQuerySurface_bridge(void *dpy, void *surf, int attr, int *value) {
  if (attr == EGL_WIDTH) *value = SCREEN_W;
  else if (attr == EGL_HEIGHT) *value = SCREEN_H;
  else *value = 0;
  return 1;
}

int eglSwapInterval_bridge(void *dpy, int interval) { return 1; }

int eglGetError_bridge(void) {
  int e = egl_error;
  egl_error = EGL_SUCCESS;
  return e;
}

void *eglGetProcAddress_bridge(const char *name) {
  void *p = (void *)dynlib_lookup(name);
  if (!p)
    L_DEBUG("eglGetProcAddress(%s) -> NULL", name);
  return p;
}

// --- GL ---------------------------------------------------------------------

SOFTFP void glClearColor_sfp(float r, float g, float b, float a) { glClearColor(r, g, b, a); }
SOFTFP void glClearDepthf_sfp(float d) { glClearDepthf(d); }
SOFTFP void glPolygonOffset_sfp(float f, float u) { glPolygonOffset(f, u); }
SOFTFP void glTexParameterf_sfp(GLenum t, GLenum p, float v) { glTexParameterf(t, p, v); }

const GLubyte *glGetString_bridge(GLenum name) {
  switch (name) {
  case GL_VERSION: return (const GLubyte *)"OpenGL ES 2.0 vitaGL";
  case GL_SHADING_LANGUAGE_VERSION: return (const GLubyte *)"OpenGL ES GLSL ES 1.00";
  case GL_VENDOR: return (const GLubyte *)"Sony";
  case GL_RENDERER: return (const GLubyte *)"SGX543MP4+ (vitaGL)";
  default: return glGetString(name);
  }
}

// vitaGL's glGet*iv leave the output untouched for names it doesn't know, so
// the caller reads stack garbage (Unity once tried a 3.6 GB "Shader"
// allocation sized by GL_ACTIVE_UNIFORM_BLOCKS). Outputs are zeroed first and
// GLES3-only queries are answered here.

#define GL_ACTIVE_UNIFORM_BLOCK_MAX_NAME_LENGTH 0x8A35
#define GL_ACTIVE_UNIFORM_BLOCKS 0x8A36
#define GL_PROGRAM_BINARY_LENGTH_ 0x8741
#define GL_TRANSFORM_FEEDBACK_VARYINGS 0x8C83
#define GL_TRANSFORM_FEEDBACK_BUFFER_MODE 0x8C7F
#define GL_TRANSFORM_FEEDBACK_VARYING_MAX_LENGTH 0x8C76

void glGetIntegerv_bridge(GLenum pname, GLint *data) {
  switch (pname) {
  case 0x87FE:  // GL_NUM_PROGRAM_BINARY_FORMATS
  case 0x8DF9:  // GL_NUM_SHADER_BINARY_FORMATS
    *data = 0;
    break;
  default:
    *data = 0;
    glGetIntegerv(pname, data);
    break;
  }
}

void glGetProgramiv_bridge(GLuint prog, GLenum pname, GLint *params) {
  *params = 0;
  switch (pname) {
  case GL_ACTIVE_UNIFORM_BLOCKS:
  case GL_ACTIVE_UNIFORM_BLOCK_MAX_NAME_LENGTH:
  case GL_PROGRAM_BINARY_LENGTH_:  // no program binary cache: Unity recompiles
  case GL_TRANSFORM_FEEDBACK_VARYINGS:
  case GL_TRANSFORM_FEEDBACK_BUFFER_MODE:
  case GL_TRANSFORM_FEEDBACK_VARYING_MAX_LENGTH:
    return;
  default:
    glGetProgramiv(prog, pname, params);
  }
}

void glGetShaderiv_bridge(GLuint shader, GLenum pname, GLint *params) {
  *params = 0;
  glGetShaderiv(shader, pname, params);
}

void glGetVertexAttribiv_bridge(GLuint index, GLenum pname, GLint *params) {
  *params = 0;
  glGetVertexAttribiv(index, pname, params);
}

void glGetFramebufferAttachmentParameteriv_bridge(GLenum target, GLenum attachment, GLenum pname, GLint *params) {
  *params = 0;
  glGetFramebufferAttachmentParameteriv(target, attachment, pname, params);
}

// Not provided by vitaGL. Unity only uses sub-uploads of compressed textures
// for streaming updates, which this game doesn't rely on.
void glCompressedTexSubImage2D_bridge(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h,
                                      GLenum format, GLsizei size, const void *data) {
  static int warned;
  if (!warned++)
    L_WARN("gl: glCompressedTexSubImage2D ignored (format 0x%X)", format);
}

void glGetTexParameteriv_bridge(GLenum target, GLenum pname, GLint *params) {
  switch (pname) {
  case GL_TEXTURE_MIN_FILTER: *params = GL_LINEAR; break;
  case GL_TEXTURE_MAG_FILTER: *params = GL_LINEAR; break;
  case GL_TEXTURE_WRAP_S:
  case GL_TEXTURE_WRAP_T: *params = GL_REPEAT; break;
  default: *params = 0; break;
  }
}

// GLES lets attribute locations be bound before shaders are attached; they
// take effect at link time. vitaGL dereferences the attached vertex shader
// immediately, so bindings are recorded here and replayed in glLinkProgram.

typedef struct attrib_binding {
  GLuint prog;
  GLuint index;
  char *name;
  struct attrib_binding *next;
} attrib_binding;

static attrib_binding *bindings;

void glBindAttribLocation_bridge(GLuint prog, GLuint index, const GLchar *name) {
  for (attrib_binding *b = bindings; b; b = b->next) {
    if (b->prog == prog && strcmp(b->name, name) == 0) {
      b->index = index;
      return;
    }
  }
  attrib_binding *b = malloc(sizeof(*b));
  b->prog = prog;
  b->index = index;
  b->name = strdup(name);
  b->next = bindings;
  bindings = b;
}

// --- uniform locations ------------------------------------------------------
// vitaGL returns -(pointer to its uniform record) as the location: a huge
// positive int. Unity sizes per-shader tables by the largest location
// ((max + 1) * 16 bytes -> a 3.6 GB allocation), so locations are remapped to
// small per-program indices and translated back in glUniform*.

#define MAX_PROGRAMS 1025  // vitaGL's MAX_CUSTOM_PROGRAMS + 1 (names start at 1)

typedef struct {
  GLint *real;
  int count, cap;
} location_table;

static location_table locations[MAX_PROGRAMS];
static GLuint current_program;

static void locations_clear(GLuint prog) {
  if (prog < MAX_PROGRAMS) {
    free(locations[prog].real);
    memset(&locations[prog], 0, sizeof(location_table));
  }
}

GLint glGetUniformLocation_bridge(GLuint prog, const GLchar *name) {
  GLint real = glGetUniformLocation(prog, name);
  if (real == -1 || prog >= MAX_PROGRAMS)
    return -1;
  location_table *t = &locations[prog];
  for (int i = 0; i < t->count; i++) {
    if (t->real[i] == real)
      return i;
  }
  if (t->count == t->cap) {
    t->cap = t->cap ? t->cap * 2 : 16;
    t->real = realloc(t->real, t->cap * sizeof(GLint));
  }
  t->real[t->count] = real;
  return t->count++;
}

static GLint real_location(GLint loc) {
  if (loc < 0 || current_program >= MAX_PROGRAMS || loc >= locations[current_program].count)
    return -1;
  return locations[current_program].real[loc];
}

void glUseProgram_bridge(GLuint prog) {
  current_program = prog;
  glUseProgram(prog);
}

void glUniform1i_bridge(GLint loc, GLint v) { glUniform1i(real_location(loc), v); }
void glUniform1iv_bridge(GLint loc, GLsizei n, const GLint *v) { glUniform1iv(real_location(loc), n, v); }
void glUniform2iv_bridge(GLint loc, GLsizei n, const GLint *v) { glUniform2iv(real_location(loc), n, v); }
void glUniform3iv_bridge(GLint loc, GLsizei n, const GLint *v) { glUniform3iv(real_location(loc), n, v); }
void glUniform4iv_bridge(GLint loc, GLsizei n, const GLint *v) { glUniform4iv(real_location(loc), n, v); }
void glUniform1fv_bridge(GLint loc, GLsizei n, const GLfloat *v) { glUniform1fv(real_location(loc), n, v); }
void glUniform2fv_bridge(GLint loc, GLsizei n, const GLfloat *v) { glUniform2fv(real_location(loc), n, v); }
void glUniform3fv_bridge(GLint loc, GLsizei n, const GLfloat *v) { glUniform3fv(real_location(loc), n, v); }
void glUniform4fv_bridge(GLint loc, GLsizei n, const GLfloat *v) { glUniform4fv(real_location(loc), n, v); }
void glUniformMatrix3fv_bridge(GLint loc, GLsizei n, GLboolean t, const GLfloat *v) {
  glUniformMatrix3fv(real_location(loc), n, t, v);
}
void glUniformMatrix4fv_bridge(GLint loc, GLsizei n, GLboolean t, const GLfloat *v) {
  glUniformMatrix4fv(real_location(loc), n, t, v);
}

void glLinkProgram_bridge(GLuint prog) {
  for (attrib_binding *b = bindings; b; b = b->next) {
    if (b->prog == prog)
      glBindAttribLocation(prog, b->index, b->name);
  }
  locations_clear(prog);  // relinking invalidates every location
  glLinkProgram(prog);
}

void glDeleteProgram_bridge(GLuint prog) {
  for (attrib_binding **pb = &bindings; *pb;) {
    attrib_binding *b = *pb;
    if (b->prog == prog) {
      *pb = b->next;
      free(b->name);
      free(b);
    } else {
      pb = &b->next;
    }
  }
  locations_clear(prog);
  glDeleteProgram(prog);
}

void gl_bridge_init(void) {
  vglSetupRuntimeShaderCompiler(SHARK_OPT_UNSAFE, SHARK_ENABLE, SHARK_ENABLE, SHARK_ENABLE);
  vglInitExtended(0, SCREEN_W, SCREEN_H, VGL_RAM_THRESHOLD, SCE_GXM_MULTISAMPLE_NONE);
  L_INFO("gl: vitaGL ready (%dx%d)", SCREEN_W, SCREEN_H);
}

#include "../util/observe.h"
// EGL on top of vitaGL plus GLES2 entry points that need soft-float shims or
// different answers. Unity is steered to its GLES2 renderer by refusing ES3
// contexts and only exporting GLES2 symbols.

#include "bridges.h"
#include "../config.h"
#include "../util/log.h"
#include "../port_menu.h"

#include <ctype.h>
#include <stdio.h>
#include <psp2/kernel/processmgr.h>
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

static gl_perf_sample gl_perf;
void gl_perf_take(gl_perf_sample *sample) {
  *sample = gl_perf;
  memset(&gl_perf, 0, sizeof(gl_perf));
}

static int egl_error = EGL_SUCCESS;

// Like real EGL, every successful call resets the error state; otherwise
// Unity reads the stale EGL_BAD_MATCH from the refused GLES3 context after a
// later eglMakeCurrent and reports it as a failure.
static int egl_ok(void) {
  egl_error = EGL_SUCCESS;
  return 1;
}

void *eglGetDisplay_bridge(void *native) { return FAKE_DISPLAY; }

int eglInitialize_bridge(void *dpy, int *major, int *minor) {
  if (major) *major = 1;
  if (minor) *minor = 4;
  return egl_ok();
}

int eglTerminate_bridge(void *dpy) { return egl_ok(); }

int eglChooseConfig_bridge(void *dpy, const int *attribs, void **configs, int size, int *num) {
  if (configs && size > 0)
    configs[0] = FAKE_CONFIG;
  if (num)
    *num = 1;
  return egl_ok();
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
  return egl_ok();
}

void *eglCreateWindowSurface_bridge(void *dpy, void *cfg, void *win, const int *attribs) {
  egl_ok();
  return FAKE_SURFACE;
}
int eglDestroySurface_bridge(void *dpy, void *surf) { return egl_ok(); }

void *eglCreateContext_bridge(void *dpy, void *cfg, void *share, const int *attribs) {
  for (const int *a = attribs; a && *a != EGL_NONE; a += 2) {
    if (a[0] == EGL_CONTEXT_CLIENT_VERSION && a[1] > 2) {
      L_INFO("egl: refusing GLES%d context, Unity will fall back to GLES2", a[1]);
      egl_error = EGL_BAD_MATCH;
      return NULL;
    }
  }
  egl_ok();
  return FAKE_CONTEXT;
}

int eglDestroyContext_bridge(void *dpy, void *ctx) { return egl_ok(); }
int eglMakeCurrent_bridge(void *dpy, void *draw, void *read, void *ctx) { return egl_ok(); }

static unsigned shader_cache_counts[6];
static unsigned observed_link_program;
void vgl_shader_cache_event(unsigned event) {
  if (event < 6) {
    __atomic_fetch_add(&shader_cache_counts[event], 1, __ATOMIC_RELAXED);
    const char *names[] = {"cache-hit", "cache-miss", "cache-saved", "cache-error", "compiler-invoked", "boot-preload-used"};
    char label[64]; unsigned program = __atomic_load_n(&observed_link_program, __ATOMIC_RELAXED);
    snprintf(label, sizeof(label), "%s program=%u", names[event], program);
    if (event == 1 || event == 3 || event == 4) observe_span(OBS_SHADER, label, observe_now(), 0, event, program);
  }
}

static void gl_frame_stats(void);

int eglSwapBuffers_bridge(void *dpy, void *surf) {
  gl_frame_stats();
  uint64_t start = sceKernelGetProcessTimeWide();
  vglSwapBuffers(ime_running());  // GL_TRUE lets a common dialog draw on top
  gl_perf.present_us += sceKernelGetProcessTimeWide() - start;
  return egl_ok();
}

int eglQuerySurface_bridge(void *dpy, void *surf, int attr, int *value) {
  if (attr == EGL_WIDTH) *value = SCREEN_W;
  else if (attr == EGL_HEIGHT) *value = SCREEN_H;
  else *value = 0;
  return egl_ok();
}

int eglSwapInterval_bridge(void *dpy, int interval) { return egl_ok(); }

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

// --- diagnostics --------------------------------------------------------------
// vitaGL fails quietly when a GLSL shader doesn't translate or a texture
// format isn't supported, which leaves the screen at the clear colour. These
// wrappers log shader link failures with their sources, every texture format
// seen, GL errors and the number of draw calls per frame.

#define MAX_SHADERS 2049  // vitaGL's MAX_CUSTOM_SHADERS + 1
static char *shader_sources[MAX_SHADERS];
static GLuint program_shaders[1025][2];
static unsigned draws_this_frame, frames_seen;
static int link_failures, links_ok;
static uint8_t prog_linked[1025];
static unsigned draws_skipped, draws_no_program;
static uint64_t link_us_total;
static unsigned links_total, slow_links;

// Unity's fixed-function emulation shaders name a local "texture", which is
// fine in GLSL ES but a reserved word for the Vita's Cg compiler. Such words
// are renamed (whole identifiers only, so texture2D is untouched).
static const char *const cg_reserved[] = {"texture", "sampler", "pass", "technique", "compile", "string"};

static int is_ident(char c) { return isalnum((unsigned char)c) || c == '_'; }

static char *rename_reserved(const char *src) {
  size_t len = strlen(src), extra = 0;
  for (int pass = 0; pass < 2; pass++) {
    char *out = pass ? malloc(len + extra + 1) : NULL, *o = out;
    for (const char *p = src; *p;) {
      if (is_ident(*p) && (p == src || !is_ident(p[-1]))) {
        const char *e = p;
        while (is_ident(*e))
          e++;
        int reserved = 0;
        for (unsigned i = 0; i < sizeof(cg_reserved) / sizeof(*cg_reserved); i++)
          if (strlen(cg_reserved[i]) == (size_t)(e - p) && !memcmp(p, cg_reserved[i], e - p))
            reserved = 1;
        if (pass) {
          memcpy(o, p, e - p);
          o += e - p;
          if (reserved) {
            memcpy(o, "_srr", 4);
            o += 4;
          }
        } else if (reserved) {
          extra += 4;
        }
        p = e;
      } else {
        if (pass)
          *o++ = *p;
        p++;
      }
    }
    if (pass) {
      *o = 0;
      return out;
    }
  }
  return NULL;
}

// Preserve the game's diffuse/envmap equation instead of flattening every
// character to an unlit texture. The common variant also serves world models;
// it has no Z offset and uses signed Lambert rather than half-Lambert.
static int character_compat = 1;
static int material_shaders = 1;
// Stable diffuse pair used before the lit/envmap material replacement.
static const char diffuse_vertex[] =
  "attribute vec4 _glesVertex;\n"
  "attribute vec4 _glesMultiTexCoord0;\n"
  "uniform highp mat4 glstate_matrix_mvp;\n"
  "uniform highp vec4 _DifTex_ST;\n"
  "uniform highp float _InnerZOffset;\n"
  "varying highp vec2 xlv_TEXCOORD0;\n"
  "void main() { gl_Position = glstate_matrix_mvp * _glesVertex;\n"
  "gl_Position.z += _InnerZOffset * 0.01;\n"
  "xlv_TEXCOORD0 = _glesMultiTexCoord0.xy * _DifTex_ST.xy + _DifTex_ST.zw; }\n";
static const char diffuse_fragment[] =
  "precision mediump float;\n"
  "uniform sampler2D _DifTex;\n"
  "uniform highp vec4 _DifCol;\n"
  "varying highp vec2 xlv_TEXCOORD0;\n"
  "void main() { gl_FragColor = vec4(texture2D(_DifTex, xlv_TEXCOORD0).rgb * _DifCol.rgb, 1.0); }\n";
#define CHARACTER_VERTEX_BODY \
  "attribute vec4 _glesVertex;\n" \
  "attribute vec3 _glesNormal;\n" \
  "attribute vec4 _glesMultiTexCoord0;\n" \
  "uniform highp mat4 glstate_matrix_mvp;\n" \
  "uniform highp mat4 glstate_matrix_invtrans_modelview0;\n" \
  "uniform highp vec4 unity_LightPosition[8];\n" \
  "uniform highp vec4 unity_LightColor[8];\n" \
  "uniform highp vec4 glstate_lightmodel_ambient;\n" \
  "uniform highp vec4 _DifTex_ST;\n" \
  "#ifndef SRR_COMMON\n" \
  "uniform highp float _InnerZOffset;\n" \
  "#endif\n" \
  "#ifdef SRR_SCROLL\n" \
  "uniform highp vec4 _ScrollingSpeed;\n" \
  "uniform highp vec4 _Time;\n" \
  "#endif\n" \
  "varying highp vec4 xlv_COLOR;\n" \
  "varying highp vec2 xlv_TEXCOORD0;\n" \
  "varying highp vec2 xlv_TEXCOORD1;\n" \
  "highp vec3 srr_unit(highp vec3 v) { return v * inversesqrt(max(dot(v,v), 0.00000001)); }\n" \
  "void main() {\n" \
  "gl_Position = glstate_matrix_mvp * _glesVertex;\n" \
  "#ifndef SRR_COMMON\n" \
  "gl_Position.z += _InnerZOffset * 0.01;\n" \
  "#endif\n" \
  "#ifdef SRR_SCROLL\n" \
  "xlv_TEXCOORD0 = _glesMultiTexCoord0.xy + fract(_ScrollingSpeed.xy * _Time.y) * _DifTex_ST.xy + _DifTex_ST.zw;\n" \
  "#else\n" \
  "xlv_TEXCOORD0 = _glesMultiTexCoord0.xy * _DifTex_ST.xy + _DifTex_ST.zw;\n" \
  "#endif\n" \
  "highp vec3 n = srr_unit(_glesNormal);\n" \
  "highp vec4 env = glstate_matrix_invtrans_modelview0 * vec4(n, 1.0);\n" \
  "xlv_TEXCOORD1 = env.xy * 0.5 + 0.5;\n" \
  "highp vec3 l = (unity_LightPosition[0] * glstate_matrix_invtrans_modelview0).xyz;\n" \
  "if (unity_LightPosition[0].w != 0.0) l -= _glesVertex.xyz;\n" \
  "highp float diffuse = dot(n, srr_unit(l));\n" \
  "#ifndef SRR_COMMON\n" \
  "diffuse = diffuse * 0.5 + 0.5;\n" \
  "#endif\n" \
  "xlv_COLOR = vec4(diffuse * unity_LightColor[0].rgb + glstate_lightmodel_ambient.rgb, diffuse);\n" \
  "if (dot(unity_LightColor[0].rgb, unity_LightColor[0].rgb) + dot(glstate_lightmodel_ambient.rgb, glstate_lightmodel_ambient.rgb) < 0.00000001) xlv_COLOR = vec4(0.5, 0.5, 0.5, 0.5);\n" \
  "}\n"
static const char character_vertex[] = CHARACTER_VERTEX_BODY;
static const char common_vertex[] = "#define SRR_COMMON\n" CHARACTER_VERTEX_BODY;
static const char scrolling_vertex[] = "#define SRR_SCROLL\n" CHARACTER_VERTEX_BODY;
#undef CHARACTER_VERTEX_BODY
static const char character_fragment[] =
  "precision mediump float;\n"
  "uniform sampler2D _DifTex;\n"
  "uniform sampler2D _EnvTex;\n"
  "uniform highp vec4 _DifCol;\n"
  "varying highp vec4 xlv_COLOR;\n"
  "varying highp vec2 xlv_TEXCOORD0;\n"
  "varying highp vec2 xlv_TEXCOORD1;\n"
  "void main() { highp vec4 tex = texture2D(_DifTex, xlv_TEXCOORD0);\n"
  "highp vec3 env = texture2D(_EnvTex, xlv_TEXCOORD1).rgb;\n"
  "gl_FragColor = vec4(2.0 * (xlv_COLOR.rgb * _DifCol.rgb * tex.rgb + env * tex.a * xlv_COLOR.a), 1.0); }\n";

static const char *character_shader_source(GLenum type, const char *source) {
  if (!character_compat) return source;
  int vertex = type == GL_VERTEX_SHADER && strstr(source, "_DifTex_ST") &&
    strstr(source, "unity_LightPosition") && strstr(source, "glstate_matrix_invtrans_modelview0") &&
    strstr(source, "xlv_COLOR") && strstr(source, "xlv_TEXCOORD1") && !strstr(source, "_RimPower");
  // Glass/falloff shaders share these sampler names, but not this equation.
  // Replacing their fragment alone used to mismatch the original vertex stage.
  int fragment = type == GL_FRAGMENT_SHADER && strstr(source, "_DifTex") &&
    strstr(source, "_EnvTex") && strstr(source, "_DifCol") &&
    strstr(source, "texCol_2.w") && strstr(source, "xlv_COLOR.w") &&
    strstr(source, "vec3(2.0, 2.0, 2.0)") && !strstr(source, "_RimPower");
  if (!vertex && !fragment) return source;
  L_INFO("gl: %s %s shader selected", material_shaders ? "lit diffuse/envmap compatibility" : "simple diffuse compatibility",
         vertex ? "vertex" : "fragment");
  if (!material_shaders) return vertex ? diffuse_vertex : diffuse_fragment;
  if (fragment) return character_fragment;
  if (strstr(source, "_ScrollingSpeed")) return scrolling_vertex;
  return strstr(source, "_InnerZOffset") ? character_vertex : common_vertex;
}

void glShaderSource_bridge(GLuint shader, GLsizei count, const GLchar *const *strings, const GLint *lengths) {
  size_t total = 0;
  for (int i = 0; i < count; i++)
    total += lengths && lengths[i] >= 0 ? (size_t)lengths[i] : strlen(strings[i]);
  char *joined = malloc(total + 1), *p = joined;
  for (int i = 0; i < count; i++) {
    size_t n = lengths && lengths[i] >= 0 ? (size_t)lengths[i] : strlen(strings[i]);
    memcpy(p, strings[i], n);
    p += n;
  }
  *p = 0;
  GLint type = 0;
  glGetShaderiv(shader, GL_SHADER_TYPE, &type);
  char *src = rename_reserved(character_shader_source(type, joined));
  free(joined);
  glShaderSource(shader, 1, (const GLchar *const *)&src, NULL);
  if (shader < MAX_SHADERS) {
    free(shader_sources[shader]);
    shader_sources[shader] = src;
  } else {
    free(src);
  }
}

// --- deferred shader deletion ------------------------------------------------
// GL only destroys a deleted shader once no program references it; Unity
// relies on that (it deletes shaders early and shares them between programs).
// The bundled vitaGL could free such a shader underneath a later
// glLinkProgram (crash in sceGxmProgramGetParameterCount on stage load), so
// deletion is deferred here until no program we know of uses the shader.

static uint8_t shader_delete_pending[MAX_SHADERS];

static int shader_in_use(GLuint shader) {
  for (int p = 0; p < 1025; p++)
    if (program_shaders[p][0] == shader || program_shaders[p][1] == shader)
      return 1;
  return 0;
}

static void release_if_unused(GLuint shader) {
  if (shader && shader < MAX_SHADERS && shader_delete_pending[shader] && !shader_in_use(shader)) {
    shader_delete_pending[shader] = 0;
    free(shader_sources[shader]);
    shader_sources[shader] = NULL;
    glDeleteShader(shader);
  }
}

void glDeleteShader_bridge(GLuint shader) {
  if (!shader || shader >= MAX_SHADERS) {
    glDeleteShader(shader);
    return;
  }
  shader_delete_pending[shader] = 1;
  release_if_unused(shader);
}

void glAttachShader_bridge(GLuint prog, GLuint shader) {
  GLuint replaced = 0;
  if (prog < 1025) {
    GLint type = 0;
    glGetShaderiv(shader, GL_SHADER_TYPE, &type);
    int slot = type == GL_FRAGMENT_SHADER ? 1 : 0;
    replaced = program_shaders[prog][slot];
    program_shaders[prog][slot] = shader;
  }
  glAttachShader(prog, shader);
  if (replaced && replaced != shader)
    release_if_unused(replaced);
}

static void log_source(const char *what, GLuint shader) {
  const char *src = shader < MAX_SHADERS ? shader_sources[shader] : NULL;
  if (!src) {
    L_ERROR("gl:   %s shader %u: source unknown", what, shader);
    return;
  }
  L_ERROR("gl:   %s shader %u source:", what, shader);
  char line[200];
  int lines = 0;
  for (const char *p = src; *p && lines < 60;) {
    size_t n = strcspn(p, "\n");
    snprintf(line, sizeof(line), "%.*s", (int)(n < sizeof(line) - 1 ? n : sizeof(line) - 1), p);
    L_ERROR("gl:     %s", line);
    p += n;
    if (*p)
      p++;
    lines++;
  }
}

static void log_info(const char *what, GLuint shader) {
  if (!shader)
    return;
  char buf[1024];
  GLsizei len = 0;
  glGetShaderInfoLog(shader, sizeof(buf), &len, buf);
  if (len <= 0)
    return;
  L_ERROR("gl:   %s shader %u compiler log:", what, shader);
  for (char *p = buf, *e; *p; p = e) {
    e = p + strcspn(p, "\n");
    char c = *e;
    *e = 0;
    if (*p)
      L_ERROR("gl:     %s", p);
    if (c)
      e++;
  }
}

static void check_link(GLuint prog) {
  GLint ok = 0;
  glGetProgramiv(prog, GL_LINK_STATUS, &ok);
  if (prog < 1025)
    prog_linked[prog] = ok != 0;
  if (ok)
    links_ok++;
  if (ok || link_failures >= 6)
    return;
  link_failures++;
  L_ERROR("gl: program %u failed to link", prog);
  if (prog < 1025) {
    log_info("vertex", program_shaders[prog][0]);
    log_info("fragment", program_shaders[prog][1]);
    log_source("vertex", program_shaders[prog][0]);
    log_source("fragment", program_shaders[prog][1]);
  }
}

static int seen_format(uint32_t key) {
  static uint32_t seen[64];
  static int n;
  for (int i = 0; i < n; i++)
    if (seen[i] == key)
      return 1;
  if (n < 64)
    seen[n++] = key;
  return 0;
}

// vitaGL's normal debug printf is disabled in this build; keep allocation
// failures in the same persistent log as the guest crash report.
void vgl_texture_allocation_failure(const char *operation, uint32_t bytes) {
  L_WARN("gl: texture allocation failed: %s %u bytes; free VRAM=%u RAM=%u PHY=%u KB",
         operation, bytes, (unsigned)(vglMemFree(VGL_MEM_VRAM) / 1024),
         (unsigned)(vglMemFree(VGL_MEM_RAM) / 1024),
         (unsigned)(vglMemFree(VGL_MEM_PHYCONT) / 1024));
}

// Bound disk logging to loading-time diagnostics; never log every small upload.
static unsigned texture_trace_count;
static int trace_texture(const char *call, GLint level, GLsizei w, GLsizei h, const void *data) {
  if ((int64_t)w * h < 1024 * 1024 || texture_trace_count >= 96) return 0;
  L_INFO("gl: texture begin #%u %s level %d %dx%d data %p GPU free VRAM=%u RAM=%u PHY=%u KB",
         ++texture_trace_count, call, level, w, h, data,
         (unsigned)(vglMemFree(VGL_MEM_VRAM) / 1024),
         (unsigned)(vglMemFree(VGL_MEM_RAM) / 1024),
         (unsigned)(vglMemFree(VGL_MEM_PHYCONT) / 1024));
  return 1;
}

void glTexSubImage2D_bridge(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h,
                            GLenum fmt, GLenum type, const void *data) {
  int trace = trace_texture("subimage", level, w, h, data);
  uint64_t start = sceKernelGetProcessTimeWide();
  glTexSubImage2D(target, level, x, y, w, h, fmt, type, data);
  gl_perf.texture_us += sceKernelGetProcessTimeWide() - start;
  if (level == 0 || sceKernelGetProcessTimeWide() - start >= 50000) {
  GLint bound = 0;
  if (target == GL_TEXTURE_2D) glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
  char observed_name[96];
  snprintf(observed_name, sizeof(observed_name), "subimage texture=%d %dx%d mip=%d", bound, w, h, level);
  observe_span(OBS_TEXTURE, observed_name, start, 0, 0, bound);
  }
  if (trace) L_INFO("gl: texture end subimage");
}

// `before` is an error left by an earlier call (e.g. an unsupported
// glTexParameter); only `after` belongs to the upload itself. Each format is
// logged once, plus once more the first time its upload itself fails.
void glTexImage2D_bridge(GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLint border, GLenum fmt,
                         GLenum type, const void *data) {
  GLenum before = glGetError();
  int trace = trace_texture("image", level, w, h, data);
  uint64_t start = sceKernelGetProcessTimeWide();
  glTexImage2D(target, level, ifmt, w, h, border, fmt, type, data);
  gl_perf.texture_us += sceKernelGetProcessTimeWide() - start;
  if (level == 0 || sceKernelGetProcessTimeWide() - start >= 50000) {
  GLint bound = 0;
  if (target == GL_TEXTURE_2D) glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
  char observed_name[96];
  snprintf(observed_name, sizeof(observed_name), "image texture=%d %dx%d mip=%d", bound, w, h, level);
  observe_span(OBS_TEXTURE, observed_name, start, 0, 0, bound);
  }
  if (trace) L_INFO("gl: texture end image");
  GLenum after = glGetError();
  uint32_t key = ((uint32_t)fmt << 16) ^ type ^ ((uint32_t)ifmt << 8);
  if (!seen_format(key) || (after && !seen_format(key | 0x40000000)))
    L_INFO("gl: glTexImage2D ifmt 0x%X fmt 0x%X type 0x%X %dx%d -> error 0x%X (pending before: 0x%X)", ifmt, fmt,
           type, w, h, after, before);
}

void glCompressedTexImage2D_bridge(GLenum target, GLint level, GLenum ifmt, GLsizei w, GLsizei h, GLint border,
                                   GLsizei size, const void *data) {
  GLenum before = glGetError();
  int trace = trace_texture("compressed", level, w, h, data);
  uint64_t start = sceKernelGetProcessTimeWide();
  glCompressedTexImage2D(target, level, ifmt, w, h, border, size, data);
  gl_perf.texture_us += sceKernelGetProcessTimeWide() - start;
  if (level == 0 || sceKernelGetProcessTimeWide() - start >= 50000) {
  GLint bound = 0;
  if (target == GL_TEXTURE_2D) glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
  char observed_name[96];
  snprintf(observed_name, sizeof(observed_name), "compressed texture=%d %dx%d mip=%d", bound, w, h, level);
  observe_span(OBS_TEXTURE, observed_name, start, size > 0 ? size : 0, 0, bound);
  }
  if (trace) L_INFO("gl: texture end compressed");
  GLenum after = glGetError();
  uint32_t key = 0x80000000 | ifmt;
  if (!seen_format(key) || (after && !seen_format(key | 0x40000000)))
    L_INFO("gl: glCompressedTexImage2D ifmt 0x%X %dx%d (%d bytes) -> error 0x%X (pending before: 0x%X)", ifmt, w,
           h, size, after, before);
}

// Reports the first GL_INVALID_ENUM-style error per (call, pname): these are
// the likely source of the errors pending before texture uploads.
static void check_param(const char *call, GLenum pname, GLint value) {
  GLenum err = glGetError();
  if (err && !seen_format(0x20000000 ^ ((uint32_t)pname << 4) ^ (uint32_t)call[2]))
    L_INFO("gl: %s(0x%X, 0x%X) -> error 0x%X", call, pname, value, err);
}

void glTexParameteri_bridge(GLenum target, GLenum pname, GLint param) {
  glTexParameteri(target, pname, param);
  check_param("glTexParameteri", pname, param);
}

void glPixelStorei_bridge(GLenum pname, GLint param) {
  glPixelStorei(pname, param);
  check_param("glPixelStorei", pname, param);
}

void glEnable_bridge(GLenum cap) {
  glEnable(cap);
  check_param("glEnable", cap, 0);
}

void glDisable_bridge(GLenum cap) {
  glDisable(cap);
  check_param("glDisable", cap, 0);
}

// vitaGL draws with whatever state an unlinked program left behind, which can
// crash on the GPU side; such draws are skipped and counted instead.
static GLuint current_program;

static int draw_allowed(void) {
  draws_this_frame++;
  if (!current_program) {
    draws_no_program++;
    return 1;
  }
  if (current_program < 1025 && !prog_linked[current_program]) {
    draws_skipped++;
    return 0;
  }
  return 1;
}

void glDrawElements_bridge(GLenum mode, GLsizei count, GLenum type, const void *indices) {
  if (!draw_allowed())
    return;
  uint64_t start = sceKernelGetProcessTimeWide();
  glDrawElements(mode, count, type, indices);
  gl_perf.draw_us += sceKernelGetProcessTimeWide() - start;
}

void glDrawArrays_bridge(GLenum mode, GLint first, GLsizei count) {
  if (!draw_allowed())
    return;
  uint64_t start = sceKernelGetProcessTimeWide();
  glDrawArrays(mode, first, count);
  gl_perf.draw_us += sceKernelGetProcessTimeWide() - start;
}

static void gl_frame_stats(void) {
  frames_seen++;
  if (frames_seen == 1 || frames_seen % 600 == 0) {
    GLenum err = glGetError();
    L_INFO("gl: frame %u: %u draw calls, glGetError 0x%X, programs linked %d (failed %d)", frames_seen,
           draws_this_frame, err, links_ok, link_failures);
    L_INFO("gl:   draws skipped (unlinked program) %u, without program %u; %u links took %u ms (%u over 100 ms)",
           draws_skipped, draws_no_program, links_total, (unsigned)(link_us_total / 1000), slow_links);
    unsigned cache[5];
    for (unsigned i = 0; i < 5; i++) cache[i] = __atomic_load_n(&shader_cache_counts[i], __ATOMIC_RELAXED);
    L_INFO("gl: shader cache hits %u misses %u saved %u errors %u compiler calls %u",
           cache[0], cache[1], cache[2], cache[3], cache[4]);
    L_INFO("gl: boot-prepared shader programs reused %u", __atomic_load_n(&shader_cache_counts[5], __ATOMIC_RELAXED));
    draws_skipped = draws_no_program = 0;
  }
  draws_this_frame = 0;
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

static void locations_clear(GLuint prog) {
  if (prog < MAX_PROGRAMS) {
    free(locations[prog].real);
    memset(&locations[prog], 0, sizeof(location_table));
  }
}

GLint glGetUniformLocation_bridge(GLuint prog, const GLchar *name) {
  GLint real = glGetUniformLocation(prog, name);
  size_t length = strlen(name);
  if (real == -1 && length > 3 && !strcmp(name + length - 3, "[0]")) {
    char *base = strdup(name);
    if (base) {
      base[length - 3] = 0;
      real = glGetUniformLocation(prog, base);
      free(base);
    }
  }
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
  // vitaGL dereferences both shaders unconditionally; a program missing one
  // would crash instead of simply failing to link.
  if (prog < 1025 && (!program_shaders[prog][0] || !program_shaders[prog][1])) {
    L_ERROR("gl: program %u linked without a %s shader; skipped", prog,
            program_shaders[prog][0] ? "fragment" : "vertex");
    return;
  }
  for (attrib_binding *b = bindings; b; b = b->next) {
    if (b->prog == prog)
      glBindAttribLocation(prog, b->index, b->name);
  }
  locations_clear(prog);  // relinking invalidates every location
  // Shaders are compiled here (vitaGL's postponed mode), so this is where
  // loading time goes; the shader cache makes later runs skip it.
  SceUInt64 t0 = sceKernelGetProcessTimeWide();
  __atomic_store_n(&observed_link_program, prog, __ATOMIC_RELAXED);
  glLinkProgram(prog);
  __atomic_store_n(&observed_link_program, 0, __ATOMIC_RELAXED);
  SceUInt64 us = sceKernelGetProcessTimeWide() - t0;
  links_total++;
  link_us_total += us;
  gl_perf.link_us += us;
  if (us > 100000) {
    slow_links++;
    if (slow_links <= 10)
      L_INFO("gl: program %u took %u ms to compile and link", prog, (unsigned)(us / 1000));
  }
  check_link(prog);
  // Record the reflected channels for lit materials. Successful compilation
  // alone does not tell us whether Unity sees the normal and UV attributes.
  GLuint vertex = prog < 1025 ? program_shaders[prog][0] : 0;
  const char *source = vertex < MAX_SHADERS ? shader_sources[vertex] : NULL;
  if (prog < 1025 && prog_linked[prog] && source && strstr(source, "unity_LightPosition")) {
    GLint active = 0;
    glGetProgramiv(prog, GL_ACTIVE_ATTRIBUTES, &active);
    for (GLint i = 0; i < active && i < 16; i++) {
      GLchar name[96] = {0}; GLsizei length = 0; GLint size = 0; GLenum type = 0;
      glGetActiveAttrib(prog, i, sizeof(name), &length, &size, &type, name);
      L_INFO("gl: lit material program %u attribute %d name=%s location=%d type=0x%X size=%d",
             prog, i, name, glGetAttribLocation(prog, name), type, size);
    }
  }
  char observed_name[96];
  snprintf(observed_name, sizeof(observed_name), "program=%u vertex=%u fragment=%u", prog,
           prog < 1025 ? program_shaders[prog][0] : 0, prog < 1025 ? program_shaders[prog][1] : 0);
  observe_span(OBS_SHADER, observed_name, t0, 0, prog < 1025 ? prog_linked[prog] : 0, prog);
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
  GLuint vs = 0, fs = 0;
  if (prog < 1025) {
    vs = program_shaders[prog][0];
    fs = program_shaders[prog][1];
    program_shaders[prog][0] = program_shaders[prog][1] = 0;
    prog_linked[prog] = 0;
  }
  glDeleteProgram(prog);
  release_if_unused(vs);
  release_if_unused(fs);
}

void gl_bridge_init(void) {
  character_compat = 1;
  material_shaders = port_material_shaders();
  L_INFO("gl: material shaders %s (%s)", material_shaders ? "on" : "off",
         material_shaders ? "lit diffuse/envmap compatibility" : "simple diffuse compatibility");
  // Keep standard optimization rather than unsafe algebra on legacy Unity
  // shaders; lighting normalizes vectors that can be zero during transitions.
  vglSetupRuntimeShaderCompiler(SHARK_OPT_FAST, SHARK_DISABLE, SHARK_ENABLE, SHARK_ENABLE);
  vglSetShaderCachePath(DATA_PATH "/shader_cache_v35");
  // Render size from the port settings; the Vita's display scaler stretches
  // SD/HD to the screen.
  vglInitExtended(0, SCREEN_W, SCREEN_H, VGL_RAM_THRESHOLD, SCE_GXM_MULTISAMPLE_NONE);
  L_INFO("gl: vitaGL ready (%dx%d)", SCREEN_W, SCREEN_H);
}

// Unity's own eglSwapInterval calls stay ignored; the port settings pick 1
// (60 Hz) or 2 (30 fps with even pacing).
void gl_set_swap_interval(int interval) { eglSwapInterval(NULL, interval); }

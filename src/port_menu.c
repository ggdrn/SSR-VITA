// Port settings screen, opened from the "Settings" button in the LiveArea
// (target psla:-settings) or by holding SELECT while the game starts. It runs before Unity is loaded, drawing with the CPU into its
// own display buffer, and ends with "Start Game" or "Exit" (back to the
// LiveArea). Settings are only applied at boot: changing them while Unity is
// running caused visible glitches.
//
// Settings are stored in PORT_SETTINGS_PATH as key=value lines.

#include "port_menu.h"
#include "config.h"
#include "util/log.h"
#include "util/port_font.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/appmgr.h>
#include <psp2/apputil.h>
#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/touch.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <vitaGL.h>

// vitaGL globals describing the buffer being handed to the display.
extern int DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_STRIDE;
void gl_set_swap_interval(int interval);

#define PORT_SETTINGS_PATH DATA_PATH "/port_settings.txt"

static const struct {
  int w, h;
  const char *name;
} resolutions[PORT_RES_COUNT] = {
    {640, 368, "SD 640x368"},
    {720, 408, "HD 720x408"},
    {960, 544, "QHD 960x544"},
};

static struct {
  int resolution, quality, fps, fps_counter, material_shaders;
} cfg = {PORT_RES_QHD, PORT_QUALITY_DEFAULT, 60, 0, 1};

int port_screen_w = SCREEN_MAX_W, port_screen_h = SCREEN_MAX_H;

int port_material_shaders(void) { return cfg.material_shaders; }
int port_quality(void) { return cfg.quality; }
// Unity's own levels in this APK: 0 Fastest (the game's Light Mode), 1 Fast
// (the game's Normal), 2 Simple, 3 Good, 4 Beautiful, 5 Fantastic.
int port_unity_quality_level(void) { return cfg.quality == PORT_QUALITY_LOW ? 0 : 1; }
// Keep the half-size texture budget even in Light mode. Quarter-size font
// atlases lose too much glyph detail; the engine quality level stays Low.
int port_texture_limit(void) { return 1; }
int port_target_fps(void) { return cfg.fps; }

// --- Settings file ----------------------------------------------------------

static int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

void port_settings_load(void) {
  FILE *f = fopen(PORT_SETTINGS_PATH, "r");
  if (f) {
    char line[96];
    while (fgets(line, sizeof(line), f)) {
      char key[48];
      int value;
      if (sscanf(line, " %47[^= ] = %d", key, &value) != 2) continue;
      if (!strcmp(key, "resolution")) cfg.resolution = clamp(value, 0, PORT_RES_COUNT - 1);
      else if (!strcmp(key, "graphics")) cfg.quality = clamp(value, 0, PORT_QUALITY_COUNT - 1);
      else if (!strcmp(key, "fps")) cfg.fps = value == 30 ? 30 : 60;
      else if (!strcmp(key, "fps_counter")) cfg.fps_counter = !!value;
      else if (!strcmp(key, "material_shaders")) cfg.material_shaders = !!value;
    }
    fclose(f);
  }
  port_screen_w = resolutions[cfg.resolution].w;
  port_screen_h = resolutions[cfg.resolution].h;
  L_INFO("port: settings %s, graphics %d, %d fps, fps counter %d, material shaders %s", resolutions[cfg.resolution].name,
         cfg.quality, cfg.fps, cfg.fps_counter, cfg.material_shaders ? "on" : "off");
}

static void settings_save(void) {
  FILE *f = fopen(PORT_SETTINGS_PATH, "w");
  if (!f) {
    L_WARN("port: could not write " PORT_SETTINGS_PATH);
    return;
  }
  fprintf(f, "resolution=%d\ngraphics=%d\nfps=%d\nfps_counter=%d\nmaterial_shaders=%d\n", cfg.resolution,
          cfg.quality, cfg.fps, cfg.fps_counter, cfg.material_shaders);
  fclose(f);
}

// --- CPU drawing --------------------------------------------------------------

#define RGB(r, g, b) (0xFF000000u | ((uint32_t)(b) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(r))

static void draw_rect(uint32_t *fb, int stride, int x, int y, int w, int h, uint32_t color) {
  for (int yy = y; yy < y + h; yy++) {
    uint32_t *p = fb + yy * stride + x;
    for (int xx = 0; xx < w; xx++) p[xx] = color;
  }
}

// Glyphs are scaled from 8x8 to g x g with nearest sampling.
static void draw_text(uint32_t *fb, int stride, int x, int y, int g, const char *s, uint32_t color) {
  for (; *s; s++, x += g) {
    unsigned c = (unsigned char)*s;
    if (c < 32 || c > 126 || c == ' ') continue;
    const uint8_t *glyph = port_font[c - 32];
    for (int yy = 0; yy < g; yy++) {
      uint8_t bits = glyph[yy * 8 / g];
      if (!bits) continue;
      uint32_t *p = fb + (y + yy) * stride + x;
      for (int xx = 0; xx < g; xx++)
        if (bits & (0x80 >> (xx * 8 / g))) p[xx] = color;
    }
  }
}

// --- Shader cache preparation screen (before Unity starts) -------------------
static unsigned shader_phase, shader_done, shader_total, shader_prepared;
static void shader_progress_callback(void *framebuf) {
  int w = DISPLAY_WIDTH, h = DISPLAY_HEIGHT, stride = DISPLAY_STRIDE;
  int g = w * 20 / 960;
  draw_rect(framebuf, stride, 0, 0, w, h, RGB(0, 0, 0));
  const char *title = shader_phase == 0 ? "Scanning shader cache..." :
                      !shader_total ? "No cached shaders yet." :
                      shader_phase == 2 ? "Shader preparation complete" : "Preparing cached shaders...";
  int x = (w - (int)strlen(title) * g) / 2;
  draw_text(framebuf, stride, x, h / 2 - 60, g, title, RGB(255, 205, 40));
  if (shader_total) {
    char text[72];
    snprintf(text, sizeof(text), "%u / %u  (%u%%)", shader_done, shader_total, (unsigned)((uint64_t)shader_done * 100 / shader_total));
    draw_text(framebuf, stride, (w - (int)strlen(text) * g) / 2, h / 2 - 15, g, text, RGB(235, 235, 235));
    int bar = w * 3 / 4;
    draw_rect(framebuf, stride, w / 8, h / 2 + 25, bar, 14, RGB(48, 48, 48));
    draw_rect(framebuf, stride, w / 8, h / 2 + 25, (int)((uint64_t)bar * shader_done / shader_total), 14, RGB(255, 205, 40));
    snprintf(text, sizeof(text), "%u prepared - %u skipped", shader_prepared, shader_done - shader_prepared);
    draw_text(framebuf, stride, (w - (int)strlen(text) * (g * 3 / 4)) / 2, h / 2 + 60, g * 3 / 4, text, RGB(180, 190, 205));
  } else if (shader_phase != 0) {
    const char *text = "The cache will be created while you play.";
    int small = g * 3 / 4;
    draw_text(framebuf, stride, (w - (int)strlen(text) * small) / 2, h / 2, small, text, RGB(180, 190, 205));
  }
}
void port_shader_progress(unsigned phase, unsigned done, unsigned total, unsigned prepared) {
  if (phase == 3) { vglSetDisplayCallback(NULL); return; }
  shader_phase = phase; shader_total = total;
  shader_done = done > total ? total : done;
  shader_prepared = prepared > shader_done ? shader_done : prepared;
  vglSetDisplayCallback(shader_progress_callback);
  vglSwapBuffers(0);
}

// --- In-game FPS counter ------------------------------------------------------

static void fps_display_callback(void *framebuf) {
  static unsigned frames, shown;
  static uint64_t window_start;
  uint64_t now = sceKernelGetProcessTimeWide();
  frames++;
  if (now - window_start >= 1000000) {
    shown = window_start ? (unsigned)((frames * 1000000ull + (now - window_start) / 2) / (now - window_start)) : 0;
    frames = 0;
    window_start = now;
  }
  int g = DISPLAY_WIDTH * 16 / 960;  // 16 px at 960, 12 at 720, 10 at 640
  char text[16];
  snprintf(text, sizeof(text), "%u FPS", shown);
  draw_rect(framebuf, DISPLAY_STRIDE, 0, 0, ((int)strlen(text) + 1) * g, g + g / 2, RGB(0, 0, 0));
  draw_text(framebuf, DISPLAY_STRIDE, g / 2, g / 4, g, text,
            shown >= 55 ? RGB(80, 230, 80) : shown >= 28 ? RGB(255, 205, 40) : RGB(255, 80, 80));
}

void port_menu_init(void) {
  if (cfg.fps_counter) vglSetDisplayCallback(fps_display_callback);
  // A swap interval of 2 paces 30 fps on the 60 Hz display evenly.
  gl_set_swap_interval(cfg.fps == 30 ? 2 : 1);
}

// --- Settings screen (before the game loads) ----------------------------------

enum { ROW_RESOLUTION, ROW_GRAPHICS, ROW_MATERIAL_SHADERS, ROW_FPS, ROW_FPS_COUNTER, ROW_START, ROW_EXIT, ROW_COUNT };

static const char *const row_names[ROW_COUNT] = {"Resolution", "Graphics", "Material Shaders", "Frame Rate", "FPS Counter",
                                                 "Start Game", "Exit"};
static const char *const quality_names[] = {"Low", "Default"};

static void row_value(int row, char *out, size_t size) {
  switch (row) {
  case ROW_RESOLUTION: snprintf(out, size, "%s", resolutions[cfg.resolution].name); break;
  case ROW_GRAPHICS: snprintf(out, size, "%s", quality_names[cfg.quality]); break;
  case ROW_MATERIAL_SHADERS: snprintf(out, size, "%s", cfg.material_shaders ? "On" : "Off"); break;
  case ROW_FPS: snprintf(out, size, "%d FPS", cfg.fps); break;
  case ROW_FPS_COUNTER: snprintf(out, size, "%s", cfg.fps_counter ? "On" : "Off"); break;
  default: out[0] = 0;
  }
}

static const char *row_hint(int row) {
  switch (row) {
  case ROW_RESOLUTION:
    return cfg.resolution == PORT_RES_QHD ? "Native Vita resolution." : "Rendered smaller, upscaled. Faster GPU.";
  case ROW_GRAPHICS:
    return cfg.quality == PORT_QUALITY_LOW ? "Light quality. Readable half-size textures."
                                              : "Game's Normal quality. Half-size textures.";
  case ROW_MATERIAL_SHADERS: return cfg.material_shaders ? "Lighting, reflections and material variants." : "Off: simple diffuse rendering.";
  case ROW_FPS: return cfg.fps == 30 ? "Locked to 30 FPS with even frame pacing." : "Targets 60 FPS.";
  case ROW_FPS_COUNTER: return "Shows frames per second in the corner.";
  case ROW_START: return "Save and start the game.";
  case ROW_EXIT: return "Save and return to the LiveArea.";
  default: return "";
  }
}

static void change(int row, int dir) {
  switch (row) {
  case ROW_RESOLUTION: cfg.resolution = (cfg.resolution + dir + PORT_RES_COUNT) % PORT_RES_COUNT; break;
  case ROW_GRAPHICS: cfg.quality = (cfg.quality + dir + PORT_QUALITY_COUNT) % PORT_QUALITY_COUNT; break;
  case ROW_MATERIAL_SHADERS: cfg.material_shaders = !cfg.material_shaders; break;
  case ROW_FPS: cfg.fps = cfg.fps == 60 ? 30 : 60; break;
  case ROW_FPS_COUNTER: cfg.fps_counter = !cfg.fps_counter; break;
  default: return;
  }
  settings_save();
}

#define FB_W 960
#define FB_H 544
#define G 20      // glyph size in pixels
#define LINE 34   // row height

static int row_y(int row) {
  return 100 + row * LINE + (row >= ROW_START ? LINE / 2 : 0);
}

static int row_at(int x, int y) {
  if (x < 30 || x >= FB_W - 30) return -1;
  for (int row = 0; row < ROW_COUNT; row++)
    if (y >= row_y(row) - 7 && y < row_y(row) - 7 + LINE) return row;
  return -1;
}

static void draw_screen(uint32_t *fb, int selected) {
  draw_rect(fb, FB_W, 0, 0, FB_W, FB_H, RGB(0, 0, 0));
  draw_rect(fb, FB_W, 0, 70, FB_W, 3, RGB(255, 205, 40));
  draw_text(fb, FB_W, 40, 25, G, "SONIC RUNNERS REVIVAL", RGB(255, 205, 40));
  draw_text(fb, FB_W, 40 + 22 * G, 25, G, "- PORT MENU", RGB(235, 235, 235));

  for (int i = 0; i < ROW_COUNT; i++) {
    int y = row_y(i);
    if (i == selected) draw_rect(fb, FB_W, 30, y - 7, FB_W - 60, LINE, RGB(48, 48, 48));
    uint32_t color = i == selected ? RGB(255, 255, 255) : RGB(200, 210, 230);
    draw_text(fb, FB_W, 50, y, G, row_names[i], color);
    char value[32];
    row_value(i, value, sizeof(value));
    if (value[0]) {
      int vx = 480;
      if (i == selected) draw_text(fb, FB_W, vx - 2 * G, y, G, "<", RGB(255, 205, 40));
      draw_text(fb, FB_W, vx, y, G, value, color);
      if (i == selected) draw_text(fb, FB_W, vx + 13 * G, y, G, ">", RGB(255, 205, 40));
    }
  }
  draw_rect(fb, FB_W, 0, FB_H - 92, FB_W, 3, RGB(255, 205, 40));
  draw_rect(fb, FB_W, 0, FB_H - 89, FB_W, 89, RGB(0, 0, 0));
  draw_text(fb, FB_W, 40, FB_H - 72, G, row_hint(selected), RGB(200, 200, 200));
  draw_text(fb, FB_W, 40, FB_H - 36, 16, "Tap to change/play   D-pad select/change   X OK   O exit", RGB(150, 150, 150));
}

// Match the LiveArea launch option format used by GTA SA Vita (psla:-config).
// This runs before APK/Unity loading; the engine starts only after Start Game.
static int livearea_settings_event(int startup) {
  SceAppUtilAppEventParam event;
  memset(&event, 0, sizeof(event));
  int received = sceAppUtilReceiveAppEvent(&event);
  if (startup || event.type)
    L_INFO("port: AppUtil receive=0x%08X type=0x%X", received, event.type);
  if (received < 0 || event.type != 0x05) return 0;
  char arg[2048];
  memset(arg, 0, sizeof(arg));
  int parsed = sceAppUtilAppEventParseLiveArea(&event, arg);
  arg[sizeof(arg) - 1] = 0;
  L_INFO("port: LiveArea parse=0x%08X argument=\"%s\"", parsed, arg);
  return parsed >= 0 && strstr(arg, "settings") != NULL;
}

int port_settings_requested(int argc, char *argv[]) {
  for (int i = 0; i < argc; i++)
    if (argv[i] && (!strcmp(argv[i], "settings") || !strcmp(argv[i], "-settings"))) {
      L_INFO("port: settings requested on restart");
      return 1;
    }
  if (livearea_settings_event(1)) {
    L_INFO("port: settings requested from the LiveArea");
    return 1;
  }
  char param[1024];
  memset(param, 0, sizeof(param));
  if (sceAppMgrGetAppParam(param) >= 0 && param[0]) {
    L_INFO("port: app param \"%s\"", param);
    if (strstr(param, "settings")) return 1;
  }
  SceCtrlData pad;
  memset(&pad, 0, sizeof(pad));
  if (sceCtrlPeekBufferPositive(0, &pad, 1) > 0 && (pad.buttons & SCE_CTRL_SELECT)) {
    L_INFO("port: settings requested by holding SELECT");
    return 1;
  }
  L_INFO("port: normal launch; no settings request detected");
  return 0;
}

// The Settings button was tapped while the game was running (the system just
// resumes it): restart into the settings screen, since settings only apply at
// boot.
void port_settings_poll(void) {
  if (!livearea_settings_event(0)) return;
  L_INFO("port: restarting into the settings screen");
  log_finish();
  static char arg[] = "settings";
  char *const argv[] = {arg, NULL};
  int r = sceAppMgrLoadExec("app0:/eboot.bin", argv, NULL);
  L_WARN("port: restart failed (0x%08X)", r);
}

int port_settings_screen(void) {
  const unsigned size = (FB_W * FB_H * 4 + 0x3FFFF) & ~0x3FFFF;
  SceUID block = sceKernelAllocMemBlock("port_settings_fb", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, size, NULL);
  void *base = NULL;
  if (block < 0 || sceKernelGetMemBlockBase(block, &base) < 0) {
    L_WARN("port: no display memory for the settings screen (0x%08X)", block);
    if (block >= 0) sceKernelFreeMemBlock(block);
    return 0;  // A failed settings screen must never silently start the game.
  }
  SceDisplayFrameBuf fb;
  memset(&fb, 0, sizeof(fb));
  fb.size = sizeof(fb);
  fb.base = base;
  fb.pitch = FB_W;
  fb.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
  fb.width = FB_W;
  fb.height = FB_H;

  // Ignore a finger still held from the LiveArea launch. Activate on release
  // in the same row; restore the sampling state before handing off to Unity.
  SceTouchSamplingState touch_state = SCE_TOUCH_SAMPLING_STATE_STOP;
  sceTouchGetSamplingState(SCE_TOUCH_PORT_FRONT, &touch_state);
  int touch_ready = sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START) >= 0;
  int touch_armed = 0, touch_row = -1, touch_id = -1, touch_dir = 1;
  int selected = ROW_START, result = -1;
  uint32_t prev = ~0u;  // Ignore buttons still held from the launch.
  uint64_t repeat_at = 0;
  draw_screen(base, selected);
  sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME);
  L_INFO("port: settings screen open");

  while (result < 0) {
    sceDisplayWaitVblankStart();
    SceCtrlData pad;
    memset(&pad, 0, sizeof(pad));
    sceCtrlPeekBufferPositive(0, &pad, 1);
    uint32_t buttons = pad.buttons, pressed = buttons & ~prev;
    prev = buttons;
    uint64_t now = sceKernelGetProcessTimeWide();
    uint32_t vertical = SCE_CTRL_UP | SCE_CTRL_DOWN;
    uint32_t act = pressed;
    if (pressed & (vertical | SCE_CTRL_LEFT | SCE_CTRL_RIGHT))
      repeat_at = now + 400000;
    else if ((buttons & vertical) && now >= repeat_at) {
      act |= buttons & vertical;
      repeat_at = now + 120000;
    }
    int previous_selection = selected;
    int tapped = -1;
    SceTouchData touch;
    memset(&touch, 0, sizeof(touch));
    if (touch_ready && sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1) > 0) {
      if (!touch.reportNum) {
        if (touch_armed) tapped = touch_row;
        touch_armed = 1;
        touch_row = touch_id = -1;
      } else if (touch_armed) {
        // Vita front touch coordinates are twice the native display size.
        int x = touch.report[0].x / 2, y = touch.report[0].y / 2;
        int row = row_at(x, y);
        if (touch_id < 0 && touch.reportNum == 1) {
          touch_id = touch.report[0].id;
          touch_row = row;
          touch_dir = x < 480 ? -1 : 1;
          if (row >= 0) selected = row;
        } else if (touch.reportNum != 1 || touch.report[0].id != touch_id || row != touch_row) {
          // Dragging out of a row or using multiple fingers cancels the tap.
          touch_row = -1;
          touch_armed = 0;
        }
      }
    }
    if (tapped >= 0) {
      selected = tapped;
      if (tapped == ROW_START) result = 1;
      else if (tapped == ROW_EXIT) result = 0;
      else change(tapped, touch_dir);
    }
    if (!act && tapped < 0 && selected == previous_selection) continue;
    if (act & SCE_CTRL_UP) selected = (selected + ROW_COUNT - 1) % ROW_COUNT;
    if (act & SCE_CTRL_DOWN) selected = (selected + 1) % ROW_COUNT;
    if (act & SCE_CTRL_LEFT) change(selected, -1);
    if (act & SCE_CTRL_RIGHT) change(selected, +1);
    if (act & SCE_CTRL_CROSS) {
      if (selected == ROW_START) result = 1;
      else if (selected == ROW_EXIT) result = 0;
      else change(selected, +1);
    }
    if (act & SCE_CTRL_CIRCLE) result = 0;
    draw_screen(base, selected);
  }

  if (touch_ready) sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, touch_state);
  settings_save();
  port_screen_w = resolutions[cfg.resolution].w;
  port_screen_h = resolutions[cfg.resolution].h;
  L_INFO("port: settings screen closed (%s)", result ? "start game" : "exit");
  // Blank the display before giving the memory back; vitaGL sets its own
  // buffers when the game starts.
  memset(base, 0, FB_W * FB_H * 4);
  sceDisplaySetFrameBuf(NULL, SCE_DISPLAY_SETBUF_NEXTFRAME);
  sceDisplayWaitVblankStart();
  sceDisplayWaitVblankStart();
  sceKernelFreeMemBlock(block);
  return result;
}

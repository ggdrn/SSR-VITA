// Port settings screen, opened from the "Settings" button in the LiveArea
// (psgm:play?titleid=SRRV00001&param=settings) or by holding SELECT while the
// game starts. It runs before Unity is loaded, drawing with the CPU into its
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
#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <vitaGL.h>

// vitaGL globals describing the buffer being handed to the display.
extern int DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_STRIDE;
void gl_set_swap_interval(int interval);

#define PORT_SETTINGS_PATH DATA_PATH "/port_settings.txt"
#define CHARACTER_MARKER_PATH DATA_PATH "/original_character_shader"

static const struct {
  int w, h;
  const char *name;
} resolutions[PORT_RES_COUNT] = {
    {640, 368, "SD 640x368"},
    {720, 408, "HD 720x408"},
    {960, 544, "QHD 960x544"},
};

static struct {
  int resolution, quality, fps, fps_counter, character_original;
} cfg = {PORT_RES_QHD, PORT_QUALITY_MEDIUM, 60, 0, 0};

int port_screen_w = SCREEN_MAX_W, port_screen_h = SCREEN_MAX_H;

int port_quality(void) { return cfg.quality; }
// Unity's own levels in this APK: 0 Fastest (the game's Light Mode), 1 Fast
// (the game's Normal), 2 Simple, 3 Good, 4 Beautiful, 5 Fantastic.
int port_unity_quality_level(void) { return cfg.quality == PORT_QUALITY_LOW ? 0 : cfg.quality == PORT_QUALITY_HIGH ? 2 : 1; }
// masterTextureLimit 1 (half size) is the memory cap the port has always used.
int port_texture_limit(void) { return cfg.quality == PORT_QUALITY_LOW ? 2 : 1; }
int port_target_fps(void) { return cfg.fps; }
int port_character_shader_original(void) { return cfg.character_original; }

// --- Settings file ----------------------------------------------------------

static int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

void port_settings_load(void) {
  FILE *marker = fopen(CHARACTER_MARKER_PATH, "rb");
  if (marker) {
    cfg.character_original = 1;  // Older opt-in kept as the default.
    fclose(marker);
  }
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
      else if (!strcmp(key, "character_shader_original")) cfg.character_original = !!value;
    }
    fclose(f);
  }
  port_screen_w = resolutions[cfg.resolution].w;
  port_screen_h = resolutions[cfg.resolution].h;
  L_INFO("port: settings %s, graphics %d, %d fps, fps counter %d, character shader %s", resolutions[cfg.resolution].name,
         cfg.quality, cfg.fps, cfg.fps_counter, cfg.character_original ? "original" : "compatible");
}

static void settings_save(void) {
  FILE *f = fopen(PORT_SETTINGS_PATH, "w");
  if (!f) {
    L_WARN("port: could not write " PORT_SETTINGS_PATH);
    return;
  }
  fprintf(f, "resolution=%d\ngraphics=%d\nfps=%d\nfps_counter=%d\ncharacter_shader_original=%d\n", cfg.resolution,
          cfg.quality, cfg.fps, cfg.fps_counter, cfg.character_original);
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

enum { ROW_RESOLUTION, ROW_GRAPHICS, ROW_FPS, ROW_FPS_COUNTER, ROW_CHARACTER, ROW_START, ROW_EXIT, ROW_COUNT };

static const char *const row_names[ROW_COUNT] = {"Resolution", "Graphics", "Frame Rate", "FPS Counter",
                                                 "Character Shader", "Start Game", "Exit"};
static const char *const quality_names[] = {"Low", "Medium", "High"};

static void row_value(int row, char *out, size_t size) {
  switch (row) {
  case ROW_RESOLUTION: snprintf(out, size, "%s", resolutions[cfg.resolution].name); break;
  case ROW_GRAPHICS: snprintf(out, size, "%s", quality_names[cfg.quality]); break;
  case ROW_FPS: snprintf(out, size, "%d FPS", cfg.fps); break;
  case ROW_FPS_COUNTER: snprintf(out, size, "%s", cfg.fps_counter ? "On" : "Off"); break;
  case ROW_CHARACTER: snprintf(out, size, "%s", cfg.character_original ? "Original" : "Compatible"); break;
  default: out[0] = 0;
  }
}

static const char *row_hint(int row) {
  switch (row) {
  case ROW_RESOLUTION:
    return cfg.resolution == PORT_RES_QHD ? "Native Vita resolution." : "Rendered smaller, upscaled. Faster GPU.";
  case ROW_GRAPHICS:
    return cfg.quality == PORT_QUALITY_LOW ? "Game's Light quality. Quarter-size textures."
           : cfg.quality == PORT_QUALITY_HIGH ? "Unity 'Simple' level: sharper filtering."
                                              : "Game's Normal quality. Half-size textures.";
  case ROW_FPS: return cfg.fps == 30 ? "Locked to 30 FPS with even frame pacing." : "Targets 60 FPS.";
  case ROW_FPS_COUNTER: return "Shows frames per second in the corner.";
  case ROW_CHARACTER:
    return cfg.character_original ? "The game's original env-map shader." : "Stable diffuse shading (recommended).";
  case ROW_START: return "Save and start the game.";
  case ROW_EXIT: return "Save and return to the LiveArea.";
  default: return "";
  }
}

static void change(int row, int dir) {
  switch (row) {
  case ROW_RESOLUTION: cfg.resolution = (cfg.resolution + dir + PORT_RES_COUNT) % PORT_RES_COUNT; break;
  case ROW_GRAPHICS: cfg.quality = (cfg.quality + dir + PORT_QUALITY_COUNT) % PORT_QUALITY_COUNT; break;
  case ROW_FPS: cfg.fps = cfg.fps == 60 ? 30 : 60; break;
  case ROW_FPS_COUNTER: cfg.fps_counter = !cfg.fps_counter; break;
  case ROW_CHARACTER: cfg.character_original = !cfg.character_original; break;
  default: return;
  }
  settings_save();
}

#define FB_W 960
#define FB_H 544
#define G 20      // glyph size in pixels
#define LINE 34   // row height

static void draw_screen(uint32_t *fb, int selected) {
  // Background: vertical blue gradient.
  for (int y = 0; y < FB_H; y++) {
    uint32_t c = RGB(8, 24 + y * 40 / FB_H, 70 + y * 60 / FB_H);
    uint32_t *p = fb + y * FB_W;
    for (int x = 0; x < FB_W; x++) p[x] = c;
  }
  draw_rect(fb, FB_W, 0, 0, FB_W, 70, RGB(10, 20, 50));
  draw_rect(fb, FB_W, 0, 70, FB_W, 3, RGB(255, 205, 40));
  draw_text(fb, FB_W, 40, 25, G, "SONIC RUNNERS REVIVAL", RGB(255, 205, 40));
  draw_text(fb, FB_W, 40 + 22 * G, 25, G, "- SETTINGS", RGB(235, 235, 235));

  int y = 100;
  for (int i = 0; i < ROW_COUNT; i++) {
    if (i == ROW_START) y += LINE / 2;
    if (i == selected) draw_rect(fb, FB_W, 30, y - 7, FB_W - 60, LINE, RGB(40, 80, 170));
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
    y += LINE;
  }
  draw_rect(fb, FB_W, 0, FB_H - 92, FB_W, 3, RGB(255, 205, 40));
  draw_rect(fb, FB_W, 0, FB_H - 89, FB_W, 89, RGB(10, 20, 50));
  draw_text(fb, FB_W, 40, FB_H - 72, G, row_hint(selected), RGB(150, 190, 255));
  draw_text(fb, FB_W, 40, FB_H - 36, 16, "UP/DOWN select   LEFT/RIGHT change   X confirm   O exit", RGB(150, 150, 150));
}

// LiveArea links arrive as "psgm:play?titleid=...&param=settings".
int port_settings_requested(void) {
  char param[1024];
  memset(param, 0, sizeof(param));
  if (sceAppMgrGetAppParam(param) >= 0 && strstr(param, "param=settings")) {
    L_INFO("port: settings requested from the LiveArea");
    return 1;
  }
  SceCtrlData pad;
  memset(&pad, 0, sizeof(pad));
  if (sceCtrlPeekBufferPositive(0, &pad, 1) > 0 && (pad.buttons & SCE_CTRL_SELECT)) {
    L_INFO("port: settings requested by holding SELECT");
    return 1;
  }
  return 0;
}

int port_settings_screen(void) {
  const unsigned size = (FB_W * FB_H * 4 + 0x3FFFF) & ~0x3FFFF;
  SceUID block = sceKernelAllocMemBlock("port_settings_fb", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, size, NULL);
  void *base = NULL;
  if (block < 0 || sceKernelGetMemBlockBase(block, &base) < 0) {
    L_WARN("port: no display memory for the settings screen (0x%08X)", block);
    return 1;
  }
  SceDisplayFrameBuf fb;
  memset(&fb, 0, sizeof(fb));
  fb.size = sizeof(fb);
  fb.base = base;
  fb.pitch = FB_W;
  fb.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
  fb.width = FB_W;
  fb.height = FB_H;

  int selected = 0, result = -1;
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
    if (!act) continue;
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

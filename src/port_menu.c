// Port settings menu, opened with L + R + START.
//
// The menu is drawn by the CPU straight into each display buffer from
// vitaGL's display callback, after the GPU has finished the frame. Unity's GL
// state is never touched, so its state cache stays valid. While the menu is
// open the game keeps running but receives no input (in a race, the L/START
// part of the combo has already sent BACK, which pauses the game).
//
// Settings are stored in PORT_SETTINGS_PATH as key=value lines.

#include "port_menu.h"
#include "config.h"
#include "bridges/bridges.h"
#include "util/log.h"
#include "util/port_font.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <vitaGL.h>

// vitaGL globals describing the buffer being handed to the display.
extern int DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_STRIDE;
extern volatile int unity_resize_requested;
void input_release_all(void);
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

// --- Applying settings ------------------------------------------------------

static volatile int overlay_skip;  // Display frames to leave untouched after a resize.
static int resize_next_frame;
static int character_original_at_boot;

static void apply_resolution(void) {
  int w = resolutions[cfg.resolution].w, h = resolutions[cfg.resolution].h;
  if (w == port_screen_w && h == port_screen_h && DISPLAY_WIDTH == w) return;
  if (!vglSwapResolution(w, h)) {
    L_WARN("port: display refused %dx%d", w, h);
    return;
  }
  // vitaGL switches buffers at the end of the next swap; Unity is told on the
  // frame after that, so it never renders into a buffer of the other size.
  overlay_skip = 4;
  port_screen_w = w;
  port_screen_h = h;
  resize_next_frame = 1;
  L_INFO("port: resolution -> %dx%d", w, h);
}

static void apply_fps(void) {
  // A swap interval of 2 paces 30 fps on the 60 Hz display evenly.
  gl_set_swap_interval(cfg.fps == 30 ? 2 : 1);
  mono_port_frame_rate_apply();
}

// --- Menu model --------------------------------------------------------------

enum { ROW_RESOLUTION, ROW_GRAPHICS, ROW_FPS, ROW_EFFECTS, ROW_FPS_COUNTER, ROW_CHARACTER, ROW_CLOSE, ROW_COUNT };

static int menu_open, menu_closing, selected;
static int light_mode = -1;  // Game's Light Mode, read when the menu opens.
static uint32_t prev_buttons;
static uint64_t repeat_at;

static const char *const quality_names[] = {"Low", "Medium", "High"};

static void row_value(int row, char *out, size_t size) {
  switch (row) {
  case ROW_RESOLUTION: snprintf(out, size, "%s", resolutions[cfg.resolution].name); break;
  case ROW_GRAPHICS: snprintf(out, size, "%s", quality_names[cfg.quality]); break;
  case ROW_FPS: snprintf(out, size, "%d FPS", cfg.fps); break;
  case ROW_EFFECTS: {
    snprintf(out, size, "%s", light_mode < 0 ? "Not loaded yet" : light_mode ? "Light" : "Full");
    break;
  }
  case ROW_FPS_COUNTER: snprintf(out, size, "%s", cfg.fps_counter ? "On" : "Off"); break;
  case ROW_CHARACTER: snprintf(out, size, "%s", cfg.character_original ? "Original" : "Compatible"); break;
  default: out[0] = 0;
  }
}

static const char *const row_names[ROW_COUNT] = {"Resolution", "Graphics", "Frame Rate", "Effects",
                                                 "FPS Counter", "Character Shader", "Close"};

static const char *row_hint(int row) {
  switch (row) {
  case ROW_RESOLUTION:
    return cfg.resolution == PORT_RES_QHD ? "Native Vita resolution."
                                          : "Rendered smaller, upscaled. Faster GPU.";
  case ROW_GRAPHICS:
    return cfg.quality == PORT_QUALITY_LOW ? "Game's Light quality. Quarter-size textures."
           : cfg.quality == PORT_QUALITY_HIGH ? "Unity 'Simple' level: sharper filtering."
                                              : "Game's Normal quality. Half-size textures.";
  case ROW_FPS: return cfg.fps == 30 ? "Locked to 30 FPS with even frame pacing." : "Targets 60 FPS.";
  case ROW_EFFECTS:
    return light_mode < 0 ? "Available once the title screen loads."
                                      : "Game's Light Mode: fewer effects. Next run.";
  case ROW_FPS_COUNTER: return "Shows frames per second in the corner.";
  case ROW_CHARACTER:
    return cfg.character_original != character_original_at_boot ? "Restart the game to apply."
                                                                  : "Original uses the game's env-map shader.";
  default: return "";
  }
}

static void change(int row, int dir) {
  switch (row) {
  case ROW_RESOLUTION:
    cfg.resolution = (cfg.resolution + dir + PORT_RES_COUNT) % PORT_RES_COUNT;
    apply_resolution();
    break;
  case ROW_GRAPHICS:
    cfg.quality = (cfg.quality + dir + PORT_QUALITY_COUNT) % PORT_QUALITY_COUNT;
    mono_port_quality_apply();
    break;
  case ROW_FPS:
    cfg.fps = cfg.fps == 60 ? 30 : 60;
    apply_fps();
    break;
  case ROW_EFFECTS: {
    if (light_mode >= 0 && mono_port_set_light_mode(!light_mode)) light_mode = mono_port_light_mode();
    return;  // Stored in the game's own save data.
  }
  case ROW_FPS_COUNTER: cfg.fps_counter = !cfg.fps_counter; break;
  case ROW_CHARACTER: cfg.character_original = !cfg.character_original; break;
  default: return;
  }
  settings_save();
}

// --- Overlay text (built on the Unity thread, drawn on the display thread) ---

#define TEXT_ROWS 16
#define TEXT_COLS 48
typedef struct {
  int rows;
  char text[TEXT_ROWS][TEXT_COLS];
  uint32_t color[TEXT_ROWS];
  int highlight;  // Row index drawn with a selection bar, -1 for none.
} overlay_text;

static overlay_text overlays[2];
static volatile int overlay_front = -1;  // -1: menu hidden.

#define RGB(r, g, b) (0xFF000000u | ((uint32_t)(b) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(r))
#define COLOR_TEXT RGB(235, 235, 235)
#define COLOR_TITLE RGB(255, 205, 40)
#define COLOR_HINT RGB(150, 190, 255)
#define COLOR_DIM RGB(150, 150, 150)

static void build_overlay(void) {
  int back = overlay_front == 0 ? 1 : 0;
  overlay_text *o = &overlays[back];
  memset(o, 0, sizeof(*o));
  int r = 0;
  snprintf(o->text[r], TEXT_COLS, "PORT SETTINGS");
  o->color[r++] = COLOR_TITLE;
  r++;
  o->highlight = -1;
  for (int i = 0; i < ROW_COUNT; i++) {
    char value[32];
    row_value(i, value, sizeof(value));
    if (i == ROW_CLOSE) r++;
    if (i == selected) o->highlight = r;
    if (value[0])
      snprintf(o->text[r], TEXT_COLS, "%-18s %c %-15s %c", row_names[i], i == selected ? '<' : ' ', value,
               i == selected ? '>' : ' ');
    else
      snprintf(o->text[r], TEXT_COLS, "%s", row_names[i]);
    o->color[r++] = COLOR_TEXT;
  }
  r++;
  snprintf(o->text[r], TEXT_COLS, "%.44s", row_hint(selected));
  o->color[r++] = COLOR_HINT;
  snprintf(o->text[r], TEXT_COLS, "UP/DOWN select  LEFT/RIGHT change  O close");
  o->color[r++] = COLOR_DIM;
  o->rows = r;
  __atomic_store_n(&overlay_front, back, __ATOMIC_RELEASE);
}

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

static unsigned shown_fps;

static void display_callback(void *framebuf) {
  // FPS: one callback per presented frame.
  static unsigned frames;
  static uint64_t window_start;
  uint64_t now = sceKernelGetProcessTimeWide();
  frames++;
  if (now - window_start >= 1000000) {
    shown_fps = window_start ? (unsigned)((frames * 1000000ull + (now - window_start) / 2) / (now - window_start)) : 0;
    frames = 0;
    window_start = now;
  }

  if (overlay_skip > 0) {
    overlay_skip--;
    return;
  }
  int w = DISPLAY_WIDTH, h = DISPLAY_HEIGHT, stride = DISPLAY_STRIDE;
  if (w != port_screen_w || h != port_screen_h) return;
  uint32_t *fb = framebuf;
  int g = w * 16 / 960;  // 16 px at 960, 12 at 720, 10 at 640
  if (g < 8) g = 8;

  if (cfg.fps_counter) {
    char text[16];
    snprintf(text, sizeof(text), "%u FPS", shown_fps);
    int tw = (int)strlen(text) * g;
    draw_rect(fb, stride, 0, 0, tw + g, g + g / 2, RGB(0, 0, 0));
    draw_text(fb, stride, g / 2, g / 4, g, text, shown_fps >= 55 ? RGB(80, 230, 80) : shown_fps >= 28 ? RGB(255, 205, 40) : RGB(255, 80, 80));
  }

  int front = __atomic_load_n(&overlay_front, __ATOMIC_ACQUIRE);
  if (front < 0) return;
  const overlay_text *o = &overlays[front];
  int line = g + g / 2;
  int pw = (TEXT_COLS - 2) * g, ph = (o->rows + 2) * line;
  if (pw > w - 8) pw = w - 8;
  int px = (w - pw) / 2, py = (h - ph) / 2;
  int border = g / 6 > 1 ? g / 6 : 1;
  draw_rect(fb, stride, px - border, py - border, pw + 2 * border, ph + 2 * border, RGB(255, 205, 40));
  draw_rect(fb, stride, px, py, pw, ph, RGB(16, 24, 48));
  for (int r = 0; r < o->rows; r++) {
    int y = py + line + r * line;
    if (r == o->highlight) draw_rect(fb, stride, px + g / 2, y - g / 6, pw - g, line, RGB(40, 80, 170));
    draw_text(fb, stride, px + g, y, g, o->text[r], o->color[r]);
  }
}

// --- Input -------------------------------------------------------------------

#define COMBO (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_START)

static void open_menu(void) {
  menu_open = 1;
  menu_closing = 0;
  selected = 0;
  light_mode = mono_port_light_mode();
  input_release_all();
  build_overlay();
  L_INFO("port: settings menu opened");
}

static void close_menu(void) {
  menu_open = 0;
  menu_closing = 1;  // Keep the controls until every button is released.
  __atomic_store_n(&overlay_front, -1, __ATOMIC_RELEASE);
  L_INFO("port: settings menu closed");
}

int port_menu_tick(void) {
  if (resize_next_frame) {
    resize_next_frame = 0;
    unity_resize_requested = 1;
  }
  SceCtrlData pad = {0};
  if (sceCtrlPeekBufferPositive(0, &pad, 1) <= 0) pad.buttons = prev_buttons;
  uint32_t buttons = pad.buttons, pressed = buttons & ~prev_buttons;
  int combo_now = (buttons & COMBO) == COMBO, combo_before = (prev_buttons & COMBO) == COMBO;
  prev_buttons = buttons;

  if (menu_closing) {
    if (buttons) return 1;
    menu_closing = 0;
    return 0;
  }
  if (!menu_open) {
    if (combo_now && !combo_before && !ime_running()) {
      open_menu();
      return 1;
    }
    return 0;
  }

  static unsigned light_retry;
  if (light_mode < 0 && ++light_retry % 30 == 0) light_mode = mono_port_light_mode();
  if ((combo_now && !combo_before) || (pressed & SCE_CTRL_CIRCLE)) {
    close_menu();
    return 1;
  }
  // D-pad with auto-repeat while held.
  uint64_t now = sceKernelGetProcessTimeWide();
  uint32_t dirs = SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_LEFT | SCE_CTRL_RIGHT;
  uint32_t act = pressed & (dirs | SCE_CTRL_CROSS);
  if (pressed & dirs)
    repeat_at = now + 400000;
  else if ((buttons & (SCE_CTRL_UP | SCE_CTRL_DOWN)) && now >= repeat_at) {
    act |= buttons & (SCE_CTRL_UP | SCE_CTRL_DOWN);
    repeat_at = now + 120000;
  }
  if (act & SCE_CTRL_UP) selected = (selected + ROW_COUNT - 1) % ROW_COUNT;
  if (act & SCE_CTRL_DOWN) selected = (selected + 1) % ROW_COUNT;
  if (act & SCE_CTRL_LEFT) change(selected, -1);
  if (act & SCE_CTRL_RIGHT) change(selected, +1);
  if (act & SCE_CTRL_CROSS) {
    if (selected == ROW_CLOSE) {
      close_menu();
      return 1;
    }
    change(selected, +1);
  }
  build_overlay();  // Also refreshes values the game changes (Effects).
  return 1;
}

void port_menu_init(void) {
  character_original_at_boot = cfg.character_original;
  vglSetDisplayCallback(display_callback);
  // vitaGL was created at the full 960x544 so its depth buffer fits every size.
  port_screen_w = SCREEN_MAX_W;
  port_screen_h = SCREEN_MAX_H;
  apply_resolution();
  resize_next_frame = 0;  // Unity is created at the new size; no resize needed.
  gl_set_swap_interval(cfg.fps == 30 ? 2 : 1);
  L_INFO("port: settings menu ready (L + R + START)");
}

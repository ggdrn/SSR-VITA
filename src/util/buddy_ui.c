// Skip only the proven side-effect-free return path of the supported Chao cell
// Update. No JIT code is modified and active/loading cells still enter Mono.
#include "buddy_ui.h"
#include "buddy_cell_il.h"
#include "managed_trace.h"
#include "../loader/so_util.h"
#include "../config.h"
#include "log.h"
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>

static void *cell_class;
static void *(*object_class)(void *);
static uint32_t cell_offsets[4];
static int attempted;

static int idle_cell(void *self) {
  if (!self || !object_class || object_class(self) != cell_class) return 0;
  const uint8_t *cell = self;
  if (!cell[cell_offsets[0]] || !cell[cell_offsets[1]]) return 0;
  float draw, check;
  memcpy(&draw, cell + cell_offsets[2], sizeof(draw));
  memcpy(&check, cell + cell_offsets[3], sizeof(check));
  // IL uses ble.un for drawDelay and ordered bgt for checkDelay: NaNs
  // also take the no-op path. Never skip a positive pending timer.
  return !(draw > 0.0f) && !(check > 0.0f);
}

void buddy_ui_install(void *image) {
  if (attempted || !image) return;
  attempted = 1;
  FILE *marker = fopen(DATA_PATH "/original_buddy_ui", "rb");
  if (marker) {
    fclose(marker);
    L_INFO("ui: original buddy callbacks and full menu timing requested");
    return;
  }
  so_module *mono = so_find_module("libmono.so");
  if (!mono) return;
#define API(type, name) ((type)so_symbol(mono, name))
  void *(*klass)(void *, const char *, const char *) = API(void *(*)(void *, const char *, const char *), "mono_class_from_name");
  void *(*method)(void *, const char *, int) = API(void *(*)(void *, const char *, int), "mono_class_get_method_from_name");
  void *(*header)(void *) = API(void *(*)(void *), "mono_method_get_header");
  const uint8_t *(*code)(void *, uint32_t *, uint32_t *) = API(const uint8_t *(*)(void *, uint32_t *, uint32_t *), "mono_method_header_get_code");
  void (*free_header)(void *) = API(void (*)(void *), "mono_metadata_free_mh");
  const char *(*guid)(void *) = API(const char *(*)(void *), "mono_image_get_guid");
  void *(*field)(void *, const char *) = API(void *(*)(void *, const char *), "mono_class_get_field_from_name");
  uint32_t (*offset)(void *) = API(uint32_t (*)(void *), "mono_field_get_offset");
  void *(*field_type)(void *) = API(void *(*)(void *), "mono_field_get_type");
  int (*type_kind)(void *) = API(int (*)(void *), "mono_type_get_type");
  void *(*method_token)(void *, uint32_t, void *) = API(void *(*)(void *, uint32_t, void *), "mono_get_method");
  int (*instance_size)(void *) = API(int (*)(void *), "mono_class_instance_size");
  object_class = API(void *(*)(void *), "mono_object_get_class");
  if (!klass || !method || !header || !code || !free_header || !guid || !field || !offset ||
      !field_type || !type_kind || !instance_size || !object_class) {
    L_WARN("ui: buddy idle path unavailable; original callbacks preserved");
    return;
  }
  const char *id = guid(image);
  void *cell = klass(image, "", "ui_chao_set_cell");
  void *update = cell ? method(cell, "Update", 0) : NULL;
  void *mh = update ? header(update) : NULL;
  uint32_t size = 0, stack = 0;
  const uint8_t *il = mh ? code(mh, &size, &stack) : NULL;
  int valid = id && !strcasecmp(id, BUDDY_CELL_MVID) && il && size == sizeof(buddy_cell_il) &&
              !memcmp(il, buddy_cell_il, size);
  if (mh) free_header(mh);
  if (!valid) {
    L_WARN("ui: buddy Update IL/MVID differs; original callbacks preserved");
    return;
  }
  const char *names[4] = {"m_isLoadCmp", "m_isDrawChao", "m_drawDelay", "m_checkDelay"};
  const int kinds[4] = {2, 2, 12, 12}; // MONO_TYPE_BOOLEAN / MONO_TYPE_R4
  int bytes = instance_size(cell);
  for (unsigned i = 0; i < 4; i++) {
    void *f = field(cell, names[i]);
    void *ft = f ? field_type(f) : NULL;
    uint32_t at = f ? offset(f) : 0;
    unsigned width = i < 2 ? 1 : 4;
    if (!ft || type_kind(ft) != kinds[i] || bytes < 8 || at < 8 || at > (uint32_t)bytes - width) {
      L_WARN("ui: buddy field layout differs; original callbacks preserved");
      return;
    }
    cell_offsets[i] = at;
  }
  cell_class = cell;
  managed_trace_idle_method(update, idle_cell);
  void *panel = klass(image, "", "UIPanel");
  void *camera = klass(image, "", "UICamera");
  // Keep the original slow initialization coroutine in every sample too.
  void *panel_update = panel ? method(panel, "LateUpdate", 0) : NULL;
  void *camera_update = camera ? method(camera, "Update", 0) : NULL;
  void *init_update = method_token ? method_token(image, 0x060048a0, NULL) : NULL;
  L_INFO("ui: buddy settled cells use checked idle return; pending work stays in Mono");
  if (panel_update && camera_update && init_update) {
    managed_trace_menu_methods(panel_update, update, camera_update, init_update);
    L_INFO("perf: menu callback timing focused on panel/cell/input/init; full sample every 32 frames");
  } else {
    L_WARN("perf: menu timing methods unavailable; full callback coverage preserved");
  }
#undef API
}

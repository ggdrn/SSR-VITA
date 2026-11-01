// Read-time layout correction for the game's own Splash3 UITextures.
// Both objects are compared in full before enabling any overlay. The APK
// remains unchanged; arbitrary partial reads receive the same patched bytes.
#include "title_layout.h"
#include "title_layout_data.h"
#include "log.h"
#include <string.h>
#include <stdlib.h>
static const apk_entry *title_entry;
static int title_enabled;
void title_layout_prepare(void) {
  title_entry = apk_find("assets/bin/Data/mainData");
  title_enabled = 0;
  if (!title_entry || title_entry->size != TITLE_MAIN_DATA_SIZE) return;
  size_t length = 0;
  uint8_t *data = apk_extract(title_entry, &length);
  if (!data || length != TITLE_MAIN_DATA_SIZE) { free(data); return; }
  for (unsigned i = 0; i < sizeof(title_layout_objects) / sizeof(*title_layout_objects); i++) {
    if ((uint64_t)title_layout_objects[i].offset + title_layout_objects[i].size > length ||
        memcmp(data + title_layout_objects[i].offset, title_layout_objects[i].original, title_layout_objects[i].size)) {
      L_WARN("splash: original title layout differs; kept APK layout");
      free(data);
      return;
    }
  }
  free(data);
  title_enabled = 1;
  L_INFO("splash: fan-art splash fitted at 16:9 in the original title UI (APK unchanged)");
}
void title_layout_apply(const apk_entry *entry, uint32_t pos, void *data, uint32_t size) {
  if (!title_enabled || entry != title_entry) return;
  uint64_t end = (uint64_t)pos + size;
  for (unsigned i = 0; i < sizeof(title_layout_objects) / sizeof(*title_layout_objects); i++) {
    uint32_t at = title_layout_objects[i].offset, limit = at + title_layout_objects[i].size;
    if (pos >= limit || end <= at) continue;
    uint32_t first = pos > at ? pos : at, last = end < limit ? (uint32_t)end : limit;
    memcpy((uint8_t *)data + first - pos, title_layout_objects[i].replacement + first - at, last - first);
  }
}

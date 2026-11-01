// Replace only Splash3's existing RGB24 mip bytes; preserve serialized sizes.
#include "splash_art.h"
#include "splash_art_data.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef GAME_SPLASH_PATH
#define GAME_SPLASH_PATH "app0:assets/game-splash.rgb"
#endif
#define PART_COUNT (sizeof(game_splash_parts) / sizeof(*game_splash_parts))
static const apk_entry *parts[PART_COUNT];
static uint8_t *payload;

void splash_art_prepare(void) {
  free(payload); payload = NULL;
  for (unsigned i = 0; i < PART_COUNT; i++) {
    parts[i] = apk_find(game_splash_parts[i].name);
    if (!parts[i] || parts[i]->size != game_splash_parts[i].size) return;
  }
  size_t length = 0;
  uint8_t *original = apk_extract(parts[0], &length);
  int valid = original && length >= GAME_SPLASH_HEADER_AT + sizeof(game_splash_header) &&
    !memcmp(original + GAME_SPLASH_HEADER_AT, game_splash_header, sizeof(game_splash_header));
  free(original);
  if (!valid) { L_WARN("splash: texture header differs; kept APK art"); return; }
  FILE *file = fopen(GAME_SPLASH_PATH, "rb");
  if (!file) { L_WARN("splash: packaged fan art missing; kept APK art"); return; }
  uint8_t *image = malloc(GAME_SPLASH_SIZE);
  valid = image && fread(image, 1, GAME_SPLASH_SIZE, file) == GAME_SPLASH_SIZE && fgetc(file) == EOF;
  fclose(file);
  uint32_t hash = 2166136261u;
  if (valid) for (uint32_t i = 0; i < GAME_SPLASH_SIZE; i++) hash = (hash ^ image[i]) * 16777619u;
  if (!valid || hash != GAME_SPLASH_HASH) {
    free(image); L_WARN("splash: packaged fan art invalid; kept APK art"); return;
  }
  payload = image;
  L_INFO("splash: fan art RGB24 overlay ready (%u bytes; original APK unchanged)", GAME_SPLASH_SIZE);
}

void splash_art_apply(const apk_entry *entry, uint32_t pos, void *data, uint32_t size) {
  if (!payload) return;
  for (unsigned i = 0; i < PART_COUNT; i++) {
    if (entry != parts[i]) continue;
    uint64_t begin = (uint64_t)game_splash_parts[i].base + pos, end = begin + size;
    uint64_t first = begin > GAME_SPLASH_START ? begin : GAME_SPLASH_START;
    uint64_t last = end < GAME_SPLASH_START + GAME_SPLASH_SIZE ? end : GAME_SPLASH_START + GAME_SPLASH_SIZE;
    if (first < last) memcpy((uint8_t *)data + first - begin, payload + first - GAME_SPLASH_START, last - first);
    return;
  }
}

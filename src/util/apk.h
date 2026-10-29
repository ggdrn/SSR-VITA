#ifndef SRR_APK_H
#define SRR_APK_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
  char *name;
  uint32_t data_offset_cache; // lazy internal cache; zero until header validated
  uint32_t local_offset;  // offset of the local file header
  uint32_t comp_size;
  uint32_t size;
  uint16_t method;        // 0 = stored, 8 = deflate
} apk_entry;

// Opens the APK and indexes its central directory.
int apk_open(const char *path);
void apk_close(void);
const char *apk_path(void);

const apk_entry *apk_find(const char *name);
int apk_entry_count(void);
const apk_entry *apk_entry_at(int idx);

// File offset of the entry's raw data (after the local header).
int64_t apk_data_offset(const apk_entry *e);

// Extracts an entry into a freshly malloc'd buffer (caller frees).
void *apk_extract(const apk_entry *e, size_t *out_size);

// Reads `len` bytes of a *stored* entry starting at `pos` (no allocation).
int apk_read_stored(const apk_entry *e, uint32_t pos, void *dst, uint32_t len);

#endif

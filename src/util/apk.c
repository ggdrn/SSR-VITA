// Minimal ZIP reader for the APK: central directory index + stored/deflate
// extraction through zlib. Kept independent from newlib FILE so it can be used
// before any bridge is set up.

#include "apk.h"
#include "log.h"
#include "work_trace.h"
#include "observe.h"
#include "title_layout.h"
#include "splash_art.h"

#include <psp2/io/fcntl.h>
#include <psp2/kernel/threadmgr.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

static SceUID apk_fd = -1;
static char apk_file[256];
static apk_entry *entries;
static int num_entries;
static SceKernelLwMutexWork apk_lock;

static uint16_t rd16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

static int pread_all(void *dst, uint32_t len, int64_t off) {
  uint64_t start = work_trace_begin();
  sceKernelLockLwMutex(&apk_lock, 1, NULL);
  int r = sceIoPread(apk_fd, dst, len, off);
  sceKernelUnlockLwMutex(&apk_lock, 1);
  work_trace_end(WORK_IO, start, r > 0 ? r : 0, (uintptr_t)__builtin_return_address(0));
  return r == (int)len ? 0 : -1;
}

static int entry_cmp(const void *a, const void *b) {
  return strcmp(((const apk_entry *)a)->name, ((const apk_entry *)b)->name);
}

int apk_open(const char *path) {
  sceKernelCreateLwMutex(&apk_lock, "apk", 2, 0, NULL);
  apk_fd = sceIoOpen(path, SCE_O_RDONLY, 0);
  if (apk_fd < 0)
    return -1;
  strncpy(apk_file, path, sizeof(apk_file) - 1);

  int64_t size = sceIoLseek(apk_fd, 0, SCE_SEEK_END);
  // End of central directory record is in the last 64KB + 22 bytes.
  uint32_t tail = size < 0x10000 + 22 ? (uint32_t)size : 0x10000 + 22;
  uint8_t *buf = malloc(tail);
  if (pread_all(buf, tail, size - tail) < 0) {
    free(buf);
    return -2;
  }
  int eocd = -1;
  for (int i = tail - 22; i >= 0; i--) {
    if (rd32(buf + i) == 0x06054b50) {
      eocd = i;
      break;
    }
  }
  if (eocd < 0) {
    free(buf);
    return -3;
  }
  num_entries = rd16(buf + eocd + 10);
  uint32_t cd_size = rd32(buf + eocd + 12);
  uint32_t cd_off = rd32(buf + eocd + 16);
  free(buf);

  uint8_t *cd = malloc(cd_size);
  if (pread_all(cd, cd_size, cd_off) < 0) {
    free(cd);
    return -4;
  }
  entries = calloc(num_entries, sizeof(apk_entry));
  uint8_t *p = cd;
  for (int i = 0; i < num_entries; i++) {
    if (rd32(p) != 0x02014b50) {
      num_entries = i;
      break;
    }
    uint16_t name_len = rd16(p + 28), extra_len = rd16(p + 30), comment_len = rd16(p + 32);
    entries[i].method = rd16(p + 10);
    entries[i].comp_size = rd32(p + 20);
    entries[i].size = rd32(p + 24);
    entries[i].local_offset = rd32(p + 42);
    entries[i].name = malloc(name_len + 1);
    memcpy(entries[i].name, p + 46, name_len);
    entries[i].name[name_len] = 0;
    p += 46 + name_len + extra_len + comment_len;
  }
  free(cd);
  qsort(entries, num_entries, sizeof(apk_entry), entry_cmp);
  L_INFO("apk: %s indexed, %d entries", path, num_entries);
  title_layout_prepare();
  splash_art_prepare();
  return 0;
}

void apk_close(void) {
  if (apk_fd >= 0)
    sceIoClose(apk_fd);
  apk_fd = -1;
}

const char *apk_path(void) { return apk_file; }

const apk_entry *apk_find(const char *name) {
  apk_entry key = {.name = (char *)name};
  return bsearch(&key, entries, num_entries, sizeof(apk_entry), entry_cmp);
}

int apk_entry_count(void) { return num_entries; }
const apk_entry *apk_entry_at(int idx) { return idx >= 0 && idx < num_entries ? &entries[idx] : NULL; }

int64_t apk_data_offset(const apk_entry *e) {
  // Entries returned by this module live in writable, calloc'd storage.
  // Metadata stays constant; only this private memoized offset is published.
  uint32_t *cache = (uint32_t *)&e->data_offset_cache;
  uint32_t cached = __atomic_load_n(cache, __ATOMIC_ACQUIRE);
  if (cached) return cached;
  uint8_t hdr[30];
  if (pread_all(hdr, 30, e->local_offset) < 0 || rd32(hdr) != 0x04034b50)
    return -1;
  uint64_t offset = (uint64_t)e->local_offset + 30 + rd16(hdr + 26) + rd16(hdr + 28);
  if (offset > UINT32_MAX) return -1;
  __atomic_store_n(cache, (uint32_t)offset, __ATOMIC_RELEASE);
  return offset;
}

void *apk_extract(const apk_entry *e, size_t *out_size) {
  uint64_t observed_start = observe_now();
  int64_t off = apk_data_offset(e);
  if (off < 0)
    return NULL;
  uint8_t *out = malloc(e->size ? e->size : 1);
  if (!out)
    return NULL;

  if (e->method == 0) {
    if (pread_all(out, e->size, off) < 0)
      goto fail;
  } else if (e->method == 8) {
    // Inflate in chunks to avoid holding the compressed copy too.
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    if (inflateInit2(&zs, -MAX_WBITS) != Z_OK)
      goto fail;
    const uint32_t CHUNK = 256 * 1024;
    uint8_t *in = malloc(CHUNK);
    uint32_t done = 0;
    zs.next_out = out;
    zs.avail_out = e->size;
    int r = Z_OK;
    while (r != Z_STREAM_END && done < e->comp_size) {
      uint32_t n = e->comp_size - done < CHUNK ? e->comp_size - done : CHUNK;
      if (pread_all(in, n, off + done) < 0)
        break;
      done += n;
      zs.next_in = in;
      zs.avail_in = n;
      r = inflate(&zs, Z_NO_FLUSH);
      if (r != Z_OK && r != Z_STREAM_END)
        break;
    }
    free(in);
    inflateEnd(&zs);
    if (r != Z_STREAM_END)
      goto fail;
  } else {
    L_ERROR("apk: %s uses unsupported method %d", e->name, e->method);
    goto fail;
  }
  if (out_size)
    *out_size = e->size;
  title_layout_apply(e, 0, out, e->size);
  splash_art_apply(e, 0, out, e->size);
  observe_span(OBS_APK, e->name, observed_start, e->size, 0, 0);
  return out;

fail:
  observe_span(OBS_APK, e->name, observed_start, 0, -1, 0);
  free(out);
  return NULL;
}

int apk_read_stored(const apk_entry *e, uint32_t pos, void *dst, uint32_t len) {
  uint64_t observed_start = observe_now();
  if (e->method != 0 || pos > e->size)
    return -1;
  if (len > e->size - pos)
    len = e->size - pos;
  int64_t off = apk_data_offset(e);
  if (off < 0 || pread_all(dst, len, off + pos) < 0)
    return -1;
  title_layout_apply(e, pos, dst, len);
  splash_art_apply(e, pos, dst, len);
  observe_span(OBS_APK, e->name, observed_start, len, 0, 0);
  return len;
}

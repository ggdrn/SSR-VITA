#include "bundle_mips.h"
#include "log.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct { uint32_t start, end; } mip_range;
typedef struct {
  uint32_t hash, size;
  int64_t mtime;
  mip_range *ranges; // immutable once published; never freed
  unsigned count;
} parsed_file;
typedef struct { FILE *f; const parsed_file *file; } open_stream;

#define CACHE_SLOTS 512
#define STREAM_SLOTS 32
#define MIN_SKIP (64 * 1024)
static parsed_file cache[CACHE_SLOTS];
static unsigned cache_used;
static open_stream streams[STREAM_SLOTS];
static int texture_limit;
static uint64_t skipped_bytes, reads_split;
static unsigned files_with_ranges;
static int lock;

static void acquire(void) { while (__atomic_exchange_n(&lock, 1, __ATOMIC_ACQUIRE)) {} }
static void release(void) { __atomic_store_n(&lock, 0, __ATOMIC_RELEASE); }

void bundle_mips_set_limit(int limit) { __atomic_store_n(&texture_limit, limit, __ATOMIC_RELEASE); }

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

// Bytes per pixel of the uncompressed Unity 4 formats seen in these bundles:
// Alpha8, ARGB4444, RGB24, RGBA32, ARGB32, RGB565, RGBA4444, BGRA32.
static unsigned format_bpp(uint32_t format) {
  switch (format) {
  case 1: return 1;
  case 2: case 7: case 13: return 2;
  case 3: return 3;
  case 4: case 5: case 14: return 4;
  default: return 0;
  }
}

static int read_at(FILE *f, uint32_t offset, void *out, size_t size) {
  return fseek(f, offset, SEEK_SET) == 0 && fread(out, 1, size, f) == size;
}

// Unity 4.7 serialized file, format 9, without type trees (as in these
// bundles). Texture2D (class 28) layout: name, width, height, complete image
// size, format, mipmap/readable/readAllowed bools (aligned), image count,
// dimension, 4 texture settings, lightmap format, color space, image data.
static unsigned parse_ranges(FILE *f, uint32_t file_size, mip_range **out) {
  uint8_t header[20];
  if (!read_at(f, 0, header, sizeof(header))) return 0;
  uint32_t meta_size = be32(header), declared = be32(header + 4), version = be32(header + 8), data = be32(header + 12);
  if (version != 9 || declared != file_size || header[16] != 0 || meta_size < 64 || meta_size > 1024 * 1024 ||
      20 + meta_size > data || data >= file_size)
    return 0;
  uint8_t *meta = malloc(meta_size);
  if (!meta || !read_at(f, 20, meta, meta_size)) { free(meta); return 0; }
  const uint8_t *p = memchr(meta, 0, meta_size < 32 ? meta_size : 32), *end = meta + meta_size;
  unsigned found = 0, capacity = 0;
  mip_range *ranges = NULL;
  if (!p || end - ++p < 16 || le32(p + 4) != 0 || le32(p + 8) != 0) goto done; // types, big IDs
  uint32_t objects = le32(p + 12);
  p += 16;
  if (objects > (uint32_t)(end - p) / 20) goto done;
  for (uint32_t i = 0; i < objects; i++, p += 20) {
    uint32_t start = le32(p + 4), size = le32(p + 8);
    uint16_t klass = p[16] | p[17] << 8;
    if (klass != 28 || size < MIN_SKIP || start > file_size - data || size > file_size - data - start) continue;
    uint8_t t[160];
    uint32_t object = data + start;
    if (!read_at(f, object, t, sizeof(t))) continue;
    uint32_t name = le32(t), at = (4 + name + 3) & ~3u;
    if (name > 96) continue;
    uint32_t w = le32(t + at), h = le32(t + at + 4), complete = le32(t + at + 8), format = le32(t + at + 12);
    uint8_t mipmap = t[at + 16], readable = t[at + 17];
    uint32_t images = le32(t + at + 20), dimension = le32(t + at + 24), image = le32(t + at + 52);
    unsigned bpp = format_bpp(format);
    if (!bpp || mipmap != 1 || readable || images != 1 || dimension != 2 || w < 2 || h < 2 || w > 4096 || h > 4096)
      continue;
    uint64_t chain = 0;
    for (uint32_t mw = w, mh = h;; mw = mw > 1 ? mw / 2 : 1, mh = mh > 1 ? mh / 2 : 1) {
      chain += (uint64_t)mw * mh * bpp;
      if (mw == 1 && mh == 1) break;
    }
    uint32_t body = at + 56, top = w * h * bpp;
    if (image != complete || image != chain || top < MIN_SKIP || (uint64_t)body + image > size ||
        size - body - image >= 4)
      continue;
    if (found == capacity) {
      mip_range *grown = realloc(ranges, (capacity = capacity ? capacity * 2 : 16) * sizeof(*ranges));
      if (!grown) break;
      ranges = grown;
    }
    ranges[found++] = (mip_range){object + body, object + body + top};
  }
done:
  free(meta);
  if (!found) { free(ranges); return 0; }
  *out = ranges;
  return found;
}

static uint32_t path_hash(const char *s) {
  uint32_t h = 2166136261u;
  while (*s) h = (h ^ (uint8_t)*s++) * 16777619u;
  return h;
}

void bundle_mips_open(FILE *f, const char *path) {
  struct stat st;
  if (!f || fstat(fileno(f), &st) != 0 || st.st_size <= 0 || st.st_size > 0x7fffffff) return;
  uint32_t hash = path_hash(path), size = (uint32_t)st.st_size;
  const parsed_file *file = NULL;
  acquire();
  for (unsigned i = 0; i < cache_used; i++)
    if (cache[i].hash == hash && cache[i].size == size && cache[i].mtime == (int64_t)st.st_mtime) file = &cache[i];
  release();
  if (!file) {
    mip_range *ranges = NULL;
    unsigned count = parse_ranges(f, size, &ranges);
    fseek(f, 0, SEEK_SET);
    uint64_t bytes = 0;
    for (unsigned i = 0; i < count; i++) bytes += ranges[i].end - ranges[i].start;
    acquire();
    if (cache_used < CACHE_SLOTS) {
      cache[cache_used] = (parsed_file){hash, size, (int64_t)st.st_mtime, ranges, count};
      file = &cache[cache_used++];
      ranges = NULL;
    }
    unsigned n = count ? ++files_with_ranges : 0;
    release();
    free(ranges);
    const char *base = strrchr(path, '/');
    if (n && (n <= 8 || (n & (n - 1)) == 0))
      L_INFO("io: %s top mips %u, %u of %u KiB not read while masterTextureLimit >= 1 (files %u)",
             base ? base + 1 : path, count, (unsigned)(bytes / 1024), size / 1024, n);
  }
  if (!file || !file->count) return;
  acquire();
  for (unsigned i = 0; i < STREAM_SLOTS; i++)
    if (!streams[i].f) { streams[i] = (open_stream){f, file}; break; }
  release();
}

void bundle_mips_close(FILE *f) {
  acquire();
  for (unsigned i = 0; i < STREAM_SLOTS; i++)
    if (streams[i].f == f) streams[i].f = NULL;
  release();
}

int bundle_mips_fread(void *p, size_t size, size_t count, FILE *f, size_t *result) {
  if (__atomic_load_n(&texture_limit, __ATOMIC_ACQUIRE) < 1 || !size || !count) return 0;
  const parsed_file *file = NULL;
  acquire();
  for (unsigned i = 0; i < STREAM_SLOTS; i++)
    if (streams[i].f == f) { file = streams[i].file; break; }
  release();
  if (!file) return 0;
  long at = ftell(f);
  uint64_t total = (uint64_t)size * count;
  if (at < 0 || total > 0x7fffffff) return 0;
  uint32_t pos = (uint32_t)at, end = pos + (uint32_t)total;
  if (end < pos) return 0;
  // Only reads that contain a whole skipped page are worth splitting.
  const mip_range *first = NULL;
  for (unsigned i = 0; i < file->count; i++)
    if (file->ranges[i].start < end && file->ranges[i].end > pos &&
        (file->ranges[i].end < end ? file->ranges[i].end : end) -
        (file->ranges[i].start > pos ? file->ranges[i].start : pos) >= MIN_SKIP) { first = &file->ranges[i]; break; }
  if (!first) return 0;
  uint8_t *out = p;
  uint32_t cur = pos;
  uint64_t skipped = 0;
  while (cur < end) {
    const mip_range *r = NULL;
    for (unsigned i = 0; i < file->count; i++)
      if (file->ranges[i].end > cur && file->ranges[i].start < end &&
          (!r || file->ranges[i].start < r->start)) r = &file->ranges[i];
    uint32_t until = r ? (r->start > cur ? r->start : cur) : end;
    if (until > cur) {
      size_t want = until - cur, got = fread(out + (cur - pos), 1, want, f);
      cur += got;
      if (got != want) break;
    }
    if (!r || cur >= end) break;
    uint32_t resume = r->end < end ? r->end : end;
    if (fseek(f, resume, SEEK_SET) != 0) break;
    skipped += resume - cur;
    cur = resume;
  }
  __atomic_add_fetch(&skipped_bytes, skipped, __ATOMIC_RELAXED);
  uint64_t splits = __atomic_add_fetch(&reads_split, 1, __ATOMIC_RELAXED);
  if (splits <= 4 || (splits & (splits - 1)) == 0)
    L_INFO("io: top-mip reads skipped %llu, %llu MiB not read from the card",
           (unsigned long long)splits, (unsigned long long)(__atomic_load_n(&skipped_bytes, __ATOMIC_RELAXED) >> 20));
  *result = (cur - pos) / size;
  return 1;
}

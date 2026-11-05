#include "shader_warmup.h"
#include "../port_menu.h"
#include "../bridges/bridges.h"
#include "observe.h"
#include "log.h"
#include <vitaGL.h>
#include <ctype.h>
#include <dirent.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define MAX_CACHE_FILES 4096
// Bound the file list separately from vitaGL's 8 MiB prepared-program budget.
typedef struct { char path[256]; size_t bytes; time_t modified; } cache_file;
static int hexadecimal(const char *s, size_t n) {
  for (size_t i = 0; i < n; i++) if (!isxdigit((unsigned char)s[i])) return 0;
  return 1;
}
static int recent_first(const void *a, const void *b) {
  const cache_file *x = a, *y = b;
  if (x->modified != y->modified) return x->modified > y->modified ? -1 : 1;
  return strcmp(x->path, y->path);
}
void shader_cache_warmup(void) {
  uint64_t start = observe_now(), last_draw = start;
  port_shader_progress(0, 0, 0, 0);
  const char *root = vglGetShaderCachePath();
  cache_file *files = NULL;
  unsigned count = 0, capacity = 0, omitted = 0;
  for (unsigned stage = 0; stage < 2; stage++) {
    char parent[256];
    if (snprintf(parent, sizeof(parent), "%s/%c", root, stage ? 'f' : 'v') >= sizeof(parent)) continue;
    DIR *dir = opendir(parent);
    if (!dir) continue;
    struct dirent *shard;
    while ((shard = readdir(dir))) {
      if (strlen(shard->d_name) != 2 || !hexadecimal(shard->d_name, 2)) continue;
      char path[256];
      if (snprintf(path, sizeof(path), "%s/%s", parent, shard->d_name) >= sizeof(path)) continue;
      DIR *sub = opendir(path);
      if (!sub) continue;
      struct dirent *item;
      while ((item = readdir(sub))) {
        if (strlen(item->d_name) != 20 || strcmp(item->d_name + 16, ".gxp") || !hexadecimal(item->d_name, 16)) continue;
        cache_file next;
        if (snprintf(next.path, sizeof(next.path), "%s/%s", path, item->d_name) >= sizeof(next.path)) continue;
        struct stat st;
        if (stat(next.path, &st) || !S_ISREG(st.st_mode)) continue;
        if (count == MAX_CACHE_FILES) { omitted++; continue; }
        if (count == capacity) {
          unsigned grow = capacity ? capacity * 2 : 64;
          cache_file *new_files = realloc(files, grow * sizeof(*files));
          if (!new_files) { omitted++; continue; }
          files = new_files; capacity = grow;
        }
        next.bytes = st.st_size > 0 ? (size_t)st.st_size : 0;
        next.modified = st.st_mtime;
        files[count++] = next;
      }
      closedir(sub);
      // Long scans keep the screen and watchdog alive without advancing a fake total.
      watchdog_frames++;
      if (observe_now() - last_draw >= 50000) {
        port_shader_progress(0, 0, 0, 0); last_draw = observe_now();
      }
    }
    closedir(dir);
  }
  if (count > 1) qsort(files, count, sizeof(*files), recent_first);
  unsigned prepared = 0, failed = 0, limited = 0;
  port_shader_progress(1, 0, count, 0);
  for (unsigned i = 0; i < count; i++) {
    int result = vglPreloadShaderCacheFile(files[i].path, files[i].bytes);
    if (result > 0) prepared++;
    else if (result < 0) limited++;
    else failed++;
    watchdog_frames++;
    if (observe_now() - last_draw >= 50000 || i + 1 == count) {
      port_shader_progress(1, i + 1, count, prepared); last_draw = observe_now();
    }
  }
  free(files);
  port_shader_progress(2, count, count, prepared);
  port_shader_progress(3, 0, 0, 0);
  L_INFO("gl: boot shader preparation files=%u prepared=%u invalid=%u limited=%u omitted=%u elapsed=%u ms (before Unity)",
         count, prepared, failed, limited, omitted, (unsigned)((observe_now() - start) / 1000));
  observe_span(OBS_SHADER, "boot.shader-preparation", start, 0, prepared, count);
}

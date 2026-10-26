// Virtual /proc. Mono refuses to start without /proc/self/maps, Boehm GC reads
// /proc/self/stat (main stack base) and /proc/stat (CPU count), and Unity reads
// /proc/cpuinfo for NEON detection. Each read regenerates the file under
// ux0:data/srr/cache/ and hands that path to the real open/fopen.

#include "bridges.h"
#include "../config.h"
#include "../loader/so_util.h"
#include "../util/log.h"

#include <stdio.h>
#include <string.h>

#include <psp2/io/fcntl.h>
#include <psp2/kernel/threadmgr.h>

#define PROC_PID "1000"  // keep in sync with getpid_bridge()

static SceUID main_thread;

static int gen_maps(char *buf, size_t size) {
  int n = 0;
  for (so_module *m = so_modules(); m; m = m->next) {
    n += snprintf(buf + n, size - n, "%08x-%08x r-xp 00000000 b3:19 %-10d " ANDROID_APK_DIR "/lib/%s\n",
                  m->text_base, m->text_base + m->text_size, 1, m->name);
    for (int i = 0; i < m->n_data; i++)
      n += snprintf(buf + n, size - n, "%08x-%08x rw-p 00000000 b3:19 %-10d " ANDROID_APK_DIR "/lib/%s\n",
                    m->data_base[i], m->data_base[i] + m->data_size[i], 1, m->name);
  }
  SceKernelThreadInfo info;
  info.size = sizeof(info);
  if (sceKernelGetThreadInfo(main_thread, &info) >= 0) {
    uintptr_t lo = (uintptr_t)info.stack;
    n += snprintf(buf + n, size - n, "%08x-%08x rw-p 00000000 00:00 0          [stack]\n", lo, lo + info.stackSize);
  }
  return n;
}

static int gen_self_stat(char *buf, size_t size) {
  uintptr_t startstack = 0;
  SceKernelThreadInfo info;
  info.size = sizeof(info);
  if (sceKernelGetThreadInfo(main_thread, &info) >= 0)
    startstack = (uintptr_t)info.stack + info.stackSize - 16;
  // Field 28 is startstack; everything else is neutral.
  return snprintf(buf, size,
                  PROC_PID " (srr) R 1 " PROC_PID " " PROC_PID " 0 -1 4194560 0 0 0 0 0 0 0 0 20 0 8 0 0 "
                  "0 0 4294967295 0 0 %u 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0\n",
                  startstack);
}

static int gen_stat(char *buf, size_t size) {
  return snprintf(buf, size,
                  "cpu  100 0 100 1000 0 0 0 0 0 0\n"
                  "cpu0 50 0 50 500 0 0 0 0 0 0\n"
                  "cpu1 25 0 25 250 0 0 0 0 0 0\n"
                  "cpu2 25 0 25 250 0 0 0 0 0 0\n"
                  "btime 0\nprocesses 1\nprocs_running 1\nprocs_blocked 0\n");
}

static int gen_cpuinfo(char *buf, size_t size) {
  int n = 0;
  n += snprintf(buf + n, size - n, "Processor\t: ARMv7 Processor rev 0 (v7l)\n");
  for (int i = 0; i < 3; i++)
    n += snprintf(buf + n, size - n,
                  "processor\t: %d\nBogoMIPS\t: 888.00\n"
                  "Features\t: swp half thumb fastmult vfp edsp neon vfpv3 tls vfpd32\n"
                  "CPU implementer\t: 0x41\nCPU architecture: 7\nCPU variant\t: 0x4\n"
                  "CPU part\t: 0xc09\nCPU revision\t: 0\n\n",
                  i);
  n += snprintf(buf + n, size - n, "Hardware\t: PS Vita\nRevision\t: 0000\nSerial\t\t: 0000000000000000\n");
  return n;
}

static int gen_meminfo(char *buf, size_t size) {
  return snprintf(buf, size, "MemTotal:         524288 kB\nMemFree:          131072 kB\nCached:                0 kB\n");
}

static int gen_empty(char *buf, size_t size) {
  buf[0] = 0;
  return 0;
}

static const struct {
  const char *name;  // path after /proc/self/ or /proc/
  int per_process;
  int (*gen)(char *, size_t);
} files[] = {
    {"maps", 1, gen_maps},       {"stat", 1, gen_self_stat}, {"auxv", 1, gen_empty},
    {"stat", 0, gen_stat},       {"cpuinfo", 0, gen_cpuinfo}, {"meminfo", 0, gen_meminfo},
};

const char *proc_translate(const char *path, char *out, size_t out_size) {
  const char *rest = NULL;
  int per_process = 0;
  if (strncmp(path, "/proc/self/", 11) == 0) {
    rest = path + 11;
    per_process = 1;
  } else if (strncmp(path, "/proc/" PROC_PID "/", 6 + sizeof(PROC_PID)) == 0) {
    rest = path + 6 + sizeof(PROC_PID);
    per_process = 1;
  } else if (strncmp(path, "/proc/", 6) == 0) {
    rest = path + 6;
  } else {
    return NULL;
  }

  for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
    if (files[i].per_process != per_process || strcmp(files[i].name, rest) != 0)
      continue;
    static char buf[16 * 1024];
    int len = files[i].gen(buf, sizeof(buf));
    snprintf(out, out_size, CACHE_PATH "/proc_%s%s", per_process ? "self_" : "", rest);
    SceUID fd = sceIoOpen(out, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd >= 0) {
      sceIoWrite(fd, buf, len);
      sceIoClose(fd);
    }
    L_DEBUG("proc: %s -> %s (%d bytes)", path, out, len);
    return out;
  }
  L_WARN("proc: %s not emulated", path);
  return NULL;
}

void proc_init(void) { main_thread = sceKernelGetThreadId(); }

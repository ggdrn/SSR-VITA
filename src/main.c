// SRR Vita: Android so-loader for Sonic Runners Revival (Unity 4.7 / Mono).
// The game folder holds only the original APK; native libraries are inflated
// from it into memory at boot and linked against the Vita bridges.

#include "config.h"
#include "version.h"
#include "bridges/bridges.h"
#include "jni/jni.h"
#include "loader/so_util.h"
#include "util/apk.h"
#include "util/log.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/apputil.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <psp2/vshbridge.h>

int _newlib_heap_size_user = NEWLIB_HEAP_SIZE;
unsigned int sceUserMainThreadStackSize = 8 * 1024 * 1024;

extern const so_default_dynlib default_dynlib[];
extern const int default_dynlib_len;

void libc_bridge_init(void);
void pthread_bridge_init(void);
void gl_bridge_init(void);
int unity_run(void);

// Load order = initialisation order (dependencies first).
static const char *const libraries[] = {"libmono.so", "libmain.so", "libunity.so", "libcri_ware_unity.so"};
#define NUM_LIBS (sizeof(libraries) / sizeof(libraries[0]))
static so_module modules[NUM_LIBS];

static void log_free_memory(const char *when) {
  SceKernelFreeMemorySizeInfo info;
  info.size = sizeof(info);
  sceKernelGetFreeMemorySize(&info);
  L_INFO("mem %s: user %d KB, cdram %d KB, phycont %d KB", when, info.size_user / 1024, info.size_cdram / 1024,
         info.size_phycont / 1024);
}

static int file_exists(const char *path) {
  SceIoStat st;
  return sceIoGetstat(path, &st) >= 0;
}

static int module_loaded(const char *name) {
  char buf[0x100];
  return _vshKernelSearchModuleByName(name, buf) >= 0;
}

// The APK keeps its original name; take the first *.apk in the game folder.
static int find_apk(char *out, size_t size) {
  DIR *d = opendir(DATA_PATH);
  if (!d)
    return -1;
  struct dirent *e;
  int found = -1;
  while ((e = readdir(d))) {
    size_t n = strlen(e->d_name);
    if (n > 4 && strcasecmp(e->d_name + n - 4, ".apk") == 0) {
      snprintf(out, size, DATA_PATH "/%s", e->d_name);
      found = 0;
      break;
    }
  }
  closedir(d);
  return found;
}

static void check_requirements(void) {
  if (!module_loaded("kubridge"))
    fatal_error("kubridge.skprx is not loaded.\nInstall it in ur0:tai/ and add it under *KERNEL in config.txt.");
  if (!file_exists("ur0:data/libshacccg.suprx") && !file_exists("ur0:data/external/libshacccg.suprx"))
    fatal_error("libshacccg.suprx is missing.\nExtract it with ShaRKBR33D and try again.");
}

static void load_libraries(void) {
  for (size_t i = 0; i < NUM_LIBS; i++) {
    char path[128];
    snprintf(path, sizeof(path), LIB_DIR "%s", libraries[i]);
    const apk_entry *e = apk_find(path);
    if (!e)
      fatal_error("%s not found inside the APK.", path);
    size_t size;
    void *elf = apk_extract(e, &size);
    if (!elf)
      fatal_error("Could not extract %s (not enough memory?).", path);
    int r = so_load(&modules[i], libraries[i], elf, size, 0);
    free(elf);
    if (r < 0)
      fatal_error("Could not load %s (error %d).\nSee " LOG_PATH, libraries[i], r);
  }
  // Link only after every module is mapped so cross-library imports resolve.
  for (size_t i = 0; i < NUM_LIBS; i++) {
    so_relocate(&modules[i]);
    so_resolve(&modules[i], default_dynlib, default_dynlib_len);
    so_flush_caches(&modules[i]);
  }
  for (size_t i = 0; i < NUM_LIBS; i++) {
    L_INFO("init %s", libraries[i]);
    so_initialize(&modules[i]);
  }
}

int main(void) {
  log_init();
  L_INFO("SRR Vita " SRR_VERSION_STRING " (build %s)", __DATE__ " " __TIME__);

  scePowerSetArmClockFrequency(444);
  scePowerSetBusClockFrequency(222);
  scePowerSetGpuClockFrequency(222);
  scePowerSetGpuXbarClockFrequency(166);

  SceAppUtilInitParam init_param;
  SceAppUtilBootParam boot_param;
  memset(&init_param, 0, sizeof(init_param));
  memset(&boot_param, 0, sizeof(boot_param));
  sceAppUtilInit(&init_param, &boot_param);

  check_requirements();

  char apk[256];
  if (find_apk(apk, sizeof(apk)) < 0)
    fatal_error("No APK found.\nCopy the game APK to " DATA_PATH "/");
  if (apk_open(apk) < 0)
    fatal_error("Could not read %s.\nIs the APK complete?", apk);

  fs_init(apk);
  libc_bridge_init();
  pthread_bridge_init();
  mem_init();
  signal_init();
  input_init();
  audio_init();
  jni_init();

  // Libraries first: vitaGL claims all free RAM above VGL_RAM_THRESHOLD, so
  // the engine's code/data blocks must already be allocated by then.
  log_free_memory("before libraries");
  load_libraries();
  log_free_memory("after libraries");
  gl_bridge_init();
  log_free_memory("after vitaGL");

  int r = unity_run();
  L_INFO("game exited with %d", r);
  sceKernelExitProcess(0);
  return 0;
}

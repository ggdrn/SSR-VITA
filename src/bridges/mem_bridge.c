// mmap/mprotect for Mono's JIT (needs RWX pages via kubridge), dlopen/dlsym
// over the loaded modules, syscall() and the ARM unwinder hook.

#include "bridges.h"
#include "../loader/so_util.h"
#include "../util/log.h"
#include "../ui_mesh.h"

#include <kubridge.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

int *__errno_bionic(void);
uintptr_t dynlib_lookup(const char *name);

#define B_PROT_READ 1
#define B_PROT_WRITE 2
#define B_PROT_EXEC 4
#define B_PROT_NONE 0
#define B_MAP_FIXED 0x10
#define B_MAP_ANONYMOUS 0x20
#define B_MAP_FAILED ((void *)-1)

#define ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((a) - 1))

typedef enum { MAP_HEAP, MAP_EXEC } map_kind;

typedef struct {
  uintptr_t addr;
  size_t len;
  map_kind kind;
  SceUID block;
} mapping;

#define MAX_MAPPINGS 4096
static mapping maps[MAX_MAPPINGS];
static SceKernelLwMutexWork map_lock;

static mapping *map_find(uintptr_t addr) {
  for (int i = 0; i < MAX_MAPPINGS; i++) {
    if (maps[i].addr && addr >= maps[i].addr && addr < maps[i].addr + maps[i].len)
      return &maps[i];
  }
  return NULL;
}

static void map_add(uintptr_t addr, size_t len, map_kind kind, SceUID block) {
  sceKernelLockLwMutex(&map_lock, 1, NULL);
  for (int i = 0; i < MAX_MAPPINGS; i++) {
    if (!maps[i].addr) {
      maps[i] = (mapping){addr, len, kind, block};
      break;
    }
  }
  sceKernelUnlockLwMutex(&map_lock, 1);
}

static void *exec_alloc(size_t len, SceUID *block) {
  void *addr = NULL;
  len = ALIGN_UP(len, 0x1000);
  SceUID uid = kuKernelMemReserve(&addr, len, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW);
  if (uid < 0) {
    L_ERROR("mmap: kuKernelMemReserve(0x%X) failed 0x%08X", len, uid);
    return NULL;
  }
  int r = kuKernelMemCommit(addr, len, KU_KERNEL_PROT_READ | KU_KERNEL_PROT_WRITE | KU_KERNEL_PROT_EXEC, NULL);
  if (r < 0) {
    L_ERROR("mmap: kuKernelMemCommit RWX failed 0x%08X", r);
    sceKernelFreeMemBlock(uid);
    return NULL;
  }
  memset(addr, 0, len);
  *block = uid;
  return addr;
}

void *mmap_bridge(void *addr, size_t len, int prot, int flags, int fd, off_t off) {
  void *p;
  SceUID block = -1;
  size_t alen = ALIGN_UP(len, 0x1000);

  // MAP_FIXED over memory we already handed out: Boehm GC "unmaps" free
  // heap blocks with mmap(PROT_NONE, MAP_FIXED) and remaps them later with
  // PROT_READ|PROT_WRITE, expecting the same address (it aborts otherwise).
  // The memory stays ours; a remap must read back as zeroes.
  if ((flags & B_MAP_FIXED) && addr) {
    mapping *m = map_find((uintptr_t)addr);
    if (m && (uintptr_t)addr + len <= m->addr + m->len) {
      if (prot != B_PROT_NONE)
        memset(addr, 0, len);
      return addr;
    }
    L_WARN("mmap: MAP_FIXED at %p+0x%X outside known mappings", addr, len);
  }

  if (prot & B_PROT_EXEC) {
    p = exec_alloc(alen, &block);
    if (!p)
      goto fail;
    map_add((uintptr_t)p, alen, MAP_EXEC, block);
  } else {
    p = memalign(0x1000, alen);
    if (!p)
      goto fail;
    memset(p, 0, alen);
    map_add((uintptr_t)p, alen, MAP_HEAP, -1);
  }

  if (!(flags & B_MAP_ANONYMOUS) && fd >= 0) {
    // File mapping: private copy of the file contents.
    off_t cur = lseek(fd, 0, SEEK_CUR);
    lseek(fd, off, SEEK_SET);
    read(fd, p, len);
    lseek(fd, cur, SEEK_SET);
  }
  return p;

fail:
  *__errno_bionic() = 12;  // ENOMEM
  return B_MAP_FAILED;
}

int munmap_bridge(void *addr, size_t len) {
  sceKernelLockLwMutex(&map_lock, 1, NULL);
  mapping *m = map_find((uintptr_t)addr);
  if (m && m->addr == (uintptr_t)addr) {
    if (m->kind == MAP_EXEC)
      sceKernelFreeMemBlock(m->block);
    else
      free(addr);
    m->addr = 0;
  } else if (m) {
    L_DEBUG("munmap: partial unmap %p+0x%X ignored", addr, len);
  }
  sceKernelUnlockLwMutex(&map_lock, 1);
  return 0;
}

int mprotect_bridge(void *addr, size_t len, int prot) {
  mapping *m = map_find((uintptr_t)addr);
  if (m && m->kind == MAP_EXEC) {
    // Keep JIT pages RWX: Mono patches code in place.
    return 0;
  }
  if (prot & B_PROT_EXEC)
    L_WARN("mprotect: PROT_EXEC requested on heap memory %p+0x%X", addr, len);
  return 0;
}

int madvise_bridge(void *addr, size_t len, int advice) { return 0; }

void *mremap_bridge(void *old, size_t old_len, size_t new_len, int flags) {
  mapping *m = map_find((uintptr_t)old);
  int prot = (m && m->kind == MAP_EXEC) ? B_PROT_EXEC | B_PROT_READ | B_PROT_WRITE : B_PROT_READ | B_PROT_WRITE;
  void *n = mmap_bridge(NULL, new_len, prot, B_MAP_ANONYMOUS, -1, 0);
  if (n == B_MAP_FAILED)
    return n;
  memcpy(n, old, old_len < new_len ? old_len : new_len);
  munmap_bridge(old, old_len);
  return n;
}

// --- dlfcn ------------------------------------------------------------------

#define DL_DEFAULT_HANDLE ((void *)0xD1D1D1D1)
// Discord Rich Presence has no Vita counterpart; every export becomes a no-op
// so the game's per-frame DiscordRpc calls stop throwing DllNotFoundException.
#define DL_DISCORD_HANDLE ((void *)0xD15C0001)
int ret0_bridge(void);
static const char *dl_error;

void *dlopen_bridge(const char *name, int flags) {
  if (!name)
    return DL_DEFAULT_HANDLE;
  so_module *m = so_find_module(name);
  if (m) {
    L_DEBUG("dlopen(%s) -> %s", name, m->name);
    return m;
  }
  if (strstr(name, "discord-rpc")) {
    L_DEBUG("dlopen(%s) -> no-op stub", name);
    return DL_DISCORD_HANDLE;
  }
  // System libraries are served by the bridge table.
  static const char *system_libs[] = {"libc", "libm", "libdl", "liblog", "libGLES", "libEGL", "libz",
                                      "libandroid", "libOpenSLES", "libstdc++", "__Internal"};
  const char *base = strrchr(name, '/');
  base = base ? base + 1 : name;
  for (size_t i = 0; i < sizeof(system_libs) / sizeof(system_libs[0]); i++) {
    if (strncmp(base, system_libs[i], strlen(system_libs[i])) == 0)
      return DL_DEFAULT_HANDLE;
  }
  L_WARN("dlopen(%s) failed", name);
  dl_error = "library not found";
  return NULL;
}

void *dlsym_bridge(void *handle, const char *sym) {
  if (handle == DL_DISCORD_HANDLE)
    return (void *)ret0_bridge;
  uintptr_t addr = 0;
  if (sym && !strcmp(sym, "mono_add_internal_call"))
    addr = (uintptr_t)ui_mesh_icall_registrar();
  if (!addr && handle && handle != DL_DEFAULT_HANDLE)
    addr = so_symbol((so_module *)handle, sym);
  if (!addr)
    addr = dynlib_lookup(sym);
  if (!addr && (!handle || handle == DL_DEFAULT_HANDLE))
    addr = so_symbol_global(sym);
  if (!addr) {
    L_DEBUG("dlsym(%s) not found", sym);
    dl_error = "symbol not found";
  }
  return (void *)addr;
}

int dlclose_bridge(void *handle) { return 0; }

const char *dlerror_bridge(void) {
  const char *e = dl_error;
  dl_error = NULL;
  return e;
}

// --- syscall ----------------------------------------------------------------

#define B_NR_gettid 224
#define B_NR_cacheflush 0xf0002

long syscall_bridge(long n, long a, long b, long c) {
  switch (n) {
  case B_NR_cacheflush:
    kuKernelFlushCaches((void *)a, b - a);
    return 0;
  case B_NR_gettid:
    return sceKernelGetThreadId();
  default:
    L_WARN("syscall(%ld) not implemented", n);
    *__errno_bionic() = 38;  // ENOSYS
    return -1;
  }
}

// --- ARM EHABI --------------------------------------------------------------

uintptr_t __gnu_Unwind_Find_exidx_bridge(uintptr_t pc, int *pcount) {
  so_module *m = so_module_at(pc);
  if (m && m->exidx) {
    *pcount = m->num_exidx;
    return m->exidx;
  }
  *pcount = 0;
  return 0;
}

void mem_init(void) { sceKernelCreateLwMutex(&map_lock, "mmap", SCE_KERNEL_MUTEX_ATTR_RECURSIVE, 0, NULL); }

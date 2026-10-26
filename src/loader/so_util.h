#ifndef SRR_SO_UTIL_H
#define SRR_SO_UTIL_H

#include <elf.h>
#include <stdint.h>
#include <stddef.h>
#include <psp2/types.h>

#define SO_MAX_DATA_SEG 4

typedef struct so_default_dynlib {
  const char *symbol;
  uintptr_t func;
} so_default_dynlib;

typedef struct so_module {
  struct so_module *next;
  char name[64];

  uintptr_t base;                 // address of vaddr 0
  SceUID text_blockid;
  uintptr_t text_base;
  size_t text_size;
  SceUID data_blockid[SO_MAX_DATA_SEG];
  uintptr_t data_base[SO_MAX_DATA_SEG];
  size_t data_size[SO_MAX_DATA_SEG];
  int n_data;
  uintptr_t end;                  // first free address after the module

  Elf32_Dyn *dynamic;
  Elf32_Sym *dynsym;
  int num_dynsym;
  const char *dynstr;
  uint32_t *hash;
  Elf32_Rel *rel;
  size_t num_rel;
  Elf32_Rel *jmprel;
  size_t num_jmprel;
  void (*init)(void);
  uintptr_t *init_array;
  int num_init_array;
  uintptr_t exidx;                // ARM unwind index (PT_ARM_EXIDX)
  int num_exidx;
} so_module;

typedef struct so_hook {
  uintptr_t addr;
  uintptr_t thumb_addr;
  uint32_t orig_instr[2];
  uint32_t patch_instr[2];
} so_hook;

// Maps an ELF image held in memory at `load_addr` (0 = right after the last
// loaded module). The source buffer can be freed once this returns.
int so_load(so_module *mod, const char *name, const void *elf, size_t size, uintptr_t load_addr);
int so_relocate(so_module *mod);
// Resolves imports against the other loaded modules, then `table`. Anything
// still missing is bound to a logging stub that returns 0.
int so_resolve(so_module *mod, const so_default_dynlib *table, int table_len);
void so_flush_caches(so_module *mod);
void so_initialize(so_module *mod);

uintptr_t so_symbol(so_module *mod, const char *name);
so_module *so_find_module(const char *name);
// Head of the loaded module list (linked through ->next).
so_module *so_modules(void);
// Searches every loaded module's exports.
uintptr_t so_symbol_global(const char *name);
// Returns the module containing `addr`, if any (used for crash logs).
so_module *so_module_at(uintptr_t addr);

so_hook hook_addr(uintptr_t addr, uintptr_t dst);

#define SO_CONTINUE(type, h, ...) ({ \
  kuKernelCpuUnrestrictedMemcpy((void *)(h).addr, (h).orig_instr, sizeof((h).orig_instr)); \
  kuKernelFlushCaches((void *)(h).addr, sizeof((h).orig_instr)); \
  type r = (h).thumb_addr ? ((type(*)())(h).thumb_addr)(__VA_ARGS__) : ((type(*)())(h).addr)(__VA_ARGS__); \
  kuKernelCpuUnrestrictedMemcpy((void *)(h).addr, (h).patch_instr, sizeof((h).patch_instr)); \
  kuKernelFlushCaches((void *)(h).addr, sizeof((h).patch_instr)); \
  r; \
})

#endif

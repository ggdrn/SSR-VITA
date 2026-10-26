// ELF shared object loader for Android armeabi-v7a libraries.
// Text segments go into kubridge RX memory blocks at a fixed address, data
// segments into RW blocks placed right after them so PC-relative addressing
// keeps working. Imports are resolved by name.

#include "so_util.h"
#include "../util/log.h"

#include <kubridge.h>
#include <psp2/kernel/sysmem.h>
#include <stdlib.h>
#include <string.h>

#define ALIGN_UP(x, a)   (((x) + ((a) - 1)) & ~((a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((a) - 1))
#define PAGE 0x1000

#ifndef R_ARM_ABS32
#define R_ARM_ABS32 2
#endif
#ifndef R_ARM_GLOB_DAT
#define R_ARM_GLOB_DAT 21
#endif

#ifndef SCE_KERNEL_MEMBLOCK_TYPE_USER_RX
#define SCE_KERNEL_MEMBLOCK_TYPE_USER_RX 0x0C20D050
#endif

#define DEFAULT_LOAD_ADDR 0x98000000

static so_module *modules;
static uintptr_t next_load_addr = DEFAULT_LOAD_ADDR;

static SceUID alloc_block(const char *name, SceKernelMemBlockType type, uintptr_t addr, size_t size) {
  SceKernelAllocMemBlockKernelOpt opt;
  memset(&opt, 0, sizeof(opt));
  opt.size = sizeof(opt);
  opt.attr = 0x1;  // HAS_VBASE
  opt.field_C = addr;
  return kuKernelAllocMemBlock(name, type, size, &opt);
}

int so_load(so_module *mod, const char *name, const void *elf, size_t size, uintptr_t load_addr) {
  memset(mod, 0, sizeof(*mod));
  strncpy(mod->name, name, sizeof(mod->name) - 1);
  const uint8_t *data = elf;
  const Elf32_Ehdr *ehdr = elf;
  if (size < sizeof(Elf32_Ehdr) || memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0 || ehdr->e_machine != EM_ARM) {
    L_ERROR("so: %s is not an ARM ELF", name);
    return -1;
  }
  const Elf32_Phdr *phdr = (const Elf32_Phdr *)(data + ehdr->e_phoff);

  if (!load_addr)
    load_addr = next_load_addr;

  // Text first: it defines the module base.
  const Elf32_Phdr *text = NULL;
  for (int i = 0; i < ehdr->e_phnum; i++) {
    if (phdr[i].p_type == PT_LOAD && (phdr[i].p_flags & PF_X)) {
      text = &phdr[i];
      break;
    }
  }
  if (!text) {
    L_ERROR("so: %s has no executable segment", name);
    return -1;
  }
  mod->base = load_addr - ALIGN_DOWN(text->p_vaddr, PAGE);
  mod->text_base = load_addr;
  mod->text_size = ALIGN_UP(text->p_vaddr + text->p_memsz, PAGE) - ALIGN_DOWN(text->p_vaddr, PAGE);
  mod->text_blockid = alloc_block("so_rx", SCE_KERNEL_MEMBLOCK_TYPE_USER_RX, mod->text_base, mod->text_size);
  if (mod->text_blockid < 0) {
    L_ERROR("so: %s text block alloc failed 0x%08X (addr 0x%08X size 0x%X)", name, mod->text_blockid,
            mod->text_base, mod->text_size);
    return -2;
  }
  {
    void *zero = calloc(1, mod->text_size);
    kuKernelCpuUnrestrictedMemcpy((void *)mod->text_base, zero, mod->text_size);
    free(zero);
    kuKernelCpuUnrestrictedMemcpy((void *)(mod->base + text->p_vaddr), data + text->p_offset, text->p_filesz);
  }
  uintptr_t cur_end = mod->text_base + mod->text_size;

  for (int i = 0; i < ehdr->e_phnum; i++) {
    const Elf32_Phdr *p = &phdr[i];
    if (p->p_type != PT_LOAD || p == text)
      continue;
    if (mod->n_data >= SO_MAX_DATA_SEG) {
      L_ERROR("so: %s has too many data segments", name);
      return -3;
    }
    uintptr_t seg = mod->base + p->p_vaddr;
    uintptr_t start = ALIGN_DOWN(seg, PAGE);
    uintptr_t end = ALIGN_UP(seg + p->p_memsz, PAGE);
    if (start < cur_end) {
      L_WARN("so: %s data segment %d shares a page with the previous segment", name, i);
      start = cur_end;
    }
    if (end > start) {
      int n = mod->n_data;
      mod->data_blockid[n] = alloc_block("so_rw", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, start, end - start);
      if (mod->data_blockid[n] < 0) {
        L_ERROR("so: %s data block alloc failed 0x%08X", name, mod->data_blockid[n]);
        return -4;
      }
      mod->data_base[n] = start;
      mod->data_size[n] = end - start;
      mod->n_data++;
      memset((void *)start, 0, end - start);
      cur_end = end;
    }
    // Copy whatever part of the segment is in RW memory; a shared first page
    // (rare) goes through the unrestricted path.
    if (seg < start) {
      size_t head = start - seg;
      kuKernelCpuUnrestrictedMemcpy((void *)seg, data + p->p_offset, head < p->p_filesz ? head : p->p_filesz);
      if (head < p->p_filesz)
        memcpy((void *)start, data + p->p_offset + head, p->p_filesz - head);
    } else {
      memcpy((void *)seg, data + p->p_offset, p->p_filesz);
    }
  }
  mod->end = cur_end;
  next_load_addr = ALIGN_UP(cur_end, 0x100000);

  // Dynamic section and unwind tables.
  for (int i = 0; i < ehdr->e_phnum; i++) {
    if (phdr[i].p_type == PT_DYNAMIC)
      mod->dynamic = (Elf32_Dyn *)(mod->base + phdr[i].p_vaddr);
    if (phdr[i].p_type == PT_ARM_EXIDX) {
      mod->exidx = mod->base + phdr[i].p_vaddr;
      mod->num_exidx = phdr[i].p_memsz / 8;
    }
  }
  if (!mod->dynamic) {
    L_ERROR("so: %s has no dynamic section", name);
    return -5;
  }
  size_t relsz = 0, pltrelsz = 0;
  for (Elf32_Dyn *d = mod->dynamic; d->d_tag != DT_NULL; d++) {
    uintptr_t v = d->d_un.d_ptr;
    switch (d->d_tag) {
    case DT_SYMTAB: mod->dynsym = (Elf32_Sym *)(mod->base + v); break;
    case DT_STRTAB: mod->dynstr = (const char *)(mod->base + v); break;
    case DT_HASH: mod->hash = (uint32_t *)(mod->base + v); break;
    case DT_REL: mod->rel = (Elf32_Rel *)(mod->base + v); break;
    case DT_RELSZ: relsz = v; break;
    case DT_JMPREL: mod->jmprel = (Elf32_Rel *)(mod->base + v); break;
    case DT_PLTRELSZ: pltrelsz = v; break;
    case DT_INIT: mod->init = (void (*)(void))(mod->base + v); break;
    case DT_INIT_ARRAY: mod->init_array = (uintptr_t *)(mod->base + v); break;
    case DT_INIT_ARRAYSZ: mod->num_init_array = v / sizeof(uintptr_t); break;
    default: break;
    }
  }
  mod->num_rel = relsz / sizeof(Elf32_Rel);
  mod->num_jmprel = pltrelsz / sizeof(Elf32_Rel);
  if (mod->hash) {
    mod->num_dynsym = mod->hash[1];  // nchain
  } else {
    // Fall back to the section headers that are still in the file buffer.
    const Elf32_Shdr *shdr = (const Elf32_Shdr *)(data + ehdr->e_shoff);
    for (int i = 0; i < ehdr->e_shnum; i++) {
      if (shdr[i].sh_type == SHT_DYNSYM)
        mod->num_dynsym = shdr[i].sh_size / sizeof(Elf32_Sym);
    }
  }

  mod->next = modules;
  modules = mod;
  L_INFO("so: %s loaded at 0x%08X-0x%08X (%d syms, %d rel, %d plt)", name, mod->text_base, mod->end,
         mod->num_dynsym, mod->num_rel, mod->num_jmprel);
  return 0;
}

// Relocation targets are normally in RW data, but text relocations exist.
static void write_word(so_module *mod, uintptr_t addr, uint32_t val) {
  if (addr >= mod->text_base && addr < mod->text_base + mod->text_size)
    kuKernelCpuUnrestrictedMemcpy((void *)addr, &val, sizeof(val));
  else
    *(uint32_t *)addr = val;
}

static int relocate_list(so_module *mod, Elf32_Rel *rels, size_t count) {
  for (size_t i = 0; i < count; i++) {
    uintptr_t ptr = mod->base + rels[i].r_offset;
    int type = ELF32_R_TYPE(rels[i].r_info);
    Elf32_Sym *sym = &mod->dynsym[ELF32_R_SYM(rels[i].r_info)];

    switch (type) {
    case R_ARM_RELATIVE:
      write_word(mod, ptr, *(uint32_t *)ptr + mod->base);
      break;
    case R_ARM_ABS32:
      if (sym->st_shndx != SHN_UNDEF)
        write_word(mod, ptr, *(uint32_t *)ptr + mod->base + sym->st_value);
      break;
    case R_ARM_GLOB_DAT:
    case R_ARM_JUMP_SLOT:
      if (sym->st_shndx != SHN_UNDEF)
        write_word(mod, ptr, mod->base + sym->st_value);
      break;
    case 0:
      break;
    default:
      L_ERROR("so: %s unsupported relocation type %d at 0x%X", mod->name, type, rels[i].r_offset);
      break;
    }
  }
  return 0;
}

int so_relocate(so_module *mod) {
  relocate_list(mod, mod->rel, mod->num_rel);
  relocate_list(mod, mod->jmprel, mod->num_jmprel);
  return 0;
}

// --- unresolved import stubs ---------------------------------------------
// Each missing import gets a 16-byte ARM thunk that loads its index into r12
// and jumps to so_unresolved_entry, which logs the symbol name and returns 0.

#define MAX_THUNKS 4096
#define THUNK_REGION_SIZE (MAX_THUNKS * 16)
#define THUNK_REGION_ADDR 0x97F00000

static const char *thunk_names[MAX_THUNKS];
static int thunk_calls[MAX_THUNKS];
static int num_thunks;
static SceUID thunk_block = -1;

void so_unresolved_log(int idx) {
  if (idx >= 0 && idx < num_thunks && thunk_calls[idx]++ < 3)
    L_WARN("called unimplemented import %s", thunk_names[idx]);
}

extern void so_unresolved_entry(void);
__asm__(
    ".arm\n"
    ".align 2\n"
    ".global so_unresolved_entry\n"
    ".type so_unresolved_entry, %function\n"
    "so_unresolved_entry:\n"
    "  push {r0-r3, r12, lr}\n"
    "  mov r0, r12\n"
    "  blx so_unresolved_log\n"
    "  pop {r0-r3, r12, lr}\n"
    "  mov r0, #0\n"
    "  mov r1, #0\n"
    "  bx lr\n");

static uintptr_t make_thunk(const char *name) {
  for (int i = 0; i < num_thunks; i++) {
    if (strcmp(thunk_names[i], name) == 0)
      return THUNK_REGION_ADDR + i * 16;
  }
  if (thunk_block < 0) {
    thunk_block = alloc_block("so_thunks", SCE_KERNEL_MEMBLOCK_TYPE_USER_RX, THUNK_REGION_ADDR, THUNK_REGION_SIZE);
    if (thunk_block < 0) {
      L_ERROR("so: thunk block alloc failed 0x%08X", thunk_block);
      return 0;
    }
  }
  if (num_thunks >= MAX_THUNKS)
    return 0;
  int idx = num_thunks++;
  thunk_names[idx] = name;
  uint32_t code[4] = {
      0xE59FC000,  // ldr r12, [pc]   -> code[2]
      0xE59FF000,  // ldr pc, [pc]    -> code[3]
      (uint32_t)idx,
      (uint32_t)&so_unresolved_entry,
  };
  uintptr_t addr = THUNK_REGION_ADDR + idx * 16;
  kuKernelCpuUnrestrictedMemcpy((void *)addr, code, sizeof(code));
  kuKernelFlushCaches((void *)addr, sizeof(code));
  return addr;
}

static uintptr_t lookup_table(const so_default_dynlib *table, int len, const char *name) {
  for (int i = 0; i < len; i++) {
    if (strcmp(table[i].symbol, name) == 0)
      return table[i].func;
  }
  return 0;
}

static int resolve_list(so_module *mod, Elf32_Rel *rels, size_t count, const so_default_dynlib *table,
                        int table_len, int *missing) {
  for (size_t i = 0; i < count; i++) {
    int type = ELF32_R_TYPE(rels[i].r_info);
    if (type != R_ARM_ABS32 && type != R_ARM_GLOB_DAT && type != R_ARM_JUMP_SLOT)
      continue;
    Elf32_Sym *sym = &mod->dynsym[ELF32_R_SYM(rels[i].r_info)];
    if (sym->st_shndx != SHN_UNDEF)
      continue;
    const char *name = mod->dynstr + sym->st_name;
    uintptr_t ptr = mod->base + rels[i].r_offset;

    // Our table wins over other modules so bridges can override exports.
    uintptr_t addr = lookup_table(table, table_len, name);
    if (!addr) {
      for (so_module *m = modules; m && !addr; m = m->next) {
        if (m != mod)
          addr = so_symbol(m, name);
      }
    }
    if (!addr) {
      if (ELF32_ST_BIND(sym->st_info) != STB_WEAK) {
        if (ELF32_ST_TYPE(sym->st_info) == STT_OBJECT)
          L_ERROR("so: %s missing data import %s", mod->name, name);
        else
          L_DEBUG("so: %s missing import %s", mod->name, name);
        (*missing)++;
      }
      if (ELF32_ST_TYPE(sym->st_info) == STT_OBJECT || ELF32_ST_BIND(sym->st_info) == STB_WEAK)
        continue;  // leave weak/data refs null
      addr = make_thunk(name);
    }
    write_word(mod, ptr, type == R_ARM_ABS32 ? *(uint32_t *)ptr + addr : addr);
  }
  return 0;
}

int so_resolve(so_module *mod, const so_default_dynlib *table, int table_len) {
  int missing = 0;
  resolve_list(mod, mod->rel, mod->num_rel, table, table_len, &missing);
  resolve_list(mod, mod->jmprel, mod->num_jmprel, table, table_len, &missing);
  L_INFO("so: %s resolved, %d imports without implementation", mod->name, missing);
  return missing;
}

void so_flush_caches(so_module *mod) {
  kuKernelFlushCaches((void *)mod->text_base, mod->text_size);
}

void so_initialize(so_module *mod) {
  if (mod->init)
    mod->init();
  for (int i = 0; i < mod->num_init_array; i++) {
    uintptr_t f = mod->init_array[i];
    if (f != 0 && f != (uintptr_t)-1)
      ((void (*)(void))f)();
  }
}

static uint32_t elf_hash(const char *name) {
  const uint8_t *p = (const uint8_t *)name;
  uint32_t h = 0, g;
  while (*p) {
    h = (h << 4) + *p++;
    g = h & 0xf0000000;
    if (g)
      h ^= g >> 24;
    h &= ~g;
  }
  return h;
}

uintptr_t so_symbol(so_module *mod, const char *name) {
  if (mod->hash) {
    uint32_t nbucket = mod->hash[0];
    uint32_t *bucket = &mod->hash[2];
    uint32_t *chain = &bucket[nbucket];
    for (uint32_t i = bucket[elf_hash(name) % nbucket]; i; i = chain[i]) {
      Elf32_Sym *s = &mod->dynsym[i];
      if (s->st_shndx != SHN_UNDEF && strcmp(mod->dynstr + s->st_name, name) == 0)
        return mod->base + s->st_value;
    }
    return 0;
  }
  for (int i = 0; i < mod->num_dynsym; i++) {
    Elf32_Sym *s = &mod->dynsym[i];
    if (s->st_shndx != SHN_UNDEF && strcmp(mod->dynstr + s->st_name, name) == 0)
      return mod->base + s->st_value;
  }
  return 0;
}

so_module *so_find_module(const char *name) {
  // Accept "libfoo.so", "foo", "/path/libfoo.so".
  const char *base = strrchr(name, '/');
  base = base ? base + 1 : name;
  for (so_module *m = modules; m; m = m->next) {
    if (strcmp(m->name, base) == 0)
      return m;
    size_t n = strlen(base);
    if (strncmp(m->name, "lib", 3) == 0 && strncmp(m->name + 3, base, n) == 0 &&
        (m->name[3 + n] == 0 || strcmp(m->name + 3 + n, ".so") == 0))
      return m;
  }
  return NULL;
}

uintptr_t so_symbol_global(const char *name) {
  for (so_module *m = modules; m; m = m->next) {
    uintptr_t a = so_symbol(m, name);
    if (a)
      return a;
  }
  return 0;
}

so_module *so_module_at(uintptr_t addr) {
  for (so_module *m = modules; m; m = m->next) {
    if (addr >= m->text_base && addr < m->end)
      return m;
  }
  return NULL;
}

so_hook hook_addr(uintptr_t addr, uintptr_t dst) {
  so_hook h;
  memset(&h, 0, sizeof(h));
  if (addr & 1) {
    // Thumb: align to 4 with a nop, then LDR.W PC, [PC, #0].
    h.thumb_addr = addr;
    addr &= ~1;
    h.addr = addr;
    if (addr & 2) {
      uint16_t nop = 0xbf00;
      kuKernelCpuUnrestrictedMemcpy(h.orig_instr, (void *)addr, sizeof(h.orig_instr));
      kuKernelCpuUnrestrictedMemcpy((void *)addr, &nop, sizeof(nop));
      addr += 2;
    } else {
      kuKernelCpuUnrestrictedMemcpy(h.orig_instr, (void *)addr, sizeof(h.orig_instr));
    }
    h.patch_instr[0] = 0xf000f8df;
  } else {
    h.addr = addr;
    kuKernelCpuUnrestrictedMemcpy(h.orig_instr, (void *)addr, sizeof(h.orig_instr));
    h.patch_instr[0] = 0xe51ff004;  // ldr pc, [pc, #-4]
  }
  h.patch_instr[1] = dst;
  kuKernelCpuUnrestrictedMemcpy((void *)addr, h.patch_instr, sizeof(h.patch_instr));
  kuKernelFlushCaches((void *)h.addr, 12);
  return h;
}

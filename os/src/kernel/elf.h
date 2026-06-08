#ifndef ELF_H
#define ELF_H

#include "types.h"

struct process_t;

#define ELF_MAGIC       0x464C457Fu
#define ELF_64          2
#define ELF_LSB         1
#define ELF_EXEC        2
#define ELF_DYN         3

typedef struct {
    uint32_t  magic;
    uint8_t   cls;
    uint8_t   data;
    uint8_t   version;
    uint8_t   osabi;
    uint8_t   abiver;
    uint8_t   pad[7];
    uint16_t  type;
    uint16_t  machine;
    uint32_t  version2;
    uint64_t  entry;
    uint64_t  phoff;
    uint64_t  shoff;
    uint32_t  flags;
    uint16_t  ehsize;
    uint16_t  phentsize;
    uint16_t  phnum;
    uint16_t  shentsize;
    uint16_t  shnum;
    uint16_t  shstrndx;
} __attribute__((packed)) elf64_hdr_t;

#define PT_NULL         0
#define PT_LOAD         1
#define PT_DYNAMIC      2
#define PT_INTERP       3
#define PT_PHDR         6

typedef struct {
    uint32_t  type;
    uint32_t  flags;
    uint64_t  offset;
    uint64_t  vaddr;
    uint64_t  paddr;
    uint64_t  filesz;
    uint64_t  memsz;
    uint64_t  align;
} __attribute__((packed)) elf64_phdr_t;

#define PF_X    1
#define PF_W    2
#define PF_R    4

/* Dynamic section tags */
#define DT_NULL         0
#define DT_NEEDED       1
#define DT_PLTRELSZ     2
#define DT_PLTGOT       3
#define DT_STRTAB       5
#define DT_SYMTAB       6
#define DT_RELA         7
#define DT_RELASZ       8
#define DT_RELAENT      9
#define DT_STRSZ        10
#define DT_SYMENT       11
#define DT_INIT         12
#define DT_FINI         13
#define DT_SONAME       14
#define DT_RPATH        15
#define DT_DEBUG        21
#define DT_PLTREL       20
#define DT_JMPREL       23
#define DT_INIT_ARRAY   25
#define DT_FINI_ARRAY   26
#define DT_INIT_ARRAYSZ 27
#define DT_FINI_ARRAYSZ 28

typedef struct {
    int64_t  d_tag;
    uint64_t d_val;
} __attribute__((packed)) elf64_dyn_t;

/* Symbol table entry */
typedef struct {
    uint32_t st_name;
    uint8_t  st_info;
    uint8_t  st_other;
    uint16_t st_shndx;
    uint64_t st_value;
    uint64_t st_size;
} __attribute__((packed)) elf64_sym_t;

#define ELF64_ST_TYPE(i)  ((i) & 0xf)
#define ELF64_ST_BIND(i)  ((i) >> 4)
#define STT_NOTYPE   0
#define STT_OBJECT   1
#define STT_FUNC     2
#define STT_SECTION  3
#define STB_GLOBAL   1
#define STB_WEAK     2

/* Relocation entry (RELA) */
typedef struct {
    uint64_t r_offset;
    uint64_t r_info;
    int64_t  r_addend;
} __attribute__((packed)) elf64_rela_t;

#define ELF64_R_SYM(i)  ((i) >> 32)
#define ELF64_R_TYPE(i) ((i) & 0xffffffff)

/* x86-64 relocation types */
#define R_X86_64_NONE       0
#define R_X86_64_64         1
#define R_X86_64_PC32       2
#define R_X86_64_GLOB_DAT   6
#define R_X86_64_JUMP_SLOT  7
#define R_X86_64_RELATIVE   8

err_t elf_load(struct process_t* proc, const void* elf_data, size_t elf_len);

/* Load ELF segments at a fixed base address (for interpreter) */
err_t elf_load_fixed(struct process_t* proc, const void* elf_data, size_t elf_len, uint64_t base);

/* Compute the virtual address of program headers for a loaded ELF */
uint64_t elf_phdr_vaddr(const void* elf_data, uint64_t base);

/* Info about a loaded ELF (for AT_PHDR / AT_ENTRY / AT_BASE) */
typedef struct {
    uint64_t base;
    uint64_t entry;
    uint64_t phdr_vaddr;
    uint16_t phnum;
    uint16_t phentsize;
} elf_load_info_t;

#endif

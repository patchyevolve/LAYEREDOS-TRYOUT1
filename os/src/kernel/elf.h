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

err_t elf_load(struct process_t* proc, const void* elf_data, size_t elf_len);

#endif

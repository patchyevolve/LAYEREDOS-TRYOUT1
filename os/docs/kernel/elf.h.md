# elf.h — ELF64 Type Definitions and Loader Interface

**Path:** `os/src/kernel/elf.h`  
**Layer:** Layer 3 (Process Lifecycle) — header

---

## Purpose

Declares the ELF64 on-disk data structures and the single public function
`elf_load`.  Included by `process.c` and `shell.c` (`elfload` command).

---

## Magic and validation constants

```c
#define ELF_MAGIC   0x464C457F  // "\x7FELF" little-endian uint32
#define ELF_64      2           // EI_CLASS: 64-bit
#define ELF_LSB     1           // EI_DATA: little-endian
#define ELF_EXEC    2           // e_type: executable
```

---

## `elf64_hdr_t` — ELF file header

```c
typedef struct {
    uint32_t  magic;        // 0x464C457F (\x7FELF)
    uint8_t   cls;          // 1=32-bit, 2=64-bit
    uint8_t   data;         // 1=LSB, 2=MSB
    uint8_t   version;      // ELF version (must be 1)
    uint8_t   osabi;        // 0=System V (used here)
    uint8_t   abiver;
    uint8_t   pad[7];
    uint16_t  type;         // ET_EXEC=2, ET_DYN=3
    uint16_t  machine;      // EM_X86_64=62
    uint32_t  version2;     // must be 1
    uint64_t  entry;        // virtual entry point address
    uint64_t  phoff;        // file offset of program header table
    uint64_t  shoff;        // file offset of section header table (unused)
    uint32_t  flags;        // processor-specific flags (0 for x86-64)
    uint16_t  ehsize;       // size of this header (64 bytes)
    uint16_t  phentsize;    // size of one program header entry (56 bytes)
    uint16_t  phnum;        // number of program header entries
    uint16_t  shentsize;    // size of one section header entry
    uint16_t  shnum;        // number of section header entries (unused)
    uint16_t  shstrndx;     // section name string table index (unused)
} __attribute__((packed)) elf64_hdr_t;
```

---

## Program header types

```c
#define PT_NULL    0    // unused entry
#define PT_LOAD    1    // loadable segment — the only type elf_load handles
#define PT_DYNAMIC 2    // dynamic linking info (not supported)
#define PT_INTERP  3    // path to dynamic linker (not supported)
#define PT_PHDR    6    // location of program header table itself
```

---

## `elf64_phdr_t` — program header entry

```c
typedef struct {
    uint32_t  type;    // PT_LOAD etc.
    uint32_t  flags;   // PF_X|PF_W|PF_R
    uint64_t  offset;  // file offset of segment data
    uint64_t  vaddr;   // desired virtual address
    uint64_t  paddr;   // physical address (ignored, we use vaddr)
    uint64_t  filesz;  // bytes to copy from file
    uint64_t  memsz;   // total size in memory (≥ filesz; BSS = memsz - filesz)
    uint64_t  align;   // alignment requirement (elf_load ignores, uses PAGE_SIZE)
} __attribute__((packed)) elf64_phdr_t;
```

---

## Segment permission flags

```c
#define PF_X  1   // executable
#define PF_W  2   // writable
#define PF_R  4   // readable
```

---

## API

```c
err_t elf_load(struct process_t* proc, const void* elf_data, size_t elf_len);
```

Maps all `PT_LOAD` segments into `proc->cr3` and sets `proc->entry_point`.
Returns `ERR_OK` on success, `ERR_INVAL` for a malformed ELF, `ERR_NOMEM`
on physical memory exhaustion.

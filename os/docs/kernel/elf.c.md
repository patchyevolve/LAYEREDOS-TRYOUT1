# elf.c — ELF64 Loader

**Path:** `os/src/kernel/elf.c`  
**Layer:** Layer 3 (Process Lifecycle)

---

## Purpose

Parses an ELF64 executable image in memory and maps its loadable segments
into a process's virtual address space.  Called by `process_exec` after
the raw ELF bytes have been read from VFS (or provided directly for the
embedded test binary).

---

## What is an ELF file?

ELF (Executable and Linkable Format) is the standard binary format for
Linux and this OS.  The relevant parts for loading are:

- **ELF header** (`elf64_hdr_t`): magic number, entry point, program header
  table offset and count.
- **Program headers** (`elf64_phdr_t`): each `PT_LOAD` entry describes one
  segment — virtual address, file offset, file size, memory size, flags
  (read/write/execute).

`memsz` ≥ `filesz` always.  The difference (`memsz - filesz`) is the BSS
area — zeroed pages that have no backing data in the file (e.g., uninitialized
globals).

---

## `elf_load(proc, elf_data, elf_len)`

### Validation

1. `elf_len < sizeof(elf64_hdr_t)` → `ERR_INVAL`
2. `hdr->magic != 0x464C457F` (`\x7FELF`) → `ERR_INVAL`
3. `hdr->cls != ELF_64` (not 64-bit) → `ERR_INVAL`
4. `hdr->type != ELF_EXEC` (not executable) → `ERR_INVAL`
5. Program header table extends beyond `elf_len` → `ERR_INVAL`

### Segment mapping

Iterates all program headers.  For each `PT_LOAD` header, calls
`elf_map_segment`.

After all segments: sets `proc->entry_point = hdr->entry`.

---

## `elf_map_segment` (internal)

Maps one ELF segment into the process's page table (`proc->cr3`):

```
Page flags:
  PAGE_USER always set
  PAGE_WRITE if PF_W flag set
  PAGE_NX if PF_X flag NOT set (data pages are non-executable)

For each 4 KB page in [vaddr & ~PAGE_MASK .. vaddr + memsz]:
  1. pmm_alloc_page()  → get a physical page
  2. kmemset(PHYS_TO_VIRT(phys), 0, PAGE_SIZE)  → zero it (covers BSS)
  3. Compute overlap between [page..page+4096] and [vaddr..vaddr+filesz]
  4. kmemcpy() the file bytes into the correct offset in the page
  5. vmm_map_page(proc->cr3, page_vaddr, phys, flags)
```

**Why page-by-page?**  
Segments are not always page-aligned in the file.  A single 4 KB page
may contain both the end of one segment and the beginning of BSS zero-fill.
The overlap calculation handles this precisely: it only copies bytes in
the `[vaddr, vaddr+filesz)` range, leaving the rest zeroed.

---

## Page flag mapping

| ELF flags | Page flags set |
|-----------|---------------|
| `PF_R` only | `PAGE_USER \| PAGE_NX` |
| `PF_R \| PF_W` | `PAGE_USER \| PAGE_WRITE \| PAGE_NX` |
| `PF_R \| PF_X` | `PAGE_USER` (NX NOT set) |
| `PF_R \| PF_W \| PF_X` | `PAGE_USER \| PAGE_WRITE` |

Setting `PAGE_NX` on data pages prevents code injection into writable memory.

---

## Error handling

If `pmm_alloc_page` returns 0 (OOM) or `vmm_map_page` fails, the partially-
mapped segment leaks its allocated pages (they are not freed).  This is
acceptable for now — process creation failure is followed by process
destruction which would call `vmm_free_user_pages` to reclaim everything.

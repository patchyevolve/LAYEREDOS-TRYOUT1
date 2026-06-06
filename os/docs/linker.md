# linker.ld — Linker Script

**Path:** `os/linker.ld`  
**Layer:** Build tooling

---

## Purpose

Controls exactly where every section of the kernel ELF is placed in both
**physical** and **virtual** memory, implementing the higher-half kernel
layout.

---

## Address constants

| Symbol | Value | Meaning |
|--------|-------|---------|
| `KERNEL_PHYS_BASE` | `0x100000` (1 MB) | Physical address where the kernel is loaded by the multiboot loader |
| `KERNEL_VMA_BASE` | `0xFFFFFFFFC0000000` | Virtual address offset added to all kernel symbols; kernel runs in the top 1 GB of the 64-bit address space |

---

## Section layout

```
Physical 0x100000
│
├── .multiboot      ← multiboot header must be in first 8 KB of the file
├── .boot_text      ← 32-bit startup code (before paging is enabled)
├── .boot_data      ← boot-time page tables, GDT, saved magic/mb_info
├── .boot_notes     ← Xen PVH start note (optional)
│
│   [gap of KERNEL_VMA_BASE bytes is added to the location counter]
│   This is NOT actual bytes in the file — it's a VMA offset.
│   The AT() directive on each higher section provides the actual
│   Load Memory Address (physical), so the ELF LOAD segment points
│   to the right physical bytes even though the VMA is 0xFFFF...
│
├── .text           VMA 0xFFFFFFFFC0100000+, LMA 0x100000+
├── .rodata         follows .text
├── .data           follows .rodata
└── .bss            follows .data   (zero-filled, no file bytes)
    _kernel_end     = end of .bss (VMA)
    _kernel_end_phys = _kernel_end - KERNEL_VMA_BASE  (used by PMM)
```

---

## Why a higher-half layout?

Running the kernel at a very high virtual address (`0xFFFF...`) means:

1. The entire 4 GB user virtual address space (`0x00000000`–`0xFFFFFFFF`)
   is free for user processes without any conflict with kernel text.
2. Kernel pointers are immediately distinguishable from user pointers
   (top bit set).
3. Identity-map of physical RAM at `PHYS_TO_VIRT(phys) = phys + KERNEL_VMA_BASE`
   lets the kernel access any physical address with a simple addition.

---

## The `AT()` directive

Without `AT()`, the linker would place the LMA (physical location) of
`.text` at `KERNEL_PHYS_BASE + KERNEL_VMA_BASE` — roughly 0xFFFFFFFFC0100000
physically, which doesn't exist.  `AT(ADDR(.text) - KERNEL_VMA_BASE)` tells
the linker "load this section from physical address VMA minus the offset",
so QEMU's `-kernel` loader puts bytes at 0x100000 in RAM.

---

## Exported symbols (used by C code)

| Symbol | Used by |
|--------|---------|
| `_text_start`, `_text_end` | Informational; kernel.h declares them |
| `_rodata_start`, `_rodata_end` | Informational |
| `_data_start`, `_data_end` | Informational |
| `_bss_start`, `_bss_end` | Informational |
| `_kernel_end` | VMM: upper bound of kernel region (VMA) |
| `_kernel_end_phys` | PMM: `pmm_mark_region_used(0x100000, &_kernel_end_phys)` |

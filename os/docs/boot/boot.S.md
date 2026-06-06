# boot.S — Multiboot Entry, Paging, Long-Mode Jump

**Path:** `os/src/boot/boot.S`  
**Layer:** Layer 0→1 boundary (hardware bring-up)  
**Language:** x86 AT&T assembly (32-bit entry, transitions to 64-bit)

---

## Purpose

This is the very first code that runs when QEMU hands control to the kernel.
Its job is to:

1. Accept the multiboot magic number and info pointer from the bootloader.
2. Verify the CPU supports 64-bit long mode (CPUID check).
3. Build a minimal set of 4-level page tables that:
   - Identity-map the first 512 MB of physical RAM (so boot code can keep running at its current physical addresses right after enabling paging).
   - Map the same 512 MB at the kernel's higher-half virtual base (`0xFFFFFFFFC0000000`) so the C kernel can run at its link-time addresses.
4. Enable PAE, load CR3, set the LME bit in EFER, enable paging.
5. Do a far jump to 64-bit code.
6. Set up the final stack pointer using the higher-half virtual address of `stack_top`.
7. Call `kmain(magic, mb_info)`.

---

## Sections used

| Section | Flags | Why |
|---------|-------|-----|
| `.note.pvh` | allocatable | Xen PVH start note — allows the kernel to boot directly under Xen without a BIOS stage |
| `.boot.data` | alloc + write | Page tables, GDT, descriptor, saved registers — all needed before `.bss` is accessible |
| `.bss` | zero-fill | Kernel stack lives here; allocated by the linker, zero-filled at load time |
| `.boot.text` | alloc + exec | All 32-bit startup code |

---

## Xen PVH note (`.note.pvh`)

The four-field note structure at the top of the file contains the physical
address of `start` and identifies the kernel to Xen's PVH loader as a
direct-boot target.  QEMU's `-kernel` flag ignores it; it only matters when
booting under Xen.

---

## Boot-time page tables (`.boot.data`)

Three static 4 KB pages, placed at **fixed physical addresses** by the linker:

| Symbol | Physical addr | Role |
|--------|---------------|------|
| `pml4_table` | `0x7000` | PML4 — top-level 4-level paging structure |
| `pdpt_table` | `0x8000` | PDPT — one table shared by both mappings |
| `pd_table`   | `0x9000` | PD — 512 entries of 2 MB huge pages |

**Two mappings are installed:**

```
PML4[0]   → pdpt_table        (identity: VA 0x0…  → PA 0x0…)
PML4[511] → pdpt_table        (higher half: VA 0xFFFFFFFFC0000000… → PA 0x0…)

PDPT[0]   → pd_table          (covers first 1 GB of each mapping)
PDPT[511] → pd_table          (mirrors for higher-half PDPT entry)

PD[0..255] → 2 MB huge pages  (256 entries × 2 MB = 512 MB physical RAM)
```

Each PD entry uses flags `0x83` = Present + Write + Huge (PS bit).

**Why only 256 entries (512 MB)?**  
The boot code uses `movl $256, %ecx` as the loop counter.  This covers
exactly 512 MB.  The kernel's `.bss` (including IST stacks and TSS stack)
must fit inside this range, which it does for the current binary size.
If the kernel grows past 512 MB physical, this must be increased to 512
entries (covering 1 GB).

---

## GDT (`.boot.data`)

A minimal 3-entry GDT used only to do the far jump into 64-bit mode:

| Index | Descriptor | Notes |
|-------|-----------|-------|
| 0 | Null | Required |
| 1 | `0x0020980000000000` | 64-bit kernel code (L=1, P=1, DPL=0) |
| 2 | `0x0000920000000000` | 64-bit kernel data (P=1, DPL=0, writable) |

This GDT is **temporary** — `hal_init()` calls `gdt_init()` which builds a
proper 7-entry GDT including user-mode segments and a TSS descriptor.

---

## Long-mode transition sequence

```
1. Save EAX (multiboot magic) and EBX (mb_info ptr) to .boot.data
2. Set up a 32-bit stack at (stack_top - KERNEL_VMA_BASE)
3. CPUID leaf 0 → check leaf count ≥ 0x80000001
4. CPUID leaf 0x80000001, EDX bit 29 (LM bit) → abort if not set
5. Zero 0x3000 bytes at PML4_ADDR (clears all three page tables)
6. Fill PML4, PDPT, PD entries as described above
7. Set CR4.PAE (bit 5)
8. Load CR3 = PML4_ADDR (0x7000)
9. Set EFER.LME (bit 8) via MSR 0xC0000080
10. Set CR0.PG (bit 31) — paging ON, CPU is now in IA-32e compatibility mode
11. lgdt from gdt_descriptor (still uses physical address — paging is on
    but we're still executing at physical addresses so identity map works)
12. ljmp $0x08, $1f  — far jump loads CS with 64-bit code descriptor,
    CPU switches to 64-bit long mode
13. Load DS/ES/SS with 0x10 (data descriptor)
14. movabsq $stack_top, %rsp  — switch to higher-half virtual stack
15. Load saved magic and mb_info from .boot.data into RDI, RSI
16. movabsq $kmain, %rax; call *%rax  — call the C kernel
```

---

## Error paths

Both `.no_cpuid` and `.no_long_mode` write ASCII characters to COM1 port
(`0x3F8`) via `outb` and hang in a `cli; hlt` loop.  There is no graphical
error message because the display is not yet initialised.

- `.no_cpuid` → sends `'E'`, `'r'`, `'r'`
- `.no_long_mode` → sends `'N'`, `'O'`

---

## Handoff to C

`kmain` receives:
- `RDI` = multiboot magic (should be `0x2BADB002` for multiboot1, or
  `0x33687578` for Xen PVH which `main.c` zeroes out)
- `RSI` = physical address of the multiboot info structure

At this point interrupts are disabled (`cli` was done at entry), paging
is active with the identity + higher-half dual mapping, and the stack
is valid.

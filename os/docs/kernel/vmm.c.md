# vmm.c — Virtual Memory Manager

**Path:** `os/src/kernel/vmm.c`  
**Layer:** Layer 4 (Memory)

---

## Purpose

Manages the x86-64 4-level page table hierarchy.  Provides functions to
map and unmap individual 4 KB pages in any PML4's virtual address space,
allocate new page-table pages from the PMM, and flush TLB entries.

---

## Page table hierarchy

x86-64 uses four levels of translation for 4 KB pages:

```
Virtual address bits:
  [63:48] sign extension (must match bit 47)
  [47:39] PML4 index  (9 bits → 512 entries)
  [38:30] PDPT index  (9 bits → 512 entries)
  [29:21] PD index    (9 bits → 512 entries)
  [20:12] PT index    (9 bits → 512 entries)
  [11:0]  Page offset (12 bits → 4 KB)
```

Index extraction macros:
```c
PML4_INDEX(v)  = (v >> 39) & 0x1FF
PDPT_INDEX(v)  = (v >> 30) & 0x1FF
PD_INDEX(v)    = (v >> 21) & 0x1FF
PT_INDEX(v)    = (v >> 12) & 0x1FF
```

Each level's entry is a 64-bit `page_entry_t` (alias for `uint64_t`):
- Bits [11:0]: flags (Present, Write, User, Huge, NX, etc.)
- Bits [51:12]: physical address of the next-level table (or page frame)

---

## `vmm_alloc_page_table`

```c
uint64_t vmm_alloc_page_table(void)
```

Allocates one physical page from the PMM and zeroes it via `PHYS_TO_VIRT`.
Used to create new PDPT, PD, and PT pages on demand when a virtual address
is mapped for the first time.

Returns the **physical address** of the new table, or 0 on OOM.

---

## `get_entry` — page-table walker with creation

Internal function that walks from PML4 down `level` levels and returns a
pointer to the final entry.  If `create=1`, missing intermediate tables are
allocated and installed with `PAGE_PRESENT | PAGE_WRITE | PAGE_USER` flags.

This is the core of `vmm_map_page` — it walks to level 4 (the PT entry)
creating tables as needed.

---

## `walk_pagetable` — read-only walker

Walks all four levels without creating anything.  Returns a pointer to the
PT entry for a given virtual address, or `NULL` if any level is not present.
Handles huge pages at PDPT (1 GB) and PD (2 MB) levels by returning early.

Used by `vmm_unmap_page`.

---

## `vmm_map_page`

```c
err_t vmm_map_page(uint64_t pml4_phys, uint64_t virt, uint64_t phys, uint64_t flags)
```

Maps one 4 KB page:

1. Validates that `virt` and `phys` are page-aligned.
2. Calls `get_entry(pml4_phys, virt, 4, create=1)` to get the PT entry.
3. Writes `(phys & ~0xFFF) | (flags & 0xFFF) | PAGE_PRESENT`.
4. Calls `vmm_flush_tlb_page(virt)` to invalidate the cached translation.

`flags` should be a combination of: `PAGE_WRITE`, `PAGE_USER`, `PAGE_HUGE`,
`PAGE_NX`.  `PAGE_PRESENT` is always added unconditionally.

---

## `vmm_unmap_page`

Walks to the PT entry and writes 0 to it, then flushes the TLB.  Does not
free the physical page — that is the caller's responsibility.

---

## `vmm_flush_tlb_page`

```c
asm volatile("invlpg (%0)" : : "r"(virt) : "memory");
```

Invalidates the single TLB entry for the given virtual address.  Must be
called after any page table modification to ensure the CPU uses the new
translation.

---

## `vmm_init`

Reads CR3 to find the current PML4 physical address (the boot page tables
set up by `boot.S`).  Stores it in `kernel_pml4`.  Prints the kernel's
physical extent for diagnostic purposes.  Does not modify any page tables —
the boot tables remain in place.

---

## Important: intermediate table flags must include `PAGE_USER`

When `get_entry` creates a new intermediate table (PDPT, PD, or PT page),
it sets `PAGE_USER` on the entry.  This is required because the CPU checks
the `U/S` bit at **every level** during a ring-3 access.  If any level is
missing `PAGE_USER`, user-mode accesses to that range will fault with
error code `0x5` (protection violation) even if the final PT entry has
`PAGE_USER` set.

This is why `thread_create_user` manually ORs `PAGE_USER` into PML4[0]
and PDPT[0] after calling `vmm_map_page` — the boot PML4/PDPT entries
were created without `PAGE_USER` because they were initially kernel-only.

---

## `PHYS_TO_VIRT` usage

All accesses to page table entries in C go through `PHYS_TO_VIRT(phys_addr)`.
The CPU stores physical addresses in page table entries, but the kernel's
C code can only dereference virtual addresses.  The identity + higher-half
mapping set up by `boot.S` ensures that for any physical address P in the
first 512 MB, `PHYS_TO_VIRT(P)` is a valid virtual address.

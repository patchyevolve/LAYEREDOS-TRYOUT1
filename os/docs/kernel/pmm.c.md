# pmm.c — Physical Memory Manager

**Path:** `os/src/kernel/pmm.c`  
**Layer:** Layer 4 (Memory)

---

## Purpose

Tracks which 4 KB physical pages are free and which are in use.  Provides
`pmm_alloc_page` / `pmm_free_page` as the lowest-level memory allocation
primitive that all other allocators (VMM, slab, thread stacks) build on.

---

## Data structures

Two parallel tracking structures are maintained:

### Free list

A singly-linked list of free pages.  Each free page stores a pointer to the
next free page **in the first 8 bytes of the page itself** (the page is not
in use, so its content can be overwritten):

```c
typedef struct free_page { struct free_page* next; } free_page_t;
static free_page_t* free_list;
```

Fast O(1) alloc (pop from head) and free (push to head).

### Bitmap

A byte array at physical address `0x10000` (`bitmap_base`), one bit per
4 KB page.  Bit N = 1 means page N is allocated; bit N = 0 means free.
Size: `total_pages / 8` bytes = 16 KB for 512 MB RAM.

The bitmap exists as a **second source of truth** alongside the free list.
It enables the `pmm_alloc_pages` contiguous scan and provides double-free
detection in `pmm_free_page`.

---

## Initialisation (`pmm_init`)

1. Calculates `total_page_count = mem_size / PAGE_SIZE`.
2. Places the bitmap at physical address `0x10000` (4 pages reserved in
   the memory layout).  Sets all bits to 0xFF (all used) first, then clears
   all bits (all free), then marks specific reserved regions as used:
   - `0x0000–0x1000` — real-mode interrupt vector table
   - `0x7000–0xB000` — boot page tables (PML4/PDPT/PD)
   - `0x10000–bitmap_end` — the bitmap itself
   - `mem_size-0x1000–mem_size` — last page (guard)
   - `0x100000–_kernel_end_phys` — the kernel binary
3. Calls `parse_mb_mmap` which iterates the multiboot memory map and calls
   `add_region_to_free_list` for every usable (`type==1`) region.

`add_region_to_free_list` walks a physical range in PAGE_SIZE steps and
pushes each non-bitmap-reserved page onto the free list.

---

## `pmm_alloc_page`

```
cli (hal_save_irq)
if free_list == NULL → return 0 (OOM)
page = free_list; free_list = page->next; free_page_count--
bitmap_set(page / PAGE_SIZE)
sti (hal_restore_irq)
kmemset(page, 0, PAGE_SIZE)   ← zero AFTER releasing lock
return page
```

Returns a **virtual address** equal to the physical address because the
kernel's higher-half mapping uses `PHYS_TO_VIRT(phys) = phys + VMA_BASE`,
but the PMM itself only deals in physical addresses.  Callers that need to
write to the page use `PHYS_TO_VIRT(addr)`.

Zeroing happens **outside** the critical section to keep the lock short.
This is safe because the page has been removed from the free list before
zeroing, so no other allocator can claim it.

---

## `pmm_alloc_pages(count)`

Scans the bitmap for `count` **contiguous** free pages.  Because this is
harder to make lock-free, the entire scan+mark is wrapped in `hal_save_irq`:

1. Scan bitmap for first run of `count` zero bits → record `first` page index.
2. For each page in the run: `bitmap_set(first+j)`, `free_page_count--`,
   remove from free_list (O(n) linear search — known performance issue).
3. Zero all pages outside the lock.

The linear free-list removal is the main weakness of `pmm_alloc_pages`.
For the current use case (kernel stack allocation at boot time) it is
acceptable.

---

## `pmm_free_page(phys_addr)`

Guards against:
- `phys_addr == 0` (NULL)
- Misaligned addresses
- Out-of-range addresses
- Double-free: `!bitmap_test(idx)` → already free → silently return

On success: `bitmap_clear`, `free_page_count++`, push page back onto free list.

---

## Interrupt safety

All mutating functions (`alloc_page`, `alloc_pages`, `free_page`,
`mark_region_used`) use `hal_save_irq` / `hal_restore_irq` to disable
interrupts.  This prevents the timer ISR from calling the watchdog (which
may call PMM) while a PMM operation is in progress.

---

## Known issues (from AUDIT.md)

### C9 — TOCTOU in `pmm_alloc_pages`
Scan and allocation are now under the same lock, so the race is closed.

### C1 — Unsynchronised PMM (original)
All operations are now wrapped in `hal_save_irq` / `hal_restore_irq`.

### Bitmap/free-list desync
If `pmm_alloc_pages` removes pages from the free list by linear search, it
can miss pages that are in the list but appear in a non-standard order.
The bitmap becomes authoritative; the free list may still contain those
addresses but they will be re-blocked by the bitmap test in `pmm_free_page`.

---

## Statistics

```c
uint64_t pmm_total_pages(void);
uint64_t pmm_free_pages_count(void);
uint64_t pmm_used_pages(void);
```

Read by the watchdog health check (OOM detection) and the shell `meminfo`
command.

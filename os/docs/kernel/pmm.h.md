# pmm.h — Physical Memory Manager Interface

**Path:** `os/src/kernel/pmm.h`  
**Layer:** Layer 4 (Memory) — header

---

## Purpose

Declares the public API surface of the PMM.  Included by VMM, the slab
allocator, the scheduler (thread stack allocation), and the watchdog.

---

## API summary

| Function | Returns | Description |
|----------|---------|-------------|
| `pmm_init(mem_size, mb_info)` | `err_t` | Initialise bitmap and free list from multiboot map |
| `pmm_alloc_page()` | `uint64_t` (phys addr or 0) | Allocate one 4 KB page |
| `pmm_free_page(phys)` | `void` | Free one page; ignores double-free |
| `pmm_alloc_pages(count)` | `uint64_t` (phys addr or 0) | Allocate `count` contiguous pages |
| `pmm_free_pages(phys, count)` | `void` | Free a contiguous range |
| `pmm_total_pages()` | `uint64_t` | Total physical pages managed |
| `pmm_free_pages_count()` | `uint64_t` | Currently free pages |
| `pmm_used_pages()` | `uint64_t` | total − free |
| `pmm_mark_region_used(start, end)` | `void` | Reserve a physical range (used during init) |

---

## Return value convention

`pmm_alloc_page` and `pmm_alloc_pages` return **physical addresses**.
A return value of `0` means allocation failed (OOM).  Physical address 0
(the first page) is reserved and never returned by the allocator, so 0
is unambiguously an error sentinel.

Callers that need to write to the allocated page must convert:
```c
uint64_t phys = pmm_alloc_page();
if (!phys) { /* OOM */ }
void* virt = (void*)PHYS_TO_VIRT(phys);
```

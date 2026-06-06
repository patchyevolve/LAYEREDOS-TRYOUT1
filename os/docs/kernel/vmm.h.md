# vmm.h — Virtual Memory Manager Interface

**Path:** `os/src/kernel/vmm.h`  
**Layer:** Layer 4 (Memory) — header

---

## Purpose

Declares page-flag constants, the `page_entry_t` type alias, and the VMM
API.  Included by `sched.c` (user thread page-table setup) and `kmalloc.c`
(PHYS_TO_VIRT conversions).

---

## Page flags

```c
PAGE_PRESENT  (bit 0)  // Entry is valid; CPU will use it
PAGE_WRITE    (bit 1)  // Region is writable
PAGE_USER     (bit 2)  // Accessible from ring 3; MUST be set at every level
PAGE_HUGE     (bit 7)  // This entry maps a 2 MB (PD) or 1 GB (PDPT) page
PAGE_NX       (bit 63) // Execute-disable (requires EFER.NXE)
```

These are combined with bitwise OR when calling `vmm_map_page`.

---

## `page_entry_t`

```c
typedef uint64_t page_entry_t;
```

All four levels of the page table use the same 64-bit entry format.
The alias documents intent without imposing a struct overhead.

---

## API summary

| Function | Description |
|----------|-------------|
| `vmm_init()` | Record boot PML4 address; no table modifications |
| `vmm_alloc_page_table()` | Allocate + zero one page-table physical page |
| `vmm_map_page(pml4, virt, phys, flags)` | Create/update a 4 KB mapping |
| `vmm_unmap_page(pml4, virt)` | Remove a mapping (does not free the page) |
| `vmm_flush_tlb_page(virt)` | `invlpg` — invalidate one TLB entry |

---

## Usage notes

- `vmm_map_page` accepts the **physical** address of the PML4 as its first
  argument.  This is the value from `%cr3` or returned by `vmm_alloc_page_table`.
- The caller is responsible for flushing the TLB after bulk modifications.
  `vmm_map_page` calls `vmm_flush_tlb_page` internally, but `vmm_unmap_page`
  does too, so single-page operations are always self-consistent.
- On SMP, TLB shootdowns (IPI to other CPUs to flush their TLBs) are not
  yet implemented.  This is safe for the current single-CPU implementation.

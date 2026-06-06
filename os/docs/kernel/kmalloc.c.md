# kmalloc.c — Slab Allocator

**Path:** `os/src/kernel/kmalloc.c`  
**Layer:** Layer 4 (Memory)

---

## Purpose

Provides a general-purpose kernel heap allocator on top of the PMM.  Uses
a slab strategy for small objects (≤ 1024 bytes) to avoid fragmentation and
per-allocation page waste, and falls back to multi-page PMM allocations for
large requests.

---

## Constants

| Constant | Value | Meaning |
|----------|-------|---------|
| `SLAB_MAGIC` | `0x5B1AB5` | Canary written into every slab header |
| `SLAB_MAX_OBJS` | 128 | Maximum objects per slab page |
| `MIN_SLAB_SHIFT` | 4 | Minimum object size = 2^4 = 16 bytes |
| `MAX_SLAB_SHIFT` | 10 | Maximum object size = 2^10 = 1024 bytes |
| `SLAB_COUNT` | 7 | Number of size classes: 16, 32, 64, 128, 256, 512, 1024 |

---

## Slab page layout

Each 4 KB physical page used by the slab allocator is structured as:

```
[0]      slab_page_t header (next*, magic, free_count, total, shift)
[header] bitmap (SLAB_BITMAP_BYTES = 16 bytes for 128 objects)
[header+bitmap] object array (total × objsize bytes)
```

The `bitmap` uses **1 = free, 0 = used** convention.  `kmalloc` scans for
a set bit (free slot), clears it, and returns `base + i*objsize`.

`kfree` computes the object index from the pointer, validates it, and sets
the bit back.

### Important: bitmap initialisation

After `kmemset(virt, 0, PAGE_SIZE)` zeros the whole page, the bitmap is
also zero (all bits 0 = all objects used).  `slab_new_page` initialises
`free_count = page->total` but the bitmap remains zero.

`kmalloc` then cannot find a free slot in the bitmap loop (no set bits).
It exits the loop, calls `slab_new_page` again for a new page — but for
the **very first allocation** from a new page, `kmalloc` has a fast path:

```c
slab_page_t* np = slab_new_page(idx);
uint8_t* bitmap = ((uint8_t*)np) + sizeof(slab_page_t);
bitmap[0] &= ~1;   // clear bit 0 = mark slot 0 as used
np->free_count--;
return base;       // return slot 0 directly
```

This gives slot 0 correctly.  Subsequent allocations from the same page
will again find no free bits (bitmap is still all-zero for slots 1–127).
The allocator will keep creating new slab pages, wasting memory.

**The fix** is to set `bitmap[0..SLAB_BITMAP_BYTES-1] = 0xFF` in
`slab_new_page` after zeroing the page, marking all objects as free.

---

## Size class selection

`slab_index(size)` rounds `size` up to the next power of two ≥ 16 and
returns the index (0 = 16 bytes, 1 = 32, ..., 6 = 1024).  For sizes > 1024
it returns -1, triggering the large-allocation path.

---

## Large allocations

For requests > 1024 bytes, `kmalloc` calls `pmm_alloc_pages(pages)` where
`pages = ceil((size + 8) / PAGE_SIZE)`.  It stores the page count in the
first 8 bytes of the allocation:

```
[phys+0]  uint64_t pages  (size header)
[phys+8]  user data
```

`kfree` distinguishes large from slab allocations by reading the `slab_page_t`
at the page start and checking `magic != SLAB_MAGIC`.  If the magic is wrong,
it treats the page as a large allocation and calls `pmm_free_pages`.

---

## `kfree` slab validation

```c
uint64_t page_start = addr & ~(PAGE_SIZE - 1);
slab_page_t* page = (slab_page_t*)page_start;
if (page->magic != SLAB_MAGIC) { /* large alloc path */ }
```

For slab objects:
- Computes `idx = (ptr - base) / objsize`
- Bounds-checks `idx`
- Checks the bit is currently 0 (used) before setting it — prevents
  silent double-frees

---

## Statistics

```c
size_t kmalloc_used(void);   // bytes currently in use by callers
size_t kmalloc_total(void);  // bytes claimed from PMM for slab pages
```

Tracking is approximate — `kmalloc_bytes_total` includes slab page overhead
(header + bitmap).

---

## Thread safety

`kmalloc` and `kfree` have **no internal locking**.  Callers sharing the
heap across threads must hold an external lock or disable interrupts.  The
current kernel usage is mostly single-threaded at init time, with the shell
and idle thread not concurrently calling kmalloc, so this is acceptable for
now.

---

## Known issues (from AUDIT.md)

| Issue | Description |
|-------|-------------|
| Bitmap init bug | After zeroing the page, all bitmap bits are 0 (used). `slab_new_page` should set bytes to `0xFF` after zeroing. The current workaround (fast path for slot 0) means every subsequent slot needs a new slab page — functional but extremely wasteful. |
| No realloc | Not implemented. |
| No per-CPU caching | Single global slab list; would need spinlocks for SMP. |

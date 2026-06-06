# kmalloc.h — Kernel Heap Allocator Interface

**Path:** `os/src/include/kmalloc.h`  
**Layer:** Layer 4 (Memory)

---

## Purpose

Declares the public API of the kernel slab allocator.  Any kernel subsystem
that needs variable-size heap allocation includes this header.

---

## API

```c
void   kmalloc_init(void);
void*  kmalloc(size_t size);
void   kfree(void* ptr);
size_t kmalloc_used(void);
size_t kmalloc_total(void);
```

### `kmalloc_init`
Called once from `kmain` after PMM and VMM are up.  Zeroes the slab pointer
array.  Must be called before any `kmalloc`.

### `kmalloc(size_t size)`
Returns a pointer to at least `size` bytes of kernel virtual memory, or
`NULL` on OOM.  The memory is zero-initialised (pages come from `pmm_alloc_page`
which zeroes them).  For sizes ≤ 1024 bytes the slab allocator is used;
for larger sizes a multi-page allocation via `pmm_alloc_pages` is used with
a size header prepended.

### `kfree(void* ptr)`
Returns memory to the slab or to the PMM.  Passing `NULL` is a no-op.
Passing a pointer not obtained from `kmalloc` is undefined behaviour (the
slab magic check will catch most cases and silently return, but there is no
guarantee).

### `kmalloc_used` / `kmalloc_total`
Byte-level accounting.  Used by the shell `meminfo` command to report heap
usage.  `total` includes the overhead of full slab pages; `used` is only
the bytes actually handed to callers.

---

## Notes for callers

- **Thread safety:** `kmalloc` and `kfree` are **not** protected by any
  lock.  Callers must ensure they are not called concurrently from different
  threads or from interrupt context without external serialisation.
- **Alignment:** returned pointers are aligned to the object size (i.e.,
  16-byte objects are 16-byte aligned).  Large allocations are page-aligned.
- **No realloc:** there is no `krealloc`.  Callers that need resizable
  buffers must allocate a new block, copy, and free the old one.

# types.h — Primitive Types, Memory Constants, Error Codes

**Path:** `os/src/include/types.h`  
**Layer:** Cross-cutting (included everywhere)

---

## Purpose

Single source of truth for every type alias, address-space constant, and
error code used across the entire kernel.  By keeping all of these in one
header, there is no risk of two files disagreeing on the size of `uint64_t`
or the value of `PAGE_SIZE`.

---

## Integer types

Because the kernel is freestanding (no `<stdint.h>` from libc), all fixed-
width types are re-declared here using GCC's guaranteed-width primitives:

| Alias | Underlying | Bits | Notes |
|-------|-----------|------|-------|
| `uint8_t` | `unsigned char` | 8 | byte; used for port I/O, bitmaps |
| `uint16_t` | `unsigned short` | 16 | port numbers, GDT/IDT fields |
| `uint32_t` | `unsigned int` | 32 | general 32-bit quantities |
| `uint64_t` | `unsigned long long` | 64 | addresses, ticks, page entries |
| `int8_t` | `signed char` | 8 | |
| `int16_t` | `signed short` | 16 | |
| `int32_t` | `signed int` | 32 | |
| `int64_t` | `signed long long` | 64 | error codes, signed offsets |
| `size_t` | `uint32_t` | 32 | matches x86-64 ILP32 ABI for size arguments |
| `ssize_t` | `int32_t` | 32 | signed size (return value of read/write) |
| `uintptr_t` | `uint64_t` | 64 | pointer-sized unsigned integer |
| `intptr_t` | `int64_t` | 64 | pointer-sized signed integer |
| `cpu_flags_t` | `uint64_t` | 64 | saved RFLAGS value (for IRQ save/restore) |

`NULL` is `((void*)0)` — explicit cast prevents implicit integer comparisons.

---

## Utility macros

```c
offsetof(T, M)        // byte offset of member M in struct T
container_of(P, T, M) // recover struct pointer from member pointer
```

`offsetof` delegates to `__builtin_offsetof` (GCC built-in, correct for
packed structs).  `container_of` is used by wait-queue and linked-list
traversal code.

---

## err_t — error code enum

All kernel functions that can fail return `err_t`.  Zero (`ERR_OK`) always
means success; negative values are errors:

| Code | Value | Meaning |
|------|-------|---------|
| `ERR_OK` | 0 | Success |
| `ERR_GENERAL` | -1 | Unspecified error |
| `ERR_NOMEM` | -2 | Physical or virtual memory exhausted |
| `ERR_INVAL` | -3 | Caller passed a bad argument |
| `ERR_BADADDR` | -4 | Pointer is unmapped or misaligned |
| `ERR_BUSY` | -5 | Resource already in use |
| `ERR_TIMEOUT` | -6 | Operation did not complete in time |
| `ERR_AGAIN` | -7 | Transient — retry later |
| `ERR_FAULT` | -8 | Hardware page fault during access |
| `ERR_NOSYS` | -9 | Syscall number not implemented |
| `ERR_PERM` | -10 | Capability / ownership check failed |
| `ERR_EXIST` | -11 | Object already exists |
| `ERR_NOENT` | -12 | Object not found |
| `ERR_IO` | -13 | Device I/O error |
| `ERR_NOSPACE` | -14 | Storage full |
| `ERR_NAMETOOLONG` | -15 | Filename exceeds limit |
| `ERR_LOOP` | -16 | Symlink loop detected |
| `ERR_STALE` | -17 | Handle refers to deleted object |
| `ERR_DEADLOCK` | -18 | Mutex/lock deadlock detected |
| `ERR_CAP` | -19 | Capability token missing or revoked |
| `ERR_BADFD` | -20 | File descriptor out of range or closed |

---

## Memory address constants

```c
KERNEL_PHYS_BASE   0x100000              // 1 MB — physical load address
KERNEL_VMA_BASE    0xFFFFFFFFC0000000    // higher-half virtual base
KERNEL_VMA         KERNEL_VMA_BASE + KERNEL_PHYS_BASE
                                         // = first kernel virtual byte

PHYS_TO_VIRT(P)    (P) + KERNEL_VMA_BASE
VIRT_TO_PHYS(V)    (V) - KERNEL_VMA_BASE
```

These two macros are used constantly to convert between the physical
addresses returned by PMM and the virtual addresses the C code actually
dereferences.  Any physical address in the first 512 MB of RAM is valid
input to `PHYS_TO_VIRT` because the boot page tables map exactly that range.

---

## Page constants

| Macro | Value | Meaning |
|-------|-------|---------|
| `PAGE_SIZE` | 4096 | Bytes per page (4 KB) |
| `PAGE_SHIFT` | 12 | log2(PAGE_SIZE) |
| `PAGE_MASK` | `0xFFFFFFFFFFFFF000` | Mask to extract page-aligned address |
| `PAGE_ALIGN(V)` | `(V + PAGE_SIZE - 1) & PAGE_MASK` | Round up to next page boundary |
| `IS_PAGE_ALIGNED(V)` | `(V & (PAGE_SIZE-1)) == 0` | Test alignment |

---

## Scheduling constants (informational)

```c
MAX_PRIORITY    255    // highest priority level
DEFAULT_PRIORITY 128  // normal thread priority
IDLE_PRIORITY   0     // idle thread
TIME_SLICE_MS   10    // default scheduler time slice
```

These mirror the `#define` values in `sched.h` and are here for reference;
the scheduler actually uses the values in `sched.h`.

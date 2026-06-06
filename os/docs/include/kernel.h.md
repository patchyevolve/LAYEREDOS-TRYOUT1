# kernel.h — Top-Level Kernel API Surface

**Path:** `os/src/include/kernel.h`  
**Layer:** Cross-cutting (included by every `.c` file)

---

## Purpose

Aggregates the minimum set of declarations needed in virtually every kernel
source file so those files only need one `#include "kernel.h"` instead of
individually including `types.h`, `errno.h`, and the klib prototypes.

It also forward-declares the linker-script symbols so any file can print
the kernel's memory layout without a separate header.

---

## What it includes

```c
#include "types.h"   // all type aliases, err_t, address macros
#include "errno.h"   // err_str() helper
```

Nothing else.  Headers for specific subsystems (`hal.h`, `sched.h`, `pmm.h`,
etc.) are included only in files that actually use those subsystems, keeping
include-graph complexity manageable.

---

## Linker-script symbol declarations

```c
extern uint64_t _text_start, _text_end;
extern uint64_t _rodata_start, _rodata_end;
extern uint64_t _data_start, _data_end;
extern uint64_t _bss_start, _bss_end, _kernel_end;
```

These are **not variables** — they are linker-generated absolute symbols.
Taking their address (`&_kernel_end`) gives the virtual address of the end
of the kernel image.  The PMM uses `_kernel_end_phys` (declared elsewhere)
to know where the kernel binary ends in physical memory.

---

## Console output functions

| Function | Description |
|----------|-------------|
| `kputchar(char c)` | Emit one character to UART and VGA |
| `kputs(const char* s)` | Emit a null-terminated string |
| `kprintf(const char* fmt, ...)` | printf-like; supports `%d %u %x %s %c %p %lu %llu` |
| `kputhex(uint64_t v)` | Always prints full 16-digit hex (with `0x` prefix) |
| `kputdec(uint64_t v, int pad)` | Decimal with zero-padding |

All output goes to both the serial port (COM1, 0x3F8) and the VGA text
buffer.  There is no buffering — every character is written immediately.

---

## String and memory utilities

| Function | Description |
|----------|-------------|
| `kstrcmp(a, b)` | Lexicographic compare; returns 0 if equal |
| `kstrncpy(d, s, n)` | Copy at most n bytes; pads with zeros like `strncpy` |
| `kmemset(d, c, n)` | Fill n bytes with value c |
| `kmemcpy(d, s, n)` | Copy n bytes from s to d (no overlap check) |

These are implemented in `klib.c` with simple byte loops — no SIMD, no
memcpy hijacking by the compiler (the `-fno-builtin` effect of `-ffreestanding`
prevents the compiler from replacing these with inline SIMD).

---

## kpanic

```c
void kpanic(const char* msg, ...) __attribute__((noreturn));
```

Prints `====== KERNEL PANIC ======`, the message (supports `%s %x %d`),
a raw RBP-chain stack trace, then executes `cli; hlt` forever.

`__attribute__((noreturn))` lets the compiler omit "control reaches end of
non-void function" warnings in code paths that call `kpanic`.

The stack trace walks RBP frames up to 32 levels.  Because the kernel is
compiled with `-fno-omit-frame-pointer`, every C function maintains a valid
frame pointer chain.  The trace prints virtual addresses; you can cross-
reference with `nm build/kernel.elf` or `objdump -d build/kernel.elf`.

---

## Usage pattern

Every kernel `.c` file begins:

```c
#include "kernel.h"
#include "<specific_subsystem>.h"
```

The layering rule means a file at Layer N should only include headers for
Layer N and below.  `kernel.h` is the single exception — it is allowed
everywhere because it contains no implementation, only declarations.

# cat_program.S — Ring-3 `cat` Utility Binary

**Path:** `os/src/boot/cat_program.S`  
**Layer:** User space (ring 3)  
**Language:** x86-64 AT&T assembly

---

## Purpose

A second ring-3 test program that exercises the full VFS-backed syscall
path.  It opens a hardcoded file (`"version.txt"`), reads up to 128 bytes
from it, closes the file, writes the contents to stdout, and exits.  This
tests the open/read/close/write chain end-to-end from user mode.

---

## Why this file exists

`user_program.S` proves the basic ring-3 entry and `SYS_WRITE`.
`cat_program.S` goes further — it exercises `SYS_OPEN`, `SYS_READFILE`,
and `SYS_CLOSE`, which involve the VFS layer.  Embedding both programs
lets the kernel test progressively more complex user↔kernel interactions
without a filesystem on the host.

---

## Syscalls used

| Syscall | Constant | Purpose |
|---------|----------|---------|
| `SYS_OPEN` (5) | open file by name | `rdi` = ptr to `"version.txt"`, `rsi` = flags=0 (read-only) |
| `SYS_READFILE` (7) | read from fd | `rdi` = fd, `rsi` = buf ptr, `rdx` = 128 |
| `SYS_CLOSE` (6) | close fd | `rdi` = fd |
| `SYS_WRITE` (1) | write to stdout | `rdi` = 1, `rsi` = buf, `rdx` = bytes read |
| `SYS_EXIT` (0) | exit | `rdi` = exit code |

All constants come from `#include "syscall_defs.h"` — the same header shared
with the kernel and libuser, ensuring ABI consistency.

---

## Flow

```
_start:
    SYS_OPEN("version.txt", 0)  → fd in %rax → %r12
    if fd < 0 → jump .Lerr
    SYS_READFILE(fd, buf, 128)  → bytes read → %r13
    SYS_CLOSE(fd)
    SYS_WRITE(1, buf, %r13)     ← print file content
    SYS_WRITE(1, "\n", 1)       ← trailing newline
    SYS_EXIT(0)
.Lerr:
    SYS_WRITE(1, "ERROR!\n", 7)
    SYS_EXIT(1)
```

`%r12` and `%r13` are callee-saved registers — safe to use across `int $0x80`
because the kernel preserves all GP registers in `int_frame_t` and restores
them on `iretq`.

---

## Data

```asm
.section .rodata
filename: .asciz "version.txt"   // null-terminated path
errmsg:   .asciz "ERROR!\n"
newline:  .byte 10               // '\n' for trailing newline

.section .bss
buf: .space 128                  // read buffer — zero-initialised at load time
```

`.bss` data is allocated by the kernel when mapping the ELF segments into
user memory (the `memsz > filesz` segment in the PT_LOAD entry covers it).

---

## Build note

This file is compiled via the same `$(USER_BIN)` pipeline as `user_program.S`
if selected.  The Makefile would need to add a separate target for
`cat_program.bin` and embed it with a distinct symbol prefix.  Currently
only `user_program.S` is wired into the default build.

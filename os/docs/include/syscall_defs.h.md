# syscall_defs.h — Syscall Number Table (Shared ABI)

**Path:** `os/src/include/syscall_defs.h`  
**Layer:** Shared — used by kernel, user programs, and libuser

---

## Purpose

Single source of truth for all syscall numbers.  Both the kernel's
`syscall.c` dispatch table and user-space programs (`user_program.S`,
`cat_program.S`, `user.c`) include this file, ensuring both sides always
agree on which number maps to which system call.

---

## `SYSCALL(n)` macro

```c
#define SYSCALL(n) n
```

Defined for both `__ASSEMBLER__` and C contexts identically.  This means
assembly files can use `movq $SYS_WRITE, %rax` directly after
`#include "syscall_defs.h"` without needing a separate `.equ` block.

---

## Syscall table

| Number | Constant | Kernel handler | Description |
|--------|----------|---------------|-------------|
| 0 | `SYS_EXIT` | `sys_exit` | Terminate current thread |
| 1 | `SYS_WRITE` | `sys_write` | Write bytes to fd (stdout → serial) |
| 2 | `SYS_READ` | `sys_read` | Read bytes from fd (stdin → UART) |
| 3 | `SYS_GETPID` | — | Get current process ID |
| 4 | `SYS_SBRK` | — | Extend/contract heap |
| 5 | `SYS_OPEN` | — | Open a VFS path → fd |
| 6 | `SYS_CLOSE` | — | Close an fd |
| 7 | `SYS_READFILE` | — | Read from open fd (VFS) |
| 8 | `SYS_WRITEFILE` | — | Write to open fd (VFS) |
| 9 | `SYS_EXECVE` | — | Replace current process image with ELF |
| 10 | `SYS_FORK` | — | Clone current process |
| 11 | `SYS_WAITPID` | — | Wait for a child process to exit |
| 12 | `SYS_GETPPID` | — | Get parent process ID |
| 13 | `SYS_YIELD` | — | Voluntarily yield the CPU |
| 14 | `SYS_SLEEP` | — | Sleep for N milliseconds |
| 15 | `SYS_UPTIME` | — | Get system uptime in ms |
| 16 | `SYS_REBOOT` | — | Reboot the system |
| 17 | `SYS_PWRDOWN` | — | Power off |
| 18 | `SYS_CREATE` | — | Create a VFS file |
| 19 | `SYS_MKDIR` | — | Create a VFS directory |
| 20 | `SYS_UNLINK` | — | Delete a VFS file |
| 21 | `SYS_LSEEK` | — | Seek within an open fd |
| 22 | `SYS_STAT` | — | Get file metadata |
| 23 | `SYS_PIPE` | — | Create a pipe pair |

`SYSCALL_COUNT = 24` — the kernel's dispatch table must have at least this
many entries or numbers ≥ count will return `ERR_NOSYS`.

---

## Implementation status

Currently `syscall.c` only implements syscalls 0–2 (`exit`, `write`,
`read`).  All other numbers return `ERR_NOSYS` when invoked.  The
constants in this file define the *intended* full ABI for when the
remaining handlers are implemented.

---

## Adding a new syscall

1. Add `#define SYS_NEWCALL  N` here (increment `SYSCALL_COUNT`).
2. Add `static uint64_t sys_newcall(int_frame_t*)` in `syscall.c`.
3. Append it to `syscall_table[]` at index N.
4. Add the wrapper `long newcall(...)` in `user.c` and declare it in `user.h`.

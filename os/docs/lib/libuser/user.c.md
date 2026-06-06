# user.c — User-space Standard Library

**Path:** `os/src/lib/libuser/user.c`  
**Layer:** User space — libc equivalent

---

## Purpose

Provides a minimal C library for user-space programs targeting this kernel.
Every function is implemented as a thin wrapper around `int $0x80` syscalls
using the inline `syscall0`–`syscall3` helpers from `user.h`.  This is the
user-space counterpart to the kernel's `syscall.c`.

---

## Syscall wrappers

The inline helpers in `user.h` follow the kernel's ABI:

| Helper | Registers used | Example |
|--------|---------------|---------|
| `syscall0(n)` | `rax=n` | `getpid()` |
| `syscall1(n, a1)` | `rax=n, rdi=a1` | `close(fd)` |
| `syscall2(n, a1, a2)` | `rax=n, rdi=a1, rsi=a2` | `open(path, flags)` |
| `syscall3(n, a1, a2, a3)` | `rax=n, rdi=a1, rsi=a2, rdx=a3` | `write(fd, buf, n)` |

All use `"memory"` clobber to prevent the compiler from caching values
across the syscall.

---

## Implemented functions

| Function | Syscall | Notes |
|----------|---------|-------|
| `_exit(code)` | `SYS_EXIT` | Loops forever after (should not return) |
| `putchar(c)` | `SYS_WRITE(1, &c, 1)` | Writes one byte to stdout |
| `puts(s)` | `SYS_WRITE(1, s, len) + SYS_WRITE(1, "\n", 1)` | Writes string + newline |
| `write(fd, buf, count)` | `SYS_WRITE` | Raw write |
| `read(fd, buf, count)` | `SYS_READ` | Raw read |
| `open(path, flags)` | `SYS_OPEN` | Returns fd or error |
| `close(fd)` | `SYS_CLOSE` | |
| `readfile(fd, buf, count)` | `SYS_READFILE` | VFS read |
| `writefile(fd, buf, count)` | `SYS_WRITEFILE` | VFS write |
| `execve(path)` | `SYS_EXECVE` | Replace process image |
| `fork()` | `SYS_FORK` | Clone process |
| `waitpid(pid, status)` | `SYS_WAITPID` | Wait for child |
| `getpid()` | `SYS_GETPID` | |
| `getppid()` | `SYS_GETPPID` | |
| `sbrk(increment)` | `SYS_SBRK` | Heap growth |
| `yield()` | `SYS_YIELD` | Voluntarily yield CPU |
| `sleep_ms(ms)` | `SYS_SLEEP` | Sleep N ms |
| `uptime_ms()` | `SYS_UPTIME` | System uptime in ms |
| `reboot()` | `SYS_REBOOT` | |
| `poweroff()` | `SYS_PWRDOWN` | |

Note: `fork`, `execve`, `waitpid`, `sbrk`, `readfile`, `writefile` are
**stub syscalls** — the kernel's `syscall.c` returns `ERR_NOSYS` for all
of these currently.

---

## `puts` vs `write`

`puts(s)` calls `write` twice: once for the string, once for `"\n"`.  There
is no buffering — each call goes directly to `int $0x80`.  For character-
at-a-time output, `putchar` is similarly unbuffered.  This is intentional
for simplicity; a more complete libc would add stdio buffering.

---

## Freestanding nature

`user.c` does not include any host system headers.  All types are defined
in `user.h` (`uint64_t`, `size_t`, etc.).  There is no `malloc`, no
`printf`, no `strlen` — programs must implement these themselves or use
the raw syscall wrappers.

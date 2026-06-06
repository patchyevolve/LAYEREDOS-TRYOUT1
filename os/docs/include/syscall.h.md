# syscall.h — Syscall Dispatch Interface

**Path:** `os/src/include/syscall.h`  
**Layer:** Layer N-1 (Syscall Gateway)

---

## Purpose

Declares the two functions that form the kernel side of the `int $0x80`
syscall interface.  Included by `main.c` (to call `syscall_init`) and by
`hal.c` (to call `syscall_handler` from `interrupt_handler`).

---

## API

```c
void syscall_init(void);
void syscall_handler(struct int_frame* frame);
```

### `syscall_init`
Prints an info line listing how many syscalls are registered.  There is no
runtime initialisation needed beyond that — the IDT entry for vector 128
(DPL=3) is installed by `idt_init()` in `hal.c` unconditionally via the
`isr_vectors` table.

### `syscall_handler(int_frame_t* frame)`
Called directly from `interrupt_handler` when `frame->vector == 128`.
Reads `frame->rax` as the syscall number, dispatches through `syscall_table[]`,
and writes the return value back to `frame->rax` so `iretq` delivers it
to the user process in `%rax`.

---

## Includes `syscall_defs.h`

This header includes `syscall_defs.h` (a shared header that would also be
used by user-space libc stubs to agree on syscall numbers).  Currently the
numbers are:

| Number | Name | Arguments |
|--------|------|-----------|
| 0 | `sys_exit` | (none used) — calls `thread_exit(0)` |
| 1 | `sys_write` | `rdi`=fd, `rsi`=buf, `rdx`=count |
| 2 | `sys_read` | `rdi`=fd, `rsi`=buf, `rdx`=count |

---

## Forward declaration of `int_frame`

`syscall.h` uses `struct int_frame` as an incomplete type (pointer only),
so it does not need to include `hal.h`.  Files that actually dereference
frame fields must include `hal.h` themselves (which `syscall.c` does).

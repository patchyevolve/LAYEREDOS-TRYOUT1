# syscall.c — Syscall Handler Implementations

**Path:** `os/src/kernel/syscall.c`  
**Layer:** Layer N-1 (Syscall Gateway)

---

## Purpose

Implements the three system calls available to user-mode programs via
`int $0x80`.  Dispatches based on the syscall number in `frame->rax` and
writes the return value back so `iretq` delivers it to the user.

---

## Dispatch mechanism

```c
typedef uint64_t (*syscall_fn)(int_frame_t*);
static syscall_fn syscall_table[] = {
    sys_exit,    // 0
    sys_write,   // 1
    sys_read,    // 2
};
```

`syscall_handler` reads `frame->rax` as the number.  If it's out of range,
`frame->rax` is set to `(uint64_t)(int64_t)ERR_NOSYS` (-9 cast to unsigned)
and returns.  Otherwise `syscall_table[num](frame)` is called and its
return value stored back in `frame->rax`.

---

## Syscall 0 — `sys_exit`

```c
thread_exit(0);
return 0;  // never reached
```

Immediately terminates the current thread with exit code 0.  The `thread_exit`
call never returns (it switches to another thread via `switch_context`).

---

## Syscall 1 — `sys_write`

```c
int fd        = frame->rdi;   // file descriptor (ignored, always serial)
const char* buf = frame->rsi; // user virtual address of buffer
size_t count  = frame->rdx;   // byte count
```

Iterates `count` bytes, calling `kputchar` for each.  Stops early at a
null byte (defensive).  The `fd` argument is accepted but ignored — all
output goes to the kernel serial console.

**Security note:** `buf` is a user-supplied pointer.  The kernel currently
dereferences it without any validation.  If the user passes an unmapped
address, a page fault will occur in kernel context, triggering `kpanic`.
A production kernel would use `copy_from_user()` with fault isolation.

Returns the number of bytes written.

---

## Syscall 2 — `sys_read`

```c
int fd     = frame->rdi;   // ignored
char* buf  = frame->rsi;   // user buffer to fill
size_t count = frame->rdx; // max bytes to read
```

Calls `hal_uart_getchar()` in a loop up to `count` times.  Stops early on
`'\n'` or `'\r'` (line-oriented read).  Returns the number of bytes read.

Same security caveat as `sys_write` — user pointer `buf` is not validated.

---

## `syscall_init`

Purely informational.  Prints:
```
[SYSCALL] int 0x80 handler registered, 3 syscalls
```

The actual IDT gate for vector 128 (DPL=3) is installed by `idt_init` in
`hal.c` using the `isr128` stub from `isr.S`.  `syscall_init` does not
touch the IDT.

---

## Calling convention summary (user-side ABI)

```asm
mov $syscall_num, %rax    ; syscall number
mov $arg1,        %rdi    ; first argument
mov $arg2,        %rsi    ; second argument
mov $arg3,        %rdx    ; third argument
int $0x80                 ; enter kernel
; return value in %rax
```

This is a simplified ABI matching the register layout in `int_frame_t`.
It is intentionally different from the Linux `syscall` instruction ABI
to make it clear this is a custom kernel.

---

## Extension path

To add a new syscall:
1. Define `static uint64_t sys_newcall(int_frame_t* frame) { ... }` in
   `syscall.c`.
2. Append it to `syscall_table[]`.
3. Add the number to `syscall_defs.h` so user programs can reference it
   by name.

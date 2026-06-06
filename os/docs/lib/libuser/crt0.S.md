# crt0.S — C Runtime Zero (User-space Startup)

**Path:** `os/src/lib/libuser/crt0.S`  
**Layer:** User space — C runtime startup

---

## Purpose

`crt0` ("C runtime zero") is the true entry point for C user programs.
It runs *before* `main` and is responsible for setting up the minimal
execution environment that `main` expects:

1. Setting RBP to 0 (marks the bottom of the call stack for debuggers
   and stack-trace walkers).
2. Extracting `argc` and `argv` from the user stack.
3. Calling `main(argc, argv)`.
4. Passing the return value of `main` to `SYS_EXIT` so the process
   terminates cleanly.

---

## Why this is assembly, not C

The entry point `_start` cannot be a C function — the C ABI requires a
valid stack frame to already exist when a function is entered.  At `_start`,
the stack is in the raw state set up by `process_exec`: the kernel pushed
the `iretq` frame and zeroed the registers, but the user stack has no C
frame yet.  Writing `_start` in assembly ensures the first instruction
executes unconditionally without any prologue assumptions.

---

## Stack layout at `_start`

When `process_exec` builds the user stack, it places:

```
[RSP+0]   argc           (pushed by convention)
[RSP+8]   argv[0]
[RSP+16]  argv[1]
...
```

Currently the kernel sets up a zero stack — `argc = 0`, no argv pointers.
`crt0.S` reads `argc` from `(%rsp)` and `&argv[0]` from `8(%rsp)`.

---

## Code

```asm
_start:
    xorq  %rbp, %rbp          ; RBP = 0 (sentinel for stack trace walkers)
    movq  (%rsp), %rdi        ; rdi = argc
    leaq  8(%rsp), %rsi       ; rsi = &argv[0]
    call  main                 ; call main(argc, argv)
    movq  %rax, %rdi          ; rdi = main's return value (exit code)
    movq  $SYS_EXIT, %rax     ; rax = 0
    int   $0x80                ; sys_exit(exit_code)
```

`SYS_EXIT = 0` comes from `#include "syscall_defs.h"` at the top of the
file.  The `int $0x80` after `movq $SYS_EXIT, %rax` is the fallback —
`sys_exit` calls `thread_exit` which never returns, but the `int $0x80`
provides a definitive endpoint.

---

## Linking

User programs that use libuser must link `crt0.o` first so `_start`
is the ELF entry point:

```
ld -nostdlib -Ttext=0x40000000 crt0.o user.o myprogram.o -o myprogram.elf
```

The ELF header's `e_entry` will point to `_start`.

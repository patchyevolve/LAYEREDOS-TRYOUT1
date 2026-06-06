# user_program.S — Ring-3 Test Binary

**Path:** `os/src/boot/user_program.S`  
**Layer:** User space (ring 3) — NOT part of the kernel  
**Language:** x86-64 AT&T assembly

---

## Purpose

A minimal ring-3 program that exercises the two most fundamental user→kernel
paths:

1. **`sys_write` (syscall 1)** — prints "Hello from user mode!\n" to the
   serial console via `int $0x80`.
2. **`sys_exit` (syscall 0)** — terminates the thread via `int $0x80`.

This file exists solely to prove that the ring-3 entry path, the page-table
USER flag propagation, the TSS RSP0 stack switch, and the `int $0x80` IDT
gate all work end-to-end.  It is not a libc; it has no startup runtime.

---

## Why assembly, not C?

A C compiler targeting a freestanding environment still needs crt0 startup
code to initialise the stack, clear `.bss`, and call `main`.  Writing the
test in assembly lets the entry point be exactly `_start` with no wrapper,
keeping the binary as small as possible (< 64 bytes of machine code + the
string literal).

---

## Build pipeline

This file is **not** compiled as part of the normal kernel object list.
The Makefile handles it specially:

```
1. gcc -c user_program.S -o user_program.o
       (same CFLAGS as kernel but that's fine — only .text matters)

2. ld -nostdlib -Ttext=0 user_program.o -o user_program.elf
       Link at VA 0 so the flat binary starts at byte offset 0.
       If linked at 0x40000000, objcopy -O binary would pad 1 GB of zeros.

3. objcopy -O binary user_program.elf user_program.bin
       Strip ELF headers; output is raw machine code starting at offset 0.

4. objcopy --input-target=binary --output-target=elf64-x86-64
          --rename-section .data=.rodata
          user_program.bin user_program_embedded.o
       Wrap the raw bytes in an ELF64 object with section .rodata so the
       linker places it in the kernel's .rodata region.
       Exposes three symbols the kernel uses:
         _binary_build_user_program_bin_start
         _binary_build_user_program_bin_end
         _binary_build_user_program_bin_size
```

---

## Runtime layout

At execution time the kernel:

- Copies the raw bytes from `_binary_..._start` to a freshly-allocated
  physical page mapped at virtual address `0x40000000` with `PAGE_USER|PAGE_WRITE`.
- Maps a zeroed page at `0x70000000` as the user stack.
- Builds an `iretq` frame that sets `RIP=0x40000000`, `RSP=0x70001000`,
  `CS=0x1B` (user code, ring 3), `SS=0x23` (user data, ring 3),
  `RFLAGS=0x202` (interrupts enabled, reserved bit).

Because the binary was linked at VA 0 but is *executed* at `0x40000000`,
all PC-relative addresses are wrong — **except** the `lea msg(%rip), %rsi`
which is already RIP-relative and resolves correctly regardless of load
address.  The `int $0x80` instruction is also position-independent (it's
a fixed opcode with no address).

---

## Syscall ABI (`int $0x80`)

The kernel's `syscall_handler` reads the frame that `isr_common_handler`
built:

| Register | Role |
|----------|------|
| `RAX` | Syscall number (0 = exit, 1 = write, 2 = read) |
| `RDI` | arg1 — file descriptor (1 = stdout for write) |
| `RSI` | arg2 — buffer pointer (user virtual address) |
| `RDX` | arg3 — byte count |

Return value is written back to `frame->rax`.

---

## The `msg` label

```asm
msg: .ascii "Hello from user mode!\n"
```

Placed immediately after the `int $0x80` (exit) instruction, still within
the `.text` section.  `lea msg(%rip), %rsi` computes the RIP-relative
offset at assemble time; at runtime this resolves to the correct physical
page + offset regardless of the 0x40000000 load address, because the
offset within the page is the same whether the page is viewed at VA 0
(link time) or VA 0x40000000 (run time).

---

## What a successful run looks like

```
PT dump: pd_phys=... pt_phys=... PT[0]=...
code_page=... content=4800000001c0c748 ...
binary_start=ffffffffc0109af2 first_bytes=4800000001c0c748
[timer ticks...]
Hello from user mode!
OS>
```

The `first_bytes` value `0x4800000001c0c748` decodes as:
```
48 c7 c0 01 00 00 00   mov $1, %rax
48 ...                 mov $1, %rdi
```
which is exactly the first two instructions of `_start`.

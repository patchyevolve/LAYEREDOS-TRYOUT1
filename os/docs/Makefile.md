# Makefile — Build System

**Path:** `os/Makefile`  
**Layer:** Build tooling (no runtime layer)

---

## Purpose

Drives the entire build: compiles all C and assembly sources, assembles
and strips the ring-3 test binary, embeds that binary as a read-only object,
links everything into a single higher-half ELF kernel, and provides QEMU
run targets.

---

## Toolchain variables

| Variable | Default | Meaning |
|----------|---------|---------|
| `CC` | `gcc` | C compiler — also used as assembler front-end for `.S` files so that C preprocessor macros work in assembly |
| `LD` | `ld` | Bare linker — used for both the user binary and the kernel |
| `AS` | `gcc` | Assembler (same as CC) |
| `OBJCOPY` | `objcopy` | Binary conversion tool |
| `QEMU` | `qemu-system-x86_64` | Emulator |

---

## CFLAGS breakdown

```
-ffreestanding        no host libc assumptions
-m64 -mcmodel=kernel  64-bit, kernel code model (addresses above 2 GB)
-mno-red-zone         disable the 128-byte red zone (ISRs corrupt it)
-mno-mmx/sse/sse2     no SIMD — keeps ISR save/restore simple
-fno-stack-protector  no __stack_chk_fail (no libc)
-fno-pic -fno-pie     position-dependent, required for -mcmodel=kernel
-nostdlib             no standard library linked
-fno-strict-aliasing  avoid UB from type-punning common in low-level code
-fno-omit-frame-pointer  keeps RBP chains intact for stack traces in kpanic
-Wall -Wextra -Werror  all warnings are errors
-O2 -g                optimise and embed debug info
```

---

## Source gathering

- **C sources** — all `*.c` files under `src/boot/`, `src/kernel/`, `src/lib/`
- **Assembly sources** — all `*.S` files under those directories,
  *excluding* `src/boot/user_program.S` which is handled separately
- The user program has its own dedicated build pipeline (see below)

---

## User-program build pipeline

```
user_program.S  ──(gcc -c)──►  user_program.o
                               │
                         (ld -Ttext=0)        ← link at VA 0 so the flat
                               │               binary starts at byte 0
                         user_program.elf
                               │
                     (objcopy -O binary)       ← strip ELF headers, produce
                               │               raw machine code
                         user_program.bin      ← embedded in kernel .rodata
                               │
                 (objcopy --input binary
                  --output elf64-x86-64
                  --rename-section .data=.rodata)
                               │
                    user_program_embedded.o    ← exports three symbols:
                                                 _binary_build_user_program_bin_start
                                                 _binary_build_user_program_bin_end
                                                 _binary_build_user_program_bin_size
```

**Why `-Ttext=0`?**  
`objcopy -O binary` pads from address 0 to the first byte of content.
If the program were linked at `0x40000000`, the output file would be
~1 GB of leading zeros before the actual code.  Linking at 0 means the
flat binary starts immediately at byte 0.  The kernel copies those bytes
to the correct user virtual address (`0x40000000`) at runtime.

**Why `--rename-section .data=.rodata` without extra flags?**  
`objcopy --input binary` places the raw bytes in a section called `.data`.
Renaming it to `.rodata` (without appending `,alloc,load,readonly`) preserves
the original `CONTENTS` attribute on the section.  If the extra flags were
appended, objcopy would **replace** all section flags, dropping `CONTENTS`,
making the section appear empty to the linker — the bug that caused the
all-zeros embedded binary.

---

## Linking

```
$(LD) -nostdlib -z max-page-size=0x1000 -T linker.ld
      $(OBJS) $(LIBGCC)
  → build/kernel.elf
```

`LIBGCC` is included for compiler-generated 128-bit division helpers
(`__udivti3` etc.) used by the PMM timer overflow fix.

The `-z max-page-size=0x1000` prevents LD from aligning sections to 2 MB
(which would add a massive gap between `.boot_data` and `.text`).

---

## Run targets

| Target | Description |
|--------|-------------|
| `make run` | QEMU with serial on stdio, VGA display, no-reboot |
| `make headless` | QEMU no display, logs to `qemu.log`, `-d cpu_reset` |
| `make debug` | QEMU + GDB stub on port 1234, halted at start, interrupt trace |
| `make monitor` | QEMU with QEMU monitor on a VC |
| `make dump` | Dump raw kernel binary for size inspection |
| `make size` | Print ELF section sizes |
| `make clean` | Remove `build/` entirely |
| `make rebuild` | `clean` then `all` |

---

## Known linker warning

```
ld: warning: build/kernel.elf has a LOAD segment with RWX permissions
```

This is expected: the `.boot_text` section must be both executable and
writable (for position-independent 32-bit startup code before paging
is enabled).  It is the only tolerated warning.

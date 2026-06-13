# OPERtur/TRY1 — Kernel Project Summary

**Last updated:** June 9, 2026

## Project Overview

OPERtur/TRY1 is an OS kernel being developed from scratch. The codebase is in `os/` and the architecture is x86-64.

## Quick Reference

### Key addresses
HPET MMIO: 0xFED00000 — NOT MAPPED on QEMU TCG (TCG softmmu cache bug workaround)
                         `hpet_map_mmio()` returns NULL immediately; fall back to PIT
APIC base: 0xFEE00000 — NOT MAPPED on QEMU TCG; fall back to legacy PIC
COM1 (UART): 0x3F8
Kernel base (higher half): 0xFFFFFFFF80000000
User space top:            0x0000800000000000
Signal trampoline:         `SIGNAL_TRAMPOLINE_ADDR` (defined in process.h)

### Known platform constraints (QEMU TCG)
- HPET and APIC MMIO must NOT be mapped. `hpet_map_mmio()` and `apic_init()` both short-circuit.
- Scheduling tick source: PIT (IRQ0) only. APIC timer is not active.
- Timekeeping: PIT-resolution only (~1 ms). `hal_timer_get_ns()` falls back to PIT tick counter * 1,000,000.
- `vmm_flush_tlb_page`: plain `invlpg` only. Do not attempt CR3 reload or `wbinvd` as workaround.
- Any future task that requires HPET precision (T5.7 TCP retransmit timer, T9.1 GDB timing) must account for PIT-only resolution.
- If/when testing on KVM or bare metal, HPET/APIC paths need to be re-enabled and re-tested.

## Current Session Accomplishments (June 9, 2026)

### Stage 4 TTY/PTY Completion
1. **SYS_IOCTL (syscall 34)** — `vfs_ioctl` dispatch with `TCGETATTR` / `TCSETATTR` / `TIOCGPGRP` / `TIOCSPGRP` for TTY.
2. **SIGTTIN / SIGTTOU** — TTY read sends SIGTTIN to background process; TTY write sends SIGTTOU when TOSTOP set and process is background.
3. **SYS_SETPGID / SYS_GETPGID (syscalls 35/36)** — Set/get process group ID with self-or-child permission check.
4. **PTY pseudo-terminal subsystem** — Created `pty.c`/`pty.h` with 8-slot pool, master-slave VFS file_ops, line discipline (canon/raw, echo, signal chars, termios ioctl), `SYS_PTY_PAIR` (syscall 37), `pty_init()` wired in `main.c`.
5. **QEMU TCG HPET/APIC workaround** — HPET and APIC MMIO mapping disabled due to QEMU TCG softmmu page-walk cache bug; kernel falls back to PIT/PIC and boots cleanly.

### Files changed
- `os/src/kernel/pty.h` — new: PTY data structures and API.
- `os/src/kernel/pty.c` — new: master/slave VFS ops, line discipline, `pty_pair_create()`, termios ioctls.
- `os/src/include/syscall_defs.h` — added `SYS_PTY_PAIR=37`, `SYSCALL_COUNT=38`.
- `os/src/kernel/syscall.c` — added `sys_pty_pair` handler and table entry.
- `os/src/kernel/main.c` — added `pty_init()` call.
- `os/src/kernel/tty.c`, `tty.h`, `vfs.c`, `vfs.h` — Stage 4 TTY changes (ioctl, job control, SIGTTIN/SIGTTOU).
- `os/src/kernel/hpet.c` — `hpet_map_mmio` short-circuits (QEMU TCG workaround).
- `os/src/kernel/apic.c` — `apic_init` short-circuits (QEMU TCG workaround).

## Status (as of June 9, 2026)

- All Stage 3 FS tests pass (`make test`).
- Stage 4 TTY/PTY complete — terminal ioctl, job control, pseudo-terminals.
- HPET/APIC disabled on QEMU TCG; kernel uses legacy PIT/PIC and boots to shell cleanly.

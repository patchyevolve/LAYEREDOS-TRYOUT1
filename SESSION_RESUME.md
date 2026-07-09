# Session Resume — Historical Archives

This file documents early session work (2026-06-08 through 2026-06-09). See `AGENTS.md` for the complete chronological record of all subsequent sessions through 2026-07-10.

---

# Session Resume — 2026-06-08

## What Was Done

### TTY Layer (Stage 4)
- Full line discipline (canonical/raw, echo, backspace, Ctrl-D EOF)
- ISR-level signal detection: Ctrl-C/SIGINT, Ctrl-Z/SIGTSTP, Ctrl-\/SIGQUIT delivered immediately via work queue (no need for process to call read())
- Process groups: `pgid` field in `process_t`, each process is its own group leader
- `signal_send_pgid()` signals all members of a process group
- Shell `cmd_run`/`cmd_fg` set `tty_set_fg_pgid()` while foreground job runs
- `/dev/ttyS0` in devfs, FDs 0/1/2 wired to TTY VFS
- `\n` → `\r\n` output processing in `tty_vfs_write`

### Files Modified
- `src/kernel/tty.h` — Added `sig_pending`, `sig_work` fields
- `src/kernel/tty.c` — `tty_input_push` checks signal chars, schedules worker; `tty_init` initializes work item
- `src/kernel/process.h` — Added `pid_t pgid` to `process_t`; declared `signal_send_pgid()`
- `src/kernel/process.c` — `process_create` sets `pgid = pid`; implemented `signal_send_pgid()`
- `src/kernel/shell.c` — `cmd_run`/`cmd_fg` set/restore `fg_pgid`; includes `tty.h`
- `src/kernel/hal.c` — UART ISR calls `tty_input_push()`
- `src/kernel/syscall.c` — `sys_read`/`sys_write` route FDs 0/1/2 through VFS
- `src/kernel/vfs.c` — FDs 0/1/2 not pre-marked (TTY handles it)
- `src/kernel/devfs.c` — Added `DEV_TTY` type and `ttyS0` entry
- `src/kernel/main.c` — Added `tty_init()` call

## Test Status
- `make test` — ALL PASS

## What Was Done (2026-06-09)

### Stage 4 Remaining Items — Three Implemented
1. **Termios ioctl** — `SYS_IOCTL` (syscall 34) with `TCGETATTR`/`TCSETATTR`/`TIOCSPGRP`/`TIOCGPGRP`
2. **SIGTTIN/SIGTTOU** — Background processes reading TTY get SIGTTIN; writing with TOSTOP gets SIGTTOU
3. **`SYS_SETPGID`/`SYS_GETPGID`** — Syscalls 35/36 for user-space process group management
4. **PTY support** — Not yet implemented

### Files Modified
- `os/src/kernel/tty.h` — Added termios_t struct, ioctl constants (TCGETATTR/TCSETATTR/TIOCGPGRP/TIOCSPGRP)
- `os/src/kernel/tty.c` — Added `tty_is_bg()`, SIGTTIN in `tty_vfs_read`, SIGTTOU in `tty_vfs_write` (with TOSTOP), `tty_vfs_ioctl` handler, wired ioctl op into `tty_file_ops`
- `os/src/kernel/vfs.h` — Added `.ioctl` to `vfs_file_ops_t`, declared `vfs_ioctl()`
- `os/src/kernel/vfs.c` — Implemented `vfs_ioctl()` dispatcher
- `os/src/include/syscall_defs.h` — Added `SYS_IOCTL=34`, `SYS_SETPGID=35`, `SYS_GETPGID=36`; bumped `SYSCALL_COUNT` to 37
- `os/src/kernel/syscall.c` — Implemented `sys_ioctl`, `sys_setpgid`, `sys_getpgid`; added to syscall table; made `copy_from_user`/`copy_to_user` non-static for TTY use; added `tty.h` include

## Next Steps

### Stage 4 remaining
1. **PTY support** — Pseudo-terminal master/slave pair

### Stage 5 — Networking (next major stage)
- NIC driver (e1000), ARP, IP, UDP/TCP, sockets API

### Stage 6 — Security
- User/group identity, capabilities, sandboxing

### Stage 7 — SMP
- Secondary CPU bring-up, per-CPU run queues

## How to Continue
Read this file, then read `ROADMAP.md` and `PROGRESS.md` in `os/docs/`.

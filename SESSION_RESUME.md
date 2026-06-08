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

## Next Steps

### Stage 4 remaining
1. **Termios ioctl** — Add `SYS_IOCTL` with `TCGETATTR`/`TCSETATTR`/`TIOCSPGRP`/`TIOCGPGRP`
2. **SIGTTIN/SIGTTOU** — Generate stop signals when background processes access TTY
3. **`SYS_SETPGID`/`SYS_GETPGID`** — User-space process group management
4. **PTY support** — Pseudo-terminal master/slave pair

### Stage 5 — Networking (next major stage)
- NIC driver (e1000), ARP, IP, UDP/TCP, sockets API

### Stage 6 — Security
- User/group identity, capabilities, sandboxing

### Stage 7 — SMP
- Secondary CPU bring-up, per-CPU run queues

## How to Continue
Read this file, then read `ROADMAP.md` and `PROGRESS.md` in `os/docs/`.

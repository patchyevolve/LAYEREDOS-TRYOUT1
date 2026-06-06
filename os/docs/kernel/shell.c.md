# shell.c — Interactive Kernel Shell

**Path:** `os/src/kernel/shell.c`  
**Layer:** Layer N-1 (User Interface / Syscall Gateway)

---

## Purpose

Implements an interactive command-line shell that runs on top of the fully
initialised kernel.  It is the primary operator interface for testing,
diagnostics, and demonstrating kernel features.  `shell_run()` is the main
loop that `kmain` calls after all subsystems are up.

---

## Architecture

The shell runs entirely in the `init` thread (the original `kmain` execution
context).  It reads characters from UART via `hal_uart_getchar()` (blocking),
builds lines in `line_buf`, and dispatches to command handlers.

Between characters it calls `eventbus_dispatch()` and `schedule()` when
`need_reschedule` is set, so other threads (demo tasks, compute workers,
idle) can run while the shell waits for input.

---

## Command table

Commands are registered in a static `shell_cmd_t commands[]` array.
Each entry has `name`, `func`, and `desc`.  `process_line` splits the input
into tokens and does a linear scan for a matching name.

### Built-in commands

| Command | Function | Description |
|---------|----------|-------------|
| `help` | `cmd_help` | List all commands |
| `echo` | `cmd_echo` | Print arguments |
| `meminfo` | `cmd_meminfo` | PMM free/used/total pages and MB |
| `ps` | `cmd_ps` | List all threads with state, priority, name |
| `top` | `cmd_top` | Uptime, memory %, current thread stats |
| `clear` | `cmd_clear` | ANSI clear screen escape |
| `version` | `cmd_version` | OS version + build timestamp |
| `reboot` | `cmd_reboot` | `hal_reboot()` |
| `poweroff` | `cmd_poweroff` | `hal_poweroff()` |
| `panic` | `cmd_panic` | Trigger `kpanic` for testing |
| `demo` | `cmd_demo` | Create 3 `demo_task` threads |
| `compute` | `cmd_compute` | Spawn N compute-bound threads |
| `stats` | `cmd_stats` | Scheduler counters, timer ticks, heap usage |
| `uptime` | `cmd_uptime` | H:M:S from timer ticks |
| `fault` | `cmd_fault` | OOM test or NULL deref trigger |
| `usermode` | `cmd_usermode` | Launch ring-3 test via `process_exec` (future) |
| `mutex` | `cmd_mutex` | 5-thread mutex contention test |
| `event` | `cmd_event` | Publish 100 `EV_USER_EVENT` events |
| `cleanup` | `cmd_cleanup` | `sched_reap_zombies()` |
| `ls` | `cmd_ls` | List VFS root directory |
| `cat` | `cmd_cat` | Print a file from VFS |
| `kbtest` | `cmd_kbtest` | Keyboard driver test loop |
| `atatest` | `cmd_atatest` | ATA drive detection and info |
| `elfload` | `cmd_elfload` | Test ELF loader on embedded binary |
| `run` | `cmd_run` | Load + exec ELF from VFS filesystem |
| `mkdir` | `cmd_mkdir` | VFS mkdir |
| `rm` | `cmd_rm` | VFS unlink |
| `writefile` | `cmd_writefile` | Create/write a file via VFS |
| `format` | `cmd_format` | Format + remount SFS on ramdisk |
| `mount` | `cmd_mount` | List block devices |

---

## Line editing

`shell_run` processes characters one at a time:

| Character | Action |
|-----------|--------|
| `\r` or `\n` | Process line, print prompt |
| `\b` or `DEL` (127) | Backspace — erase last character |
| `' '`–`'~'` | Append to `line_buf` if not full |
| All others | Silently ignored |

History is stored in a circular buffer of 16 lines (`SHELL_HISTORY = 16`).
History is recorded but not yet retrievable via arrow keys (no escape
sequence parsing).

---

## `process_line`

Tokenises the line by splitting on spaces and tabs.  Handles up to
`SHELL_MAX_ARGS = 16` tokens.  Passes `args[]` and `argc` to the matched
command function.

---

## Demo and compute tasks

`demo_task` and `compute_task` are internal thread functions used by the
`demo` and `compute` commands.  They yield periodically so the scheduler
can demonstrate round-robin switching.

- `demo_task`: prints 3 iterations with tick timestamps.
- `compute_task`: accumulates a sum of 100,000 iterations, yielding every 10,000.

---

## Forward declarations for future subsystems

The shell includes headers for several subsystems not yet implemented in the
current build (`vfs.h`, `ramdisk.h`, `keyboard.h`, `ata.h`, `process.h`,
`elf.h`, `block.h`, `sfs.h`).  Commands that use these (`ls`, `cat`, `run`,
`elfload`, `atatest`, `kbtest`, `format`, `mount`, `mkdir`, `rm`, `writefile`,
`usermode`) will compile only when those subsystems are added.  They are
included here to keep the shell as the single point of integration.

---

## `shell_init` / `shell_run`

`shell_init` simply prints the `[SHELL]` init banner.

`shell_run` prints the welcome box and enters the read-dispatch loop.
It never returns; if the kernel reaches the end of `kmain`, a `cli; hlt`
loop follows.

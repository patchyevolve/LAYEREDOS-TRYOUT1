# OPERtur / TRY1 OS — UI/UX Brief

**Version:** 2.0  
**Scope:** All user-facing interfaces — serial console/shell (current), TTY discipline, PTY, and the GUI foundation (Stage 10).

---

## 1. Current Interface: Serial Console Shell

### 1.1 Interface Description
The primary and only current user interface is an interactive shell over a UART serial console. The shell is accessed via COM1 (115200 baud, 8N1) either physically or through QEMU's `-serial stdio` or `-serial mon:stdio` modes.

### 1.2 Shell UX Requirements

**Prompt format:**
```
[username@hostname cwd]$
```
Until the user/group model exists (Stage 6), the prompt shall display:
```
root@opertur /current/dir $
```

**Line editor behaviour (all implemented):**
- Left/right arrows: move cursor within line
- Home/End: jump to start/end of line
- Backspace: delete character left of cursor
- Delete: delete character at cursor
- Ctrl-U: clear from cursor to line start
- Ctrl-K: clear from cursor to line end
- Ctrl-W: delete previous word
- Ctrl-A / Ctrl-E: jump to line start / end
- Ctrl-C: send SIGINT to foreground process group; print `^C`; fresh prompt line

**History:**
- Up/Down arrows: cycle through last 64 commands
- History persists per session; not saved to disk (no ~/.history until Stage 7)
- `history -c` clears session history

**Tab completion:**
- Complete built-in command names
- Complete alias names
- Complete VFS paths (filenames in current directory and absolute paths)
- On ambiguity: print all matches; re-display partial input

**Output conventions:**
- Errors print to stderr: `command: error description`
- Multi-column output (e.g. `ls`) should assume 80-column terminal width
- No ANSI colour codes unless the user has set `TERM=ansi` or equivalent (currently no TERM var enforcement; plain text preferred)

### 1.3 Built-in Command UX Inventory (50+ commands)

| Category | Commands |
|----------|----------|
| Navigation | cd, pwd, ls (-la) |
| File ops | cat, cp, mv, rm, mkdir, rmdir, touch, writefile, stat, chmod, ln, readlink |
| File editing | edit (line-based: l/e/d/a/w/q) |
| Process | ps, kill, nice, top, bg, fg, jobs |
| Text tools | head, tail, wc, grep, find |
| Shell control | echo, export, set, unset, printenv, alias, unalias, source, history |
| I/O | redirect >, >>, < via parser; pipes via \| |
| System | meminfo, stats, reboot, halt |
| Filesystem | snap (take/rollback/info), lock, unlock, fsck, format, mount, backup (save/restore) |
| Network | nicstat, nicdebug, eth_test, arp_test |
| Debug | hexdump |

### 1.4 TTY Line Discipline Behaviour (All Implemented)

**Canonical mode (default):**
- Input buffered until newline
- Backspace edits the buffer
- Ctrl-C → SIGINT to fg process group (delivered immediately via work queue)
- Ctrl-Z → SIGTSTP to fg process group
- Ctrl-\ → SIGQUIT to fg process group
- Ctrl-D → EOF (empty read returns 0 bytes)
- Ctrl-U → kill from cursor to line start
- Ctrl-K → kill from cursor to line end
- Ctrl-W → delete previous word

**Raw mode** (settable via termios ioctl):
- Each byte delivered immediately; no buffering
- No signal generation from special characters
- No echo

**Termios ioctl** (implemented):
- `TCGETATTR` / `TCSETATTR`: get/set termios attributes (ICANON, ECHO, ISIG, TOSTOP, VINTR, VEOF, etc.)
- `TIOCGPGRP` / `TIOCSPGRP`: get/set foreground process group
- Accessible via `SYS_IOCTL` (syscall 34) and userspace `tcgetattr()` / `tcsetattr()` wrappers

**PTY subsystem** (implemented):
- `SYS_PTY_PAIR` (syscall 37) creates a master-slave pseudo-terminal pair
- Master VFS fd: reads processed output, writes raw input
- Slave VFS fd: behaves like a standard TTY with full line discipline
- Termios ioctl, signal generation, and job control work on both master and slave
- 8 PTY slots available (pool in `pty.c`)

**SIGTTIN / SIGTTOU** (implemented):
- Background process reading TTY → receives SIGTTIN (default: stop)
- Background process writing TTY with TOSTOP set → receives SIGTTOU (default: stop)
- `fg` resumes the process and it proceeds with the I/O

### 1.5 Error Message Standards

All shell error messages must follow this format:
```
<command>: <human-readable description>: <errno name if applicable>
```
Examples:
```
open: /etc/shadow: permission denied (EPERM)
exec: /bin/foo: file not found (ENOENT)
mkdir: /tmp/test: already exists (EEXIST)
```

---

## 2. Stage 6 Security UI — Login Flow

When the user/group model is implemented:

```
OPERtur 1.0 — ttyS0

login: root
password: ****

Welcome to OPERtur.
Last login: (none)
root@opertur / $
```

Requirements:
- Password prompt must disable echo (raw mode for the password field)
- Failed login: `Login incorrect.` — no hint about whether username or password was wrong
- After 3 failures: 5-second delay before next attempt (no lockout required initially)
- /etc/passwd format: `username:hashed_password:uid:gid:home:shell` (colon-delimited)

---

## 3. Stage 9 Developer UX — GDB Integration

When the GDB stub is implemented:

**Connection:**
```
(host) $ gdb vmlinuz
(gdb) target remote :1234
Remote debugging using :1234
0xffffffff80001000 in _start ()
(gdb)
```

**QEMU command:**
```
qemu-system-x86_64 -s -S -kernel opertur.elf ...
```
(`-s` = GDB server on port 1234; `-S` = wait for GDB before starting)

**Required stub behaviour:**
- Respond to GDB RSP protocol over serial or TCP
- Support: read/write registers, read/write memory, continue, step, set breakpoints
- `info registers` must show all GP registers + RIP + RFLAGS
- Breakpoints: software (INT3) only initially; hardware (DR0–DR3) stretch goal

---

## 4. Stage 10 GUI Foundation — Visual Design

### 4.1 Target Resolution and Pixel Format
- Minimum: 1024 × 768, 32 bpp (RGBX or XRGB)
- Kernel framebuffer exposed at /dev/fb0 via mmap

### 4.2 Window System Concepts

**Window anatomy:**
```
┌─────────────────────────────────────┐  ← Title bar (24px tall)
│ [×][□][−]  Window Title             │  ← Close / maximise / minimise buttons (left side)
├─────────────────────────────────────┤  ← 1px border
│                                     │
│         Client area                 │
│         (app renders here)          │
│                                     │
└─────────────────────────────────────┘  ← 1px border
```

**Colour palette (minimal, to be expanded):**
| Role | Value |
|------|-------|
| Title bar active | #2B5EA7 |
| Title bar inactive | #666666 |
| Title bar text | #FFFFFF |
| Window border | #444444 |
| Desktop background | #1A1A2E |
| System font | Embedded 8×16 bitmap font (IBM PC 437 compatible) |

### 4.3 Input Model
- Keyboard events: scancode → keycode → Unicode codepoint
- Mouse events: relative motion + button state → absolute cursor position (clamped to screen bounds)
- Event queue: per-window FIFO; events delivered in order; no dropping
- Focus model: click-to-focus; focused window receives all keyboard events
- Z-order: click on unfocused window raises it to top

### 4.4 Compositor Behaviour
- Compositor maintains a z-ordered list of surfaces
- On damage (window move, resize, content update): redraw dirty rectangle only
- Cursor: software cursor; blit cursor bitmap over final composited frame at mouse position
- Double-buffering: compositor writes to back buffer; flip to front on vsync (or on timer if no vsync)

### 4.5 Font Rendering
- Phase 1 (Stage 10 minimum): 8×16 bitmapped font; ASCII + Latin-1 only
- Phase 2 (stretch): stb_truetype or embedded FreeType for proportional rendering
- Text in title bar: centred vertically in 24px title bar height
- Terminal window: monospaced, 8×16 glyphs, 80-column × 25-row default

### 4.6 Terminal Emulator Window (First GUI App)
- The shell must run inside a terminal emulator window as the first GUI application
- Terminal emulator connects to the PTY master; shell runs on PTY slave
- Emulator renders VT100-compatible escape sequences:
  - Cursor movement (ESC[H, ESC[A/B/C/D)
  - Clear screen (ESC[2J)
  - Bold/dim/underline (ESC[1m / ESC[2m / ESC[4m)
  - Foreground/background 8-colour (ESC[3Xm / ESC[4Xm)

---

## 5. Accessibility Constraints
- All output must be readable on a plain VT100 terminal (no mandatory colour)
- No functionality shall be gated behind GUI once the GUI exists; CLI always works
- Error messages must be machine-parseable (consistent prefix format, as in §1.5)

---

## 6. UX Consistency Rules

These apply to all current and future interfaces:

1. **Every destructive operation must be reversible or confirmed.** `rm -r /` shall require `--force` and print a warning. Filesystem snapshot/rollback provides the undo path.
2. **Errors always print to stderr.** Commands that mix status and error output to stdout are a bug.
3. **Silent success.** Commands that succeed do not print anything unless the user asks (e.g. `cp file1 file2` prints nothing on success; `cp -v` enables verbose).
4. **Exit codes.** 0 = success, 1 = general failure, 2 = usage error, 127 = command not found.
5. **No blocking prompts in scripts.** When stdin is not a TTY, confirmation prompts are skipped and the safe default (no-op) is taken.

# shell.h — Shell Interface

**Path:** `os/src/kernel/shell.h`  
**Layer:** Layer N-1 (User Interface) — header

---

## Purpose

Declares the two public functions and the two configuration constants for
the kernel shell.  Only `main.c` needs to include this header.

---

## Constants

```c
#define SHELL_HISTORY  16    // number of command history entries retained
#define SHELL_LINE_BUF 256   // maximum characters per command line
```

`SHELL_LINE_BUF` limits both the input buffer and history storage.
Commands longer than 255 characters are silently truncated.

---

## API

```c
void shell_init(void);   // print init banner — call once after all subsystems up
void shell_run(void);    // enter the command loop — never returns
```

`shell_run` is blocking.  It owns the calling thread (the `init` thread /
`kmain` execution context) for the lifetime of the kernel.  There is no
mechanism to `exit` the shell cleanly — a `reboot` or `poweroff` command
is required.

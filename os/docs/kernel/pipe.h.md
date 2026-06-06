# pipe.h — Pipe Interface

**Path:** `os/src/kernel/pipe.h`  
**Layer:** Layer 3 (IPC) — header

---

## Purpose

Declares `pipe_t` and the single public function `pipe_create`.  Included
by code that needs to create pipes (future `SYS_PIPE` syscall handler,
shell `|` operator).

---

## `pipe_t` (declared here for embedding)

The full struct is declared in the header so future code can embed a
`pipe_t` without heap allocation:

```c
typedef struct pipe {
    uint8_t      buf[PIPE_BUF_SIZE];   // PIPE_BUF_SIZE = 4096
    uint32_t     read_pos, write_pos;
    uint32_t     count;
    spinlock_t   lock;
    wait_queue_t readers, writers;
    int          read_closed, write_closed;
} pipe_t;
```

---

## API

```c
int pipe_create(int fds[2]);
// fds[0] = read end fd
// fds[1] = write end fd
// Returns 0 on success, -1 on OOM or no free fd slots
```

---

## Dependencies

`pipe.h` includes `vfs.h`, `sync.h`, and `sched.h`.  Any file including
`pipe.h` therefore transitively requires all VFS and scheduler types.
Keep this header out of files that don't need pipes to avoid unnecessary
compile-time coupling.

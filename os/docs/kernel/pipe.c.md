# pipe.c — Anonymous Pipe (IPC)

**Path:** `os/src/kernel/pipe.c`  
**Layer:** Layer 3 (IPC / Process) — via VFS

---

## Purpose

Implements POSIX-style anonymous pipes as a pair of VFS nodes sharing a
4 KB ring buffer.  One end is read-only, the other write-only.
`pipe_create(fds[2])` returns two file descriptors: `fds[0]` for reading,
`fds[1]` for writing.  Data written to `fds[1]` can be read from `fds[0]`,
allowing inter-thread (and future inter-process) communication.

---

## `pipe_t` structure

```c
typedef struct pipe {
    uint8_t      buf[PIPE_BUF_SIZE];  // 4096-byte ring buffer
    uint32_t     read_pos;            // next byte to read
    uint32_t     write_pos;           // next byte to write
    uint32_t     count;               // bytes currently in buffer
    spinlock_t   lock;                // protects all fields
    wait_queue_t readers;             // threads blocked waiting for data
    wait_queue_t writers;             // threads blocked waiting for space
    int          read_closed;         // 1 if read end has been closed
    int          write_closed;        // 1 if write end has been closed
} pipe_t;
```

The pipe uses a count + read/write position style rather than the
`(head==tail → empty, (head+1)%SIZE==tail → full)` style.  `count` is
the authoritative source; `read_pos` and `write_pos` are computed offsets.

---

## VFS integration

Each pipe end is a `vfs_node_t` with `fs = &pipe_fs` (a static `vfs_fs_t`
pointing to `pipe_ops`).  The `flags` field distinguishes the ends:
`flags = 1` for the read end, `flags = 0` for the write end.

`pipe_ops` implements:

| Op | Behaviour |
|----|-----------|
| `open` | No-op |
| `close` | Marks the corresponding end closed; frees `pipe_t` when both ends are closed |
| `read` | Copies from ring buffer under spinlock; blocks if empty (unless write-closed) |
| `write` | Copies into ring buffer under spinlock; blocks if full; returns -1 if read-closed (broken pipe) |

---

## `pipe_read` flow

```
loop until done == count:
    spinlock_acquire
    if count > 0:
        copy min(requested, available) bytes from ring
        decrement p->count
        wake writers
        spinlock_release
    else:
        if write_closed: spinlock_release; break (EOF)
        spinlock_release
        sched_block(&p->readers)  ← sleep until writer adds data
```

---

## `pipe_write` flow

```
loop until done == count:
    spinlock_acquire
    if read_closed: spinlock_release; return -1 (SIGPIPE equivalent)
    if space > 0:
        copy min(requested, space) bytes into ring
        increment p->count
        wake readers
        spinlock_release
    else:
        spinlock_release
        sched_block(&p->writers)  ← sleep until reader consumes data
```

---

## `pipe_close`

When either end is closed, the corresponding flag (`read_closed` or
`write_closed`) is set.  If **both** are closed, the pipe is freed via
`kfree(p)` and any still-blocked readers/writers are woken (they will
see EOF / broken pipe).

---

## `pipe_create`

1. `kmalloc(sizeof(pipe_t))` — allocate shared pipe state.
2. Create two `vfs_node_t`s: `"pipe:r"` and `"pipe:w"`.
3. Find two free slots in the global `fd_table[]` and assign the nodes.
4. Return both indices in `fds[0]` and `fds[1]`.

**Direct access to `fd_table`:** `pipe.c` directly accesses the global
`fd_table[]` declared in `vfs.c` via `extern`.  This is a layer coupling
that should ideally go through a VFS registration function.

---

## Limitations

- Pipe buffer is fixed at 4096 bytes (`PIPE_BUF_SIZE`).
- No `O_NONBLOCK` support — reads and writes always block if the buffer
  is empty/full.
- No `SIGPIPE` signal — write to a closed pipe returns -1 silently.
- `pipe_create` iterates the global `fd_table` directly, bypassing VFS's
  `vfs_open` path.

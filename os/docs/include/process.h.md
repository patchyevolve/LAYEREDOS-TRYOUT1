# process.h — Process Control Block and Process Manager Interface

**Path:** `os/src/include/process.h`  
**Layer:** Layer 3 (Process Lifecycle)

---

## Purpose

Declares the `process_t` structure (process control block) and the process
manager API.  A *process* is the unit of resource ownership (page table,
file descriptors, capabilities).  Each process contains one or more
*threads* (scheduled by Layer 2).  This header sits in `src/include/`
rather than `src/kernel/` so it can be included by both kernel and
user-space bridge code.

---

## Constants

```c
#define MAX_PROCESSES   256   // hard limit on simultaneous processes
#define PROCESS_NAME_MAX 64   // max chars in process name
#define MAX_FDS          32   // max open file descriptors per process
```

---

## `pid_t`

```c
typedef uint64_t pid_t;
```

Process ID — globally unique, assigned monotonically from `next_pid = 1`.
PID 0 is used as "no process" / free slot sentinel.

---

## `process_t` — Process Control Block

| Field | Type | Description |
|-------|------|-------------|
| `pid` | `pid_t` | Unique process ID |
| `ppid` | `pid_t` | Parent process ID |
| `name` | `char[64]` | Human-readable name |
| `cr3` | `uint64_t` | Physical address of this process's PML4 page table |
| `entry_point` | `uint64_t` | Virtual address of ELF entry (filled by `elf_load`) |
| `user_stack_top` | `uint64_t` | Top of user stack virtual address |
| `user_code_start` | `uint64_t` | Start of user code segment |
| `user_code_size` | `uint64_t` | Size of user code segment |
| `threads` | `list_head_t` | Head of per-process thread list |
| `process_node` | `list_head_t` | Node in global `process_list` |
| `thread_count` | `int` | Number of live threads |
| `exit_code` | `int` | Exit code set by `process_exit` |
| `exited` | `int` | 1 after process has exited |
| `exit_waiters` | `wait_queue_t` | Threads blocked in `waitpid` |
| `caps` | `uint64_t` | Capability bitmask (placeholder) |
| `fds[32]` | `void*[]` | Open file descriptor slots (placeholder) |

**`cr3` per-process:** Each process gets its own PML4, copied from the
kernel's PML4 for the top half (kernel mappings, entries 256–511).  The
bottom half (user mappings, entries 0–255) starts zeroed and is populated
by `elf_load` via `vmm_map_page`.

---

## `list_head_t` dependency

`process.h` uses `list_head_t` (a doubly-linked list node embedded in
structs).  This type must be defined before `process.h` is included — it
comes from the kernel's internal list utilities (not yet broken out into
a separate header; callers must ensure the definition is visible).

---

## API

| Function | Description |
|----------|-------------|
| `process_init()` | Init process table, create PID-1 "init" process |
| `process_create(name, ppid)` | Allocate a PCB, create a new PML4 with kernel mappings copied |
| `process_exec(proc, elf_data, elf_len)` | Load an ELF, map segments, create the first thread, add to scheduler |
| `process_exit(proc, exit_code)` | Free user pages, wake waiters, remove from process list |
| `process_find(pid)` | Linear scan of process table, returns `process_t*` or NULL |
| `process_get_current_pid()` | Returns `current_thread->proc->pid` or 0 |

# process.c — Process Manager

**Path:** `os/src/kernel/process.c`  
**Layer:** Layer 3 (Process Lifecycle)

---

## Purpose

Implements process creation, ELF execution, exit, and lookup.  A process
owns a separate virtual address space (PML4) derived from the kernel's
with user segments added by `elf_load`.  Each process contains one or more
threads managed by the Layer 2 scheduler.

---

## Global state

```c
static process_t process_table[MAX_PROCESSES];   // 256 slots
static pid_t next_pid = 1;
static list_head_t process_list;     // doubly-linked list of active processes
static spinlock_t process_lock;      // protects table + list
static uint64_t kernel_cr3 = 0;     // saved at init for page table switches
```

---

## `process_init`

1. Zeroes the process table.
2. Initialises the process list and spinlock.
3. Saves the current CR3 as `kernel_cr3` for later use in `process_exit`.
4. Creates the `"init"` process (PID 1) with `ppid = 0`.

---

## `process_create(name, ppid)`

1. Acquires `process_lock`.
2. Finds a free slot (PID field == 0) in `process_table`.
3. Assigns a new PID (`next_pid++`), copies name.
4. Allocates a new PML4 page via `pmm_alloc_page`.
5. **Copies kernel mappings:** copies entries 256–511 from the current
   PML4 into the new one.  Entries 0–255 (user half) are left as zero.
6. Sets `proc->cr3 = pml4_phys`.
7. Adds to `process_list`, releases lock.

**Why copy kernel entries?**  The kernel's higher-half mappings (PML4
entries 256–511) must be present in every process's PML4.  When a syscall
causes a ring-3→ring-0 transition, the CPU switches to the kernel stack
but keeps the same CR3 (the process's PML4).  If the kernel's virtual
addresses are not mapped there, every kernel access would fault.

---

## `process_exec(proc, elf_data, elf_len)`

1. Calls `elf_load(proc, elf_data, elf_len)` — maps all `PT_LOAD` segments.
2. Allocates a user stack page, maps it at `0x70000000` with `PAGE_USER|PAGE_WRITE`.
3. Allocates TCB (`thread_t`) and kernel stack via PMM.
4. Builds the kernel stack frame for `user_thread_entry` (same pattern as
   `thread_create_user` in `sched.c`):
   - iretq frame: `SS=USER_DS, RSP=user_stack_top, RFLAGS=0x202, CS=USER_CS, RIP=entry_point`
   - 15 × 0 GP registers
   - `switch_context` frame: return addr = `user_thread_entry`, 6 × 0
5. Sets `tcb->cr3 = proc->cr3` — the process's own PML4.
6. Calls `all_threads_add(tcb)` and `sched_add_thread(tcb)`.

**Note:** `tcb->cr3` is set but `switch_context` does not currently load
CR3 — the kernel runs all threads under the boot PML4.  For true process
isolation, `switch_context` would need to load `new->cr3` into CR3 on
every switch.

---

## `process_exit(proc, exit_code)`

1. Sets `proc->exit_code` and `proc->exited = true`.
2. Wakes `proc->exit_waiters` (threads blocked in `waitpid`).
3. Calls `vmm_free_user_pages(proc->cr3)` — walks user PML4 entries,
   frees all physical pages mapped in the user half.
4. Frees the PML4 page itself.
5. Under `process_lock`, removes from `process_list` and sets `proc->pid = 0`
   (frees the slot).

---

## `process_find(pid)`

Linear scan of `process_table[]` under lock.  O(256) worst case.  Returns
a pointer to the live `process_t` or NULL.

---

## `process_get_current_pid`

```c
if (current_thread && current_thread->proc)
    return current_thread->proc->pid;
return 0;
```

Requires that `thread_t` has a `proc` pointer (a field added to `thread_t`
by the process subsystem beyond the base `sched.h` definition).

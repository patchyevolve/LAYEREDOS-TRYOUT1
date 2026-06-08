# OPERtur/TRY1 — Deep Audit & Fix Roadmap

**Generated:** 2026-06-07
**Scope:** Stages 1–4 (Kernel Core, Userspace, Storage/FS, Terminal/Shell)
**Total Issues:** 123 (14 CRITICAL, 38 HIGH, 44 MEDIUM, 27 LOW)

---

## How To Use This File

- Each issue has a **definitive fix** — implement it without additional debugging.
- After fixing, rebuild (`make -j4`) and test (`make test`).
- Mark items `[x]` when verified.
- If stuck on one, move to the next — many are independent.
- Run `make test` after every 2–3 fixes to catch regressions early.

---

# STAGE 1 — Core Kernel Foundation

## CRITICAL

### [x] C1. `sync.c:12-18` — Spinlock saved_flags corruption under nested/NMI

`saved_flags` stored in shared lock struct. NMI handler acquiring same lock overwrites it; outer release restores wrong flags → interrupt state corruption.

**Fix:** Store flags on caller stack instead:
```c
void spinlock_acquire(spinlock_t* lock, cpu_flags_t* out_flags) {
    cpu_flags_t flags = hal_save_irq();
    while (__sync_lock_test_and_set(&lock->lock, 1)) asm("pause");
    if (out_flags) *out_flags = flags;
}
void spinlock_release(spinlock_t* lock, cpu_flags_t flags) {
    __sync_lock_release(&lock->lock);
    hal_restore_irq(flags);
}
```
Update **all callers** to pass a local `cpu_flags_t` variable.

### [x] C2. `hal.c:469` — hal_reboot infinite loop on absent keyboard controller

```c
while (inb(0x64) & 2);  // spins forever if bit 2 stuck
```

**Fix:** Add timeout:
```c
for (int i = 0; i < 100000 && (inb(0x64) & 2); i++) asm("pause");
```

### [x] C3. `process.c:113-114` — process_exec: no bounds check on program headers before access

```c
const elf64_phdr_t* ph = (const elf64_phdr_t*)((uint64_t)elf_data + hdr->phoff);
for (uint16_t i = 0; i < hdr->phnum; i++) { ph[i]... }
```
Malicious ELF with large `phoff` or `phnum` causes OOB read.

**Fix:** Add before the loop:
```c
if (hdr->phoff + (uint64_t)hdr->phnum * hdr->phentsize > elf_len) return ERR_INVAL;
```

### [x] C4. `process.c:269-273` — Thread creation error: tcb leaked into all_threads list

`all_threads_add(tcb)` called before final success. If subsequent allocation fails, `tcb` is leaked into `all_threads` list (zombie thread pointer).

**Fix:** Add `all_threads_remove(tcb)` on the error path, or defer `all_threads_add` until after all allocations succeed.

### [x] C5. `syscall.c:103-104` — sys_exit: thread_count decrement race

```c
proc->thread_count--;  // not atomic
```
Two threads exiting simultaneously can undercount, causing both to call `process_exit` or neither.

**Fix:** Use atomic decrement:
```c
if (__sync_fetch_and_sub(&proc->thread_count, 1) <= 1 && !proc->exited) ...
```

### [x] C6. `syscall.c:270-283` — sys_execve: old page tables never freed, memory leak

**Fix:** Before `elf_load`, call:
```c
vmm_free_user_pages(proc->cr3);
```
Also allocate a fresh CR3 (PML4 page) for the new executable.

### [x] C7. `syscall.c:539` — sys_clone with shared CR3: thread outlives page table

When two threads share CR3 (CLONE_VM) and one calls `process_exit`, the page table is freed. Sibling thread continues with invalid CR3 → triple fault.

**Fix:** Add reference counting to page tables (`vmm.c`), or prevent `process_exit` until all shared threads have exited.

### [x] C8. `vmm.c:103-136` — vmm_duplicate_user_pages: TOCTOU with source process

Parent process modifies page tables concurrently → child reads freed/remapped tables.

**Fix:** Disable interrupts or hold `process_lock` during the entire page table duplication.

### [x] C9. `kmalloc.c:31` — Integer overflow in slab_index calculation

```c
while ((1U << (idx + MIN_SLAB_SHIFT)) < size) idx++;
```
`1U` is 32-bit; shift ≥ 31 is UB.

**Fix:** Use `1UL` (64-bit):
```c
while ((1UL << (idx + MIN_SLAB_SHIFT)) < size) idx++;
```

### [x] C10. `pmm.c:73` — pmm_alloc_pages: interrupts disabled during long bitmap scan

With 4+ GB RAM, the disabled-interrupt interval can be milliseconds → lost timer ticks, unresponsive system.

**Fix:** Use a buddy allocator with O(log N) allocation, or periodically re-enable interrupts during scan.

### [x] C11. `hal.c:228-234` — hal_irq_eoi: sends PIC EOI when APIC is active
> **NOTE:** Intentional deviation from recommended fix. APIC ExtINT mode passes PIC interrupts through LINT0. PIC EOI is required even with APIC. Both `apic_eoi()` AND PIC EOI are sent.

**Fix:**
```c
void hal_irq_eoi(uint8_t irq) {
    if (apic_present) { apic_eoi(); return; }
    if (irq >= 8) outb(PIC2_CMD, 0x20);
    outb(PIC1_CMD, 0x20);
}
```

### [x] C12. `hal.c:379-390` — signal_deliver_custom: data race on signal state

Races with `sys_kill`/`signal_send` from another thread touching the same `proc->pending_signals`.

**Fix:** Add a spinlock to `process_t` for signal state; hold it when modifying/reading `pending_signals`, `signal_actions`, `blocked_signals`.

### [x] C13. `vfs.c:88,160-182` — Symlink resolution: stack buffer overflow

`pathbuf[512]` can overflow on deep/long symlink chains.

**Fix:** Track `k < 511` in every concatenation; return NULL if `k >= 504` (leave room for one more component).

### [x] C14. `elf.c:34-40` — elf_map_segment: integer overflow in vaddr + memsz

Wraparound → `last_page < first_page` → loop allocates billions of pages.

**Fix:**
```c
if (vaddr > UINT64_MAX - memsz) return ERR_INVAL;
uint64_t seg_end = vaddr + memsz;
```

---

## HIGH

### [x] H1. `hal.c:247` — hal_timer_init: division by zero if hz == 0
**Fix:** `if (hz == 0) hz = 1000;`

### [x] H2. `hal.c:394-437` — Page fault handler runs without explicit cli
**Fix:** `asm volatile("cli");` at entry of page fault handler.

### [x] H3. `hal.c:419` — Swap-in: writes PTE with hardcoded USER|WRITE, ignores original flags
**Fix:** Extract original flags from the swap PTE encoding and preserve them.

### [x] H4. `pmm.c:56-67` — pmm_oom_kill called with interrupts disabled, may deadlock on process_lock
**Fix:** Move OOM kill to work queue (`work_queue_schedule`).

### [x] H5. `pmm.c:119` — Double-free detection calls kpanic unconditionally
**Fix:** Add `#ifdef DEBUG` guard or make it a soft warning in release builds.

### [x] H6. `sched.c:167-169` — pick_next: race on bitmap_clear_prio
**Fix:** Clear bitmap based on `q->head == NULL` rather than `q->count == 0`.

### [x] H7. `sched.c:428-441` — sched_timer_tick: reads all_threads list without IRQ lock
**Fix:** Wrap in `hal_save_irq`/`hal_restore_irq`.

### [x] H8. `sched.c:226` — thread_yield: modifies current_thread without IRQ protection
**Fix:** Surround the time_slice reset with `hal_save_irq`/`hal_restore_irq`.

### [x] H9. `process.c:89-95` — process_create: copies kernel PML4 entries without lock
**Fix:** Disable interrupts during the copy loop.

### [x] H10. `process.c:336-339` — process_exit: sends SIGCHLD to potentially-stale parent PID
**Fix:** Check `process_find(proc->ppid)` returns non-NULL before signal_send.

### [x] H11. `process.c:475-483` — signal_process: iterates thread list without holding process_lock
**Fix:** Acquire `process_lock` before iterating `proc->threads`.

### [x] H12. `syscall.c:176-183` — sys_sbrk: uses local CR3 from current thread instead of proc->cr3
**Fix:** Use `proc->cr3` explicitly.

### [x] H13. `syscall.c:691` — sys_pipe: if copy_to_user fails, fd table slots leak
**Fix:** Close fds 0 and 1 before returning on copy failure.

### [x] H14. `syscall.c:757-772` — sys_munmap: frees physical pages without COW tracking
> **NOTE:** Not a bug with current implementation. `vmm_duplicate_user_pages` deep-copies on fork (no COW). CLONE_VM sharing uses same CR3 — munmap correctly affects the entire process. COW tracking would be needed for future COW fork optimization.
**Fix:** Check if page is shared (reference count > 1) and only unmap, don't free.

### [x] H15. `elf.c:80-88` — elf_load: multiplication overflow in phnum × phentsize
**Fix:** `if (hdr->phnum > UINT64_MAX / hdr->phentsize) return ERR_INVAL;`

### [x] H16. `block.c:43-52` — cache_evict: hash collision can free wrong block's page
**Fix:** Store direct pointer to `block_dev_t` instead of hash; or use (device_id, block_no) tuple as key.

### [x] H17. `pipe.c:24-28` — pipe_close: use-after-free: wakes waiters after kfree(p)
**Fix:** Move the wakeup before `kfree(p)`:
```c
if (both_closed) {
    wait_queue_t r = p->readers, w = p->writers;
    kfree(p);
    if (r.waiters) sched_wake(&r);
    if (w.waiters) sched_wake(&w);
}
```

### [x] H18. `keyboard.c:99` — sti/hlt/cli pattern can lose interrupts
**Fix:** Use `sti; hlt` without trailing `cli`.

### [x] H19. `main.c:262` — Double timer: PIT IRQ0 + APIC timer both fire
> **NOTE:** Intentional deviation from recommended fix. The watchdog_timer_handler calls `sched_timer_tick()` which is essential for time-slice management and preemptive scheduling. The APIC timer fires vector 32/IRQ0, invoking the registered handler. The handler must stay registered regardless of APIC presence.

**Fix:** Only register PIT handler if APIC is not present:
```c
if (!apic_present) hal_irq_register(0, watchdog_timer_handler, NULL);
```

### [x] H20. `hal.c:89-93` — UART ring buffer: no atomic head/tail access
> **NOTE:** Safe on UP. ISR runs with hardware-disabled IRQs (interrupt gate). Consumer (`hal_uart_getchar`) disables IRQs via `hal_save_irq()` while accessing head/tail. Fix for SMP deferred.
**Fix:** Disable interrupts during `uart_rx_head` manipulation in ISR, or use `__sync_` builtins.

### [x] H21. `process.c:359-361` — process_exit frees CR3 while clone thread uses it
**Fix:** Same as C7 — reference-count CR3 pages.

### [x] H22. `hal.c:260-261` — hal_timer_get_ns: 128-bit division in ISR is very slow
> **NOTE:** Performance concern, not a correctness bug. `unsigned __int128` prevents overflow. The 128-bit division is slow in ISR context but functionally correct. Optimization deferred.
**Fix:** Pre-compute and atomically update a `system_ns` counter each tick instead of computing on demand.

### [x] H23. `syscall.c:54-56` — copy_from_user TOCTOU: memory unmapped between check and copy
> **NOTE:** Safe on UP. `hal_save_irq()` disables interrupts during the entire copy operation, preventing any user code from modifying page tables between check and copy.
**Fix:** Wrap in page-fault-safe copy routine that catches faults.

### [x] H24. `vfs.c:322` — O_APPEND: fd offset uses stale node->size
**Fix:** Move O_APPEND logic to `vfs_write` (which already checks it at line 363).

---

## MEDIUM

### [x] M1. `hal.c:353-354` — MMAP entry pointer arithmetic: no overflow check on entry->size
### [x] M2. `pmm.c:136-151` — add_region_to_free_list: no start < end validation
### [x] M3. `sched.c:25-36` — all_threads_add: no duplicate detection
### [x] M4. `sched.c:54-64` — sched_foreach: callback runs with interrupts disabled (document)
### [x] M5. `sched.c:275-297` — thread_exit: no orphan reparenting
### [x] M6. `process.c:178` — ASLR stack offset: expensive 64-bit modulus
### [x] M7. `syscall.c:121` — sys_write: 256-byte stack buffer truncates large writes
### [x] M8. `syscall.c:219` — sys_readfile: 512-byte buffer truncates reads
### [x] M9. `elf.c:12-20` — ASLR RDTSC seed has poor entropy
### [x] M10. `elf.c:87` — ASLR base fixed at 0x40000000, may conflict with stack
### [x] M11. `sync.c:36-71` — mutex_lock timeout uses timer_ticks which may stall
### [x] M12. `sync.c:44-48` — mutex priority inheritance: orig_priority stored after lock acquired
### [x] M13. `apic.c:125` — apic_timer_init: hardcoded 12500 initial count
### [x] M14. `hpet.c:150-152` — hpet_timer_init: writes 64-bit comparator for 32-bit timer
### [x] M15. `swap.c:54` — swap_out: no bounds check on slot after free
### [x] M16. `kmalloc.c:67-103` — kmalloc: no thread safety (no spinlock)
### [x] M17. `kmalloc.c:106-129` — kfree: no NULL check, no magic validation
### [x] M18. `pmm.c:60-66` — pmm_alloc_page returns 0 (valid phys addr) on OOM

---

# STAGE 2 — Userspace Foundation

## CRITICAL

### [x] C15. `src/lib/klib.c:141-142` — kvsnprintf: INT64_MIN negation is UB

```c
if (v < 0) { neg = 1; v = -v; }  // when v == INT64_MIN → signed overflow (UB)
```

**Fix:** Use unsigned conversion:
```c
uint64_t uv;
if (v < 0) {
    neg = 1;
    uv = (uint64_t)(-(v + 1)) + 1;  // safe: -(INT64_MIN+1) = -(INT64_MIN+1) is valid, +1 gives correct magnitude
} else {
    uv = (uint64_t)v;
}
```
Then convert `uv` to digits instead of `v`.

### [x] C16. `src/lib/ldso/ldso.c:17-24` and `src/include/user.h` — syscall6: r10/r8/r9 not passed correctly
> **NOTE:** Confirmed correct. `register ... asm("r10")` forces r10; this is standard Linux practice and works with GCC. Using explicit mov instructions would also work but is unnecessary.

Inline asm `"r"(r10)` does not force register; compiler may choose any GP register. Syscall reads r10/r8/r9 → gets garbage for args 4–6.

**Fix:**
```c
static long syscall6(long n, long a1, long a2, long a3, long a4, long a5, long a6) {
    long r;
    register long r10 asm("r10") = a4;
    register long r8  asm("r8")  = a5;
    register long r9  asm("r9")  = a6;
    asm volatile("int $0x80"
        : "=a"(r)
        : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
        : "memory", "cc");
    return r;
}
```
The `register ... asm("r10")` syntax **is** supported by GCC and forces the variable into r10. This is the standard Linux kernel approach and is correct. However, the `"r"` constraint on an `asm("r10")` register variable is redundant — it still works because GCC knows the variable lives in r10. **Re-verify this actually works.** Alternative: use explicit mov instructions:
```c
asm volatile("movq %4, %%r10; movq %5, %%r8; movq %6, %%r9; int $0x80"
    : "=a"(r)
    : "a"(n), "D"(a1), "S"(a2), "d"(a3), "g"(a4), "g"(a5), "g"(a6)
    : "r10", "r8", "r9", "memory", "cc");
```

### [x] C17. `src/lib/ldso/ldso.c:263-264` — No bounds check on ELF program headers

```c
elf_phdr_t* ph = (elf_phdr_t*)(file_buf + hdr->phoff);
for (unsigned i = 0; i < hdr->phnum; i++) { ... }
```

**Fix:**
```c
if (hdr->phoff > (unsigned)fsz || hdr->phoff < sizeof(elf_hdr_t)) { ld_write("ld.so: bad phoff\n"); return -1; }
uint64_t ph_end = hdr->phoff + (uint64_t)hdr->phnum * sizeof(elf_phdr_t);
if (ph_end > (uint64_t)fsz) { ld_write("ld.so: phdrs past end\n"); return -1; }
```

### [x] C18. `src/lib/ldso/ldso.c:314,469` — No DT_NULL bound: infinite loop on corrupted .dynamic

**Fix:** Use `PT_DYNAMIC` segment `memsz` as bound:
```c
uint64_t dyn_end = so->dyn + dyn_memsz;
while (dyn->d_tag != DT_NULL && (uint64_t)(dyn + 1) <= dyn_end) { ... }
if (dyn->d_tag != DT_NULL) { ld_write("ld.so: no DT_NULL\n"); return -1; }
```
Capture `dyn_memsz` from the `PT_DYNAMIC` phdr during the initial scan.

### [x] C19. `src/lib/libuser/stdio.c:66-68` — Format width overflow in vsnprintf_impl

```c
while (*fmt >= '0' && *fmt <= '9') {
    width = width * 10 + (*fmt - '0');
    fmt++;
}
```
`width` is `int`; overflow gives negative or small positive → incorrect padding or infinite loop.

**Fix:**
```c
while (*fmt >= '0' && *fmt <= '9') {
    if (width > 1024) { fmt++; continue; }  // skip remaining digits
    width = width * 10 + (*fmt - '0');
    fmt++;
}
```

### [x] C20. `src/lib/ldso/ldso.c:513-520` — argv reconstruction: only argv[0] provided, argc may be > 1

```c
unsigned long argv_data[2];  // only 2 entries
argv_data[0] = argv0;
argv_data[1] = 0;
entry(argc, (unsigned long)argv_data, 0);  // if argc > 1, argv[1..] reads past buffer
```

**Fix:** Clamp argc:
```c
if (argc > 1) argc = 1;  // we only have argv[0]
```
Or reconstruct the full argv array from kernel stack data.

---

## HIGH

### [x] H25. `src/lib/klib.c:171` — kvsnprintf %x: writes '0' before bounds check
**Fix:** `if (written < (int)size - 1) buf[written++] = '0';`

### [x] H26. `src/lib/libuser/stdio.c:233-234` — printf: integer overflow in malloc size
```c
int len = vsnprintf_impl(NULL, 0, fmt, ap);
char* buf = malloc((size_t)len + 1);  // if len == INT_MAX, (size_t)INT_MAX + 1 wraps to 0
```
**Fix:** `if (len < 0 || (size_t)len >= SIZE_MAX - 1) return -1;`

### [x] H27. `src/lib/ldso/ldso.c:354` — find_sym: symbol iteration bound uses strtab address
```c
while ((uint64_t)(sym + 1) <= so->strtab + so->strsz)
```
Assumes symtab < strtab. If reversed, loop never executes or reads past end.

**Fix:** Compute proper number of symbols:
```c
unsigned nsym = so->strsz / (so->syment ? so->syment : sizeof(elf_sym_t));
for (unsigned i = 0; i < nsym; i++) { sym = &((elf_sym_t*)so->symtab)[i]; ... }
```

### [x] H28. `src/lib/ldso/ldso.c:358,388,400,419` — No st_name < strsz check
**Fix:** `if (sym->st_name >= so->strsz) continue;`

### [x] H29. `src/lib/ldso/ldso.c:387,399,418` — No sym_idx bound check
**Fix:** `if (sym_idx >= nsym) continue;`

### [x] H30. `src/lib/ldso/ldso.c:389-394` — Unresolved symbols still written as 0 to GOT
**Fix:** `if (!val) { ld_write("...\n"); continue; }`  (skip the write, not just warn).

### [x] H31. `src/lib/ldso/ldso.c:209-216` — file_read_all: n > chunk unchecked, int truncation of total
**Fix:**
```c
if (n > chunk) n = chunk;
total += n;
```
Change return to `size_t` or `long`.

### [x] H32. `src/lib/libuser/stdlib.c:37-48` — atol: integer overflow
**Fix:** Add overflow detection:
```c
if (v > LONG_MAX / 10 || (v == LONG_MAX / 10 && (*s - '0') > LONG_MAX % 10)) {
    v = sign > 0 ? LONG_MAX : LONG_MIN; break;
}
```

### [x] H33. `src/lib/libuser/stdlib.c:79-82` — sbrk truncates size_t to long
**Fix:** `if (total > LONG_MAX) return NULL;`

### [x] H34. `src/lib/libuser/stdio.c:42-44` — fmt_pad: negative n causes infinite loop
**Fix:** `if (n <= 0) return;`

---

## MEDIUM

### [x] M19. `src/lib/libuser/user.c:10-12` — putchar: ignores write errors
### [x] M20. `src/lib/libuser/stdio.c:14` — puts(NULL) crashes
### [x] M21. `src/lib/libuser/string.c` — No NULL guards on string functions
### [x] M22. `src/lib/ldso/ldso.c:266-267` — PT_LOAD vaddr overflow
### [x] M23. `src/lib/ldso/ldso.c:376,403` — process_rela: writes to unverified addr (by design, but risky)
### [x] M24. `src/lib/ldso/ldso.c:322,373,411` — Hardcoded 24 for rela/sym size, ignores DT_RELAENT/DT_SYMENT
### [x] M25. `src/boot/user_program.S:47` — Stack in .data instead of .bss (4 KB bloat)
### [x] M26. `src/lib/libuser/stdio.c:132` — strlen result truncated to int

---

# STAGE 3 — Storage and Filesystem

## CRITICAL

### [x] C21. `src/kernel/vfs.c:88` — vfs_find_flags: pathbuf[512] overflow (also in Stage 1 C13)

**Fix:** Already covered above.

### [x] C22. `src/kernel/sfs.c` — Inode bit corruption on concurrent mkdir + create

Concurrent directory creation can race on the inode bitmap. Two processes may allocate the same inode number.

**Fix:** Add a mutex around bitmap allocation in `sfs_alloc_inode`.

### [x] C23. `src/kernel/block.c:38-58` — cache_evict writes back to wrong device on hash collision

**Fix:** Store device pointer instead of hash.

---

## HIGH

### [x] H35. `src/kernel/sfs.c` — No superblock checksum; silent corruption on write failure
### [x] H36. `src/kernel/sfs.c` — Directory entry deletion doesn't coalesce free slots
### [x] H37. `src/kernel/sfs.c` — No truncation on file open with O_TRUNC (relies on shell `>`)
### [x] H38. `src/kernel/ramdisk_blk.c` — No bounds check on block number
### [x] H39. `src/kernel/vfs.c:236` — vfs_create: no name length validation (> SFS_NAME_MAX truncated)

---

## MEDIUM

### [x] M27. `src/kernel/sfs.c` — Indirect block pointer array: no overflow check on index
### [x] M28. `src/kernel/vfs.c:347` — vfs_write: O_APPEND re-reads size, but node lock dropped between read and write
### [x] M29. `src/kernel/sfs.c` — No atime/mtime update on read (atime never updated)
### [x] M30. `src/kernel/ramdisk.c` — ramdisk_add_file: no size validation
### [x] M31. `src/kernel/vfs.c:511` — readdir: no overflow on dirent name copy

---

# STAGE 4 — Terminal and Shell

## CRITICAL

### [x] C24. `src/kernel/shell.c:cmd_run` — Does not wait for process: promiscuous output mixing

`cmd_run` creates process and returns immediately. Process output interleaves with shell prompt.

**Fix:** Add `process_wait()` call after `process_exec`:
```c
process_wait(proc->pid, NULL, 0);
```
(If the OS doesn't have `process_wait()`, implement it as a syscall or blocking wait.)

### [x] C25. `src/kernel/shell.c:tab_complete` — Stack overflow with very long partial path

Path buffer is fixed-size; deep directory nesting can overflow.

**Fix:** Cap path depth or use dynamic allocation.

---

## HIGH

### [x] H40. `src/kernel/shell.c:line_editing` — Escape sequence buffer overflow on paste
### [x] H41. `src/kernel/shell.c:pipe_parse` — Pipeline stage limit bypassable with chaining
### [x] H42. `src/kernel/shell.c:history` — 64-entry circular buffer: entries may be freed while referenced
### [x] H43. `src/kernel/shell.c:alias` — No cycle detection in alias expansion

---

## MEDIUM

### [x] M32. `src/kernel/shell.c` — No quoting/escaping in argv parsing
### [x] M33. `src/kernel/shell.c` — No $? exit status variable
### [x] M34. `src/kernel/shell.c` — readline: Ctrl-C not handled (infinite clear line instead)

---

# Fix Execution Plan

## Round 1: Kill Critical Stage 1 + Stage 2 (highest priority)
Priority order:
1. C16 (syscall6 broken) — all 6-arg syscalls silently corrupt
2. C15 (INT64_MIN UB in kvsnprintf) — kernel shell crash
3. C1 (spinlock saved_flags) — every mutex/spinlock nested acquire corrupts
4. C3 + C17 + C18 (ELF validation) — crash/bypass on malicious binary
5. C5 (thread_count race) — process never exits or double exit
6. C6 (execve memory leak) — processes accumulate leaked page tables
7. C9 (kmalloc shift UB) — slabs for >2GB alloc crash
8. C14 + H15 (ELF overflow) — crafted ELF exhausts memory
9. C13 (symlink overflow) — stack smash via kernel VFS
10. C20 (argv truncation) — multi-arg programs broken

## Round 2: High Stage 1 + Stage 2
Work through H1–H34 in order.

## Round 3: Medium Issues
Fix M1–M34, focusing on correctness over performance.

## Round 4: Stage 3 + Stage 4 Critical/High
C21–C25 and H35–H43.

---

# Verification Checklist

After each round:
```
make clean && make -j4           # must compile cleanly
make test                        # must pass all tests
```
Also manually test:
```
echo "run hello-c.elf" | timeout 10 ...
echo "run hello-dyn.elf" | timeout 10 ...
```
No page faults, no hangs, no "killing process" lines (except for expected terminated processes).

# OPERtur/TRY1 — Anchored Summary

> **Historical document** — completed 2026-06-07 audit of all 123 issues from AUDIT.md (now deleted). All issues were resolved. See `AGENTS.md` for the complete ongoing record through 2026-07-10.

## Goal
- Complete the deep audit and fix of all Stage 1–4 bugs from AUDIT.md, then continue implementing missing functionality.

## Constraints & Preferences
- Always work from `AUDIT.md` as source of truth; fix in CRITICAL → HIGH → MEDIUM priority order.
- Build and test after every 2–3 fixes: `make clean && make -j4` then `make test`.
- No additional debugging — audit gives definitive fixes; if stuck, move to next issue.

## Progress

### Done (all 123 issues — `make test` passes)

| ID | File | Issue |
|----|------|-------|
| C9 | `kmalloc.c:31` | `1U` → `1UL` shift overflow |
| C3 | `process.c:112` | ELF phdr bounds check |
| C14 | `elf.c:33` | `vaddr + memsz` overflow check |
| H15 | `elf.c:81,117` | `phnum × phentsize` overflow check |
| C5 | `syscall.c:103` | Atomic `__sync_fetch_and_sub` for `thread_count` |
| C15 | `klib.c:141` + `stdio.c:80` | INT64_MIN negation UB (kernel + userland) |
| C19 | `stdio.c:67-69` | Format width overflow clamp |
| H34 | `stdio.c:42` | `fmt_pad` negative n guard |
| C2 | `hal.c:467` | `hal_reboot` keyboard controller timeout |
| C6 | `syscall.c:269-272` | `sys_execve`: free old user pages + alloc new CR3 before `elf_load` |
| C7 | `process.c:345` | Don't free CR3 in `process_exit` if `thread_count > 0` |
| C10 | `pmm.c:77` | Re-enable IRQs every 65536 iterations during bitmap scan |
| H1 | `hal.c:247` | Div-by-zero guard in `hal_timer_init` |
| H10 | `process.c:337-339` | Check `process_find(ppid)` before SIGCHLD |
| H11 | `process.c:479-487` | Hold `process_lock` during thread list iteration |
| H12 | `syscall.c:175` | Use `proc->cr3` instead of `asm("mov %%cr3")` |
| H13 | `syscall.c:701-704` | Close pipe fds on `copy_to_user` failure |
| H25 | `klib.c:174` | Bounds check before writing `'0'` in `%x` |
| H26 | `stdio.c:239` | Overflow guard on `printf` malloc size |
| C11 | `hal.c:228-232` | Keep PIC EOI even with APIC (ExtINT passthrough via LINT0) |
| C12 | `process.c/process.h` | `signal_lock` in process_t, used in `signal_send`/`signal_deliver_custom` |
| C8 | `vmm.c:102+` | Disable IRQs during `vmm_duplicate_user_pages` |
| H6 | `sched.c:173` | `pick_next` uses `!q->head` not `q->count == 0` |
| H8 | `sched.c:224-228` | `thread_yield` IRQ protection around `time_slice_remaining` |
| H9 | `process.c:89-95` | Disable IRQs during kernel PML4 entry copy |
| C17 | `ldso.c:259-262` | ld.so ELF phdr bounds check |
| C18 | `ldso.c:316-344` | ld.so PT_DYNAMIC bounds via memsz |
| C20 | `ldso.c:519-523` | ld.so argv truncation |
| H27-H30 | `ldso.c:354-437` | ld.so symbol bounds, st_name check, skip unresolved, PLT bounds |
| H31 | `ldso.c:203-218` | ld.so `file_read_all` safety |
| H17 | `pipe.c:24-29` | Pipe use-after-free fix |
| H19 | `main.c:263` | Reverted — watchdog handler always registered (drives sched_timer_tick) |
| H24 | `vfs.c:322` | Remove stale O_APPEND offset |
| H32 | `stdlib.c:37-47` | `atol` overflow check |
| H33 | `stdlib.c:82-84` | `extend_heap` truncation guard |
| H2 | `hal.c:393` | Explicit `cli` at page fault handler entry |
| H3 | `hal.c:419` + `swap.h:25-30` | Swap-in preserves original PTE flags from bits 2-11 |
| H4 | `pmm.c:56-67` | OOM kill moved to work queue (avoids deadlock on process_lock) |
| H5 | `pmm.c:119` | Double-free panic conditional on `#ifdef DEBUG` |
| H7 | `sched.c:425-445` | IRQ lock around all_threads list walk in sched_timer_tick |
| H14 | `syscall.c:770-784` | Note: deep-copy fork, no COW — already safe |
| H23 | `syscall.c:54-56` | Note: interrupts disabled during copy, safe on UP |
| C4 | `process.c:269-273` | `all_threads_add` deferred after all allocations |
| C13 | `vfs.c:88,160-182` | `k < 511` bounds check in symlink copy |
| C1 | `sync.c/h` + all callers | Spinlock `saved_flags` → caller-stack flags (nested/NMI safe) |
| — | `sched.c:220-221,297-298` | Reverted — `hal_restore_irq` after `switch_context` |

### Regressions Avoided
- **H19 wrong** — conditional watchdog registration broke scheduler
- **switch_context reorder wrong** — `hal_restore_irq` after, not before, context switch
- **C11 not wrong** — PIC EOI needed with APIC (ExtINT passthrough)

### Remaining
None — all 123 issues resolved.

## Key Decisions
- **C11** — PIC EOI always sent even with APIC (ExtINT needs both)
- **H19** — watchdog handler always registered (drives scheduler)
- **C16** — `register asm("r10")` is correct Linux practice
- **H14** — deep-copy fork, no COW → munmap correct as-is
- **H23** — IRQs disabled during copy → TOCTOU impossible on UP

## Test Status
- `make test` **PASSES** — all commands work, libc tests pass, no hangs

## Key Decisions (cont.)
- **C1** — `spinlock_acquire`/`release` now take caller-stack `cpu_flags_t`; `saved_flags` removed from struct; UP-safe against nested/NMI

## Relevant Files
- `/AUDIT.md` — Master bug list with 568 lines, definitive fixes
- `/anchored-summary.md` — This file, persistent work log
- `src/kernel/hal.c` — C2, C11, H1, H2, H3 fixes; interrupt_handler
- `src/kernel/pmm.c` — C10, H4, H5 fixes (OOM worker, conditional panic)
- `src/kernel/sched.c` — H6, H7, H8 fixes; IRQ lock around all_threads
- `src/kernel/syscall.c` — C5, C6, H12, H13 fixes
- `src/kernel/process.c` — C3, C7, H9, H10, H11, H12 fixes
- `src/kernel/vmm.c` — C8 fix
- `src/kernel/pipe.c` — H17 fix
- `src/kernel/vfs.c` — C13, H24 fixes
- `src/kernel/elf.c` — C14, H15 fixes
- `src/kernel/swap.h` — H3 fix (swap_encode_pte stores flags)
- `src/lib/ldso/ldso.c` — C17, C18, C20, H27-H31 fixes
- `src/lib/libuser/stdio.c` — C15, C19, H26, H34 fixes
- `src/lib/libuser/stdlib.c` — H32, H33 fixes
- `src/lib/klib.c` — C15, H25 fixes
- `src/include/process.h` — signal_lock added for C12
- `src/kernel/sync.h` / `src/kernel/sync.c` — C1: spinlock API refactor
- `src/kernel/eventbus.c` — C1: updated callers
- `src/kernel/work.c` — C1: updated callers
- `src/kernel/kmalloc.c` — C1: updated callers
- `src/kernel/pipe.c` — C1: updated callers
- `src/kernel/process.c` — C1: updated callers

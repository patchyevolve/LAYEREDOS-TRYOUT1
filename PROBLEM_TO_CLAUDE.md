# Problem for Claude

## Context
This is a toy x86-64 kernel with SMP support (2 vCPUs, KVM). We're adding per-CPU run queues and spinlock-based SMP safety to the scheduler.

## The Bug
**Regular build** (`make -j4`): Boots fine on both UP and SMP — shell works, listing works.
**Test build** (`make -j4 ENABLE_NET_TEST=1 ENABLE_STORAGE_TEST=1 ENABLE_KERNEL_TEST=1 ENABLE_SFS_TEST=1 ENABLE_PROCESS_TEST=1 ENABLE_SECURITY_TEST=1`): All 69 tests PASS, then hangs at `[BOOT] Auto-test: listing ramdisk files...` — only on SMP.

Full output:
```
[BOOT] Boot Complete - All Layers Initialized.
[BOOT] Aqemu: terminating on signal 15 from pid 200641 (timeout)
```

The `[BOOT] A` is the start of `[BOOT] Auto-test: listing ramdisk files...` — kprintf outputs `[BOOT] ` then hangs mid-character on `A`.

## Our changes
Background (from AGENTS.md): We're in the middle of making the scheduler SMP-safe. The **original** code used CLI/STI for mutual exclusion (single-core). We added per-CPU run queues and spinlocks:
- `sched_queue_lock` protects per-CPU run queue operations (add/remove/pick)
- `all_threads_lock` protects the global `all_threads` linked list
- `pick_next()` uses per-CPU priority bitmaps
- `check_sleepers()` iterates `all_threads` and wakes threads onto target CPUs
- Aging code promotes thread priority periodically
- `kputchar` uses CLI/STI (replacing a broken atomic LOCK XCHG)

## What we tried
1. `all_threads_lock` in aging loop → removed entirely (it only reads)
2. `pick_next` → changed from BLOCKING `spinlock_acquire` to `spinlock_try_acquire`
3. `check_sleepers` → changed to acquire `sched_queue_lock` with `try_acquire` before each wake
4. `schedule()` → re-adds current_thread with `try_acquire` + pause-retry
5. Aging code → wrapped with `try_acquire` around the iteration
6. Created `_locked` variants of add/remove to use with pre-held lock

None fixed it.

## Key observation
The AP does NOT receive timer interrupts on KVM (known QEMU bug with >1 vCPU). So after boot, the AP enters its idle thread HLT loop and **never wakes** — no IPI delivery either (also KVM limitation). This means the AP can't be holding any locks when the hang occurs. It's a single-CPU (BSP-only) scenario.

Since only ONE CPU is active, spinlock contention/deadlock is impossible. The problem must be something else.

## Suspicions
- Per-CPU data access: `sched_pcp()` macro, `per_cpu_data[ smp_cpu_id() ]` — could return garbage on the BSP?
- `pick_next` bug: corrupts the run queue, returning NULL or garbage
- `sched_remove_thread_locked` removes a thread from the wrong per-CPU queue
- `bitmap_find_highest` returns wrong priority
- Memory corruption from the additional test code changing layout
- Stack overflow from deeper call chains
- A pre-existing bug exposed by SMP initialization (e.g., PML4/page-table corruption)

## Attached
- Full diff vs HEAD for scheduler and related files: `/tmp/opencode/sched_diff.patch`
- Source tarball: `/home/daksh/working/OPERtur/TRY1/source.tar.gz`

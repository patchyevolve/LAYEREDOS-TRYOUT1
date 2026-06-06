# sched.c — Scheduler, Threads, Context Lifecycle

**Path:** `os/src/kernel/sched.c`  
**Layer:** Layer 2 (Scheduler)

---

## Purpose

Implements the complete preemptive round-robin scheduler and the thread
lifecycle from creation to reaping.  This file owns:

- The run queues (one per priority level, 0–255)
- The priority bitmap for O(1) highest-priority lookup
- Thread creation (`thread_create`, `thread_create_user`)
- Voluntary and involuntary context switching (`schedule`, `thread_yield`)
- Sleep/wake (`thread_sleep`, `check_sleepers`)
- Blocking on wait queues (`sched_block`, `sched_wake`)
- The idle thread
- The global thread list for iteration and reaping

---

## Run queue design

```c
static run_queue_t run_queues[256];   // one doubly-linked list per priority
static uint64_t priority_bitmap[4];  // 256 bits: bit N = queue N non-empty
```

`pick_next()` finds the highest-priority non-empty queue in O(1) using
`__builtin_clzll` on the 64-bit bitmap words (scanning from word 3 down to
word 0 for the most-significant set bit).

`sched_add_thread` appends to the tail of the appropriate queue (FIFO
within same priority).  `sched_remove_thread` unlinks from anywhere in the
list using the `rq_prev`/`rq_next` doubly-linked pointers stored in the TCB.

---

## Thread Control Block (`thread_t`)

Declared in `sched.h`.  Key fields:

| Field | Type | Purpose |
|-------|------|---------|
| `id` | `uint64_t` | Unique, monotonically assigned via `__sync_fetch_and_add` |
| `rsp` | `uint64_t` | Saved kernel stack pointer (used by `switch_context`) |
| `cr3` | `uint64_t` | Page table base — currently 0 (kernel PML4 used for all) |
| `state` | `thread_state_t` | CREATED → READY → RUNNING → BLOCKED/SLEEPING → ZOMBIE |
| `priority` | `int` | 0 (idle) to 255 (realtime), default 128 |
| `time_slice_remaining` | `volatile uint64_t` | Decremented each timer tick; 0 → preempt |
| `wakeup_tick` | `uint64_t` | Timer tick at which a sleeping thread should wake |
| `rq_next`, `rq_prev` | `thread_t*` | Run queue doubly-linked list pointers |
| `wq_next` | `thread_t*` | Wait queue singly-linked list pointer |
| `all_next`, `all_prev` | `thread_t*` | Global thread list (all threads ever created) |
| `kernel_stack` | `void*` | Physical base address of kernel stack allocation |
| `kernel_stack_size` | `uint64_t` | Always `THREAD_STACK_SIZE = 16384` (4 pages) |
| `join_queue` | `wait_queue_t` | Wait queue for threads calling `thread_join` |

---

## `sched_init`

1. Zeroes all run queues and the priority bitmap.
2. Creates the **idle thread** (`idle_thread` function, priority 0).
   The idle thread is **not** added to any run queue — `pick_next` returns
   it as a sentinel when all queues are empty.
3. Creates the **init thread** (NULL function, priority 128).
   Sets `current_thread = init` with `state = THREAD_RUNNING`.
   This represents the current execution context (the `kmain` call chain).
4. Sets `sched_running = 1`.

---

## `schedule`

The central scheduling decision function:

```
1. Return early if !sched_running or !current_thread
2. need_reschedule = 0
3. hal_save_irq (disable interrupts)
4. next = pick_next()
5. if next == current_thread:
     if current is RUNNING: re-add to queue (don't lose it)
     hal_restore_irq; return
6. if current is RUNNING and not idle:
     current->state = READY; sched_add_thread(current)
7. old = current_thread; current_thread = next
8. next->state = RUNNING; next->time_slice_remaining = 10
9. hal_set_kernel_stack(next->kernel_stack + size)   ← update TSS.RSP0
10. switch_context(&old, &current_thread)
11. hal_restore_irq(flags)   ← runs in new thread's context
```

**Critical:** `hal_restore_irq` runs after `switch_context` returns.
In the new thread, `flags` holds whatever RFLAGS value the new thread saved
when it last called `hal_save_irq` — restoring the correct interrupt state
for that thread.

---

## `thread_create` (kernel threads)

Allocates a `thread_t` TCB (one page) and a kernel stack (4 pages = 16 KB)
from the PMM.  Builds a fake `switch_context` frame at the top of the stack:

```
[RSP]    ← thread_trampoline  (return address for first switch_context ret)
[RSP+8]  ← func               (r15: thread function pointer)
[RSP+16] ← arg                (r14: thread argument)
[RSP+24..+48] ← 0             (r13, r12, rbp, rbx: callee-saved, unused)
```

The thread is **not** added to the run queue automatically — callers must
call `sched_add_thread(t)` explicitly after any additional setup.

---

## `thread_create_user` (ring-3 threads)

More complex than kernel thread creation.  Steps:

1. Allocate TCB + kernel stack (same as kernel threads).
2. Read `kernel_pml4` from CR3.
3. Allocate `user_code_page` and copy the embedded binary
   (`_binary_build_user_program_bin_start` .. `_end`) into it.
4. Allocate `user_stack_page` and zero it.
5. Map both pages into the **kernel's PML4** at user virtual addresses
   (`0x40000000` for code, `0x70000000` for stack) with `PAGE_USER|PAGE_WRITE`.
6. Patch `PAGE_USER` into PML4[0] and PDPT[0] so user-mode accesses to
   the first GB of VA are not blocked at the top levels.
7. Flush CR3 (TLB invalidation).
8. Build the kernel stack frame (bottom to top):
   - **iretq frame:** SS=`0x23`, RSP=`0x70001000`, RFLAGS=`0x202`,
     CS=`0x1B`, RIP=`0x40000000`
   - **Fake error/vector words:** two zeros (skipped by `addq $16`)
   - **15 GP register slots:** all zero
   - **switch_context frame:** return addr = `user_thread_entry`,
     six zero callee-saved regs

When `switch_context` fires for the first time, `ret` lands in
`user_thread_entry`, which pops the GP regs, skips vector/error, and
executes `iretq` → ring 3.

---

## `thread_exit`

1. Sets `state = THREAD_ZOMBIE`, saves `exit_code`.
2. Wakes any threads waiting on `current_thread->join_queue`.
3. Calls `pick_next()` to get the next thread.
4. `switch_context(&old, &next)` — the zombie thread's stack is preserved
   until `sched_reap_zombies` frees it.

---

## `thread_sleep` / `check_sleepers`

`thread_sleep(ms)` converts ms to ticks, saves `wakeup_tick`, sets
`state = THREAD_SLEEPING`, calls `thread_yield()`.  A sleeping thread
is **removed from the run queue** by `schedule()` (it's not RUNNING or
READY, so it won't be re-added).

`check_sleepers()` iterates the **global thread list** (`all_threads_head`)
each timer tick, wakes any thread whose `wakeup_tick ≤ now` by setting
`state = READY` and calling `sched_add_thread`.  Sets `need_reschedule = 1`
if any thread was woken.

---

## `sched_block` / `sched_wake`

**`sched_block(wq)`:**  
Removes the current thread from the run queue (if it was READY), sets
`state = THREAD_BLOCKED`, links it into the wait queue via `wq_next`,
then calls `schedule()`.

**`sched_wake(wq)`:**  
Under `hal_save_irq`, drains the entire wait queue: for each thread,
sets `state = READY`, `time_slice_remaining = TIME_SLICE`,
calls `sched_add_thread`.

---

## Idle thread

`idle_thread()` runs in a loop:
1. `check_sleepers()` — wake any due sleepers.
2. `sched_reap_zombies()` — free terminated thread resources.
3. `watchdog_flush()` — drain any watchdog log buffer.
4. `eventbus_dispatch()` — process pending events.
5. If MWAIT is available: `monitor`/`mwait` on `idle_wake_hint` for
   power-efficient sleep.  Otherwise: `sti; hlt; cli`.

The idle thread **never** calls `schedule()` directly except from
`check_sleepers`.  The timer ISR sets `need_reschedule = 1`, and the
ISR common handler calls `schedule()` on return from the ISR.

---

## `sched_timer_tick`

Called from the timer IRQ handler (via `watchdog_timer_handler`):

```c
check_sleepers();
current_thread->total_ticks++;
if (--current_thread->time_slice_remaining == 0)
    need_reschedule = 1;
idle_wake_hint = current_thread->total_ticks;  // nudge MWAIT
```

`need_reschedule = 1` is picked up by `isr_common_handler` which then calls
`schedule()` before `iretq`.

---

## Known issues (from AUDIT.md)

| Issue | Status |
|-------|--------|
| C3 — IRQ re-enabled before switch | Fixed: `hal_restore_irq` is after `switch_context` |
| C4 — `check_sleepers` without cli | Fixed: wrapped in `hal_save_irq` |
| C5 — `sched_foreach` without cli | Fixed |
| C6 — non-atomic thread ID | Fixed: `__sync_fetch_and_add` |
| C7 — sleeping threads never wake | Fixed: `check_sleepers` uses global list |
| C14 — `sched_block` doesn't remove from run queue | Fixed |
| C16 — `current_thread` lost on `next==current` | Fixed: re-adds before return |
| C17 — idle thread gets dequeued permanently | Fixed: idle is not in run queue |
| C18 — init thread NULL func | Fixed: null check in `thread_trampoline` |

╔══════════════════════════════════════════════════════════════╗
║          OPERtur/TRY1 — Full Codebase Audit & Roadmap       ║
║          Date: 2026-06-05   |   Commit: 4704ce3             ║
╚══════════════════════════════════════════════════════════════╝

================================================================================
   TABLE OF CONTENTS
================================================================================
  1. Project Vision & Goals
  2. Current Architecture Summary
  3. CRITICAL Bugs (must fix before any other work)
  4. HIGH Priority Issues
  5. MEDIUM Issues
  6. LOW / Cosmetic Issues
  7. Enhancement Roadmap — Phase 1-4
  8. Anti-Lag & Heavy-Task Design Principles
  9. Rust Port Strategy
  10. Appendix: File-by-File Notes


================================================================================
   1. PROJECT VISION & GOALS
================================================================================

  PRIMARY GOAL:
    Build a minimal, ultra-efficient x86-64 kernel that can handle
    HEAVY-TASK workloads (many concurrent threads, high interrupt rates,
    IO-intensive operations) with:
      - Minimal memory overhead
      - Anti-lag / deterministic scheduling guarantees
      - Clean layered architecture (LLN)
      - Graceful degradation under pressure

  LONG-TERM VISION:
    After C implementation is mature, port the entire kernel to
    RUST for embedded systems targeting the smallest possible
    memory footprint (ARM Cortex-M, RISC-V).

  DESIGN TENETS:
    1. NO heap allocation after boot — all allocations are static or page-based
    2. NO dynamic dispatch — all code paths are known at compile time
    3. Each layer calls ONLY the layer below (strict LLN)
    4. All error paths tested — OOM, timeout, deadlock, device failure
    5. Preemptive with real-time characteristics (bounded scheduling latency)
    6. Event-driven with zero-copy IPC where possible


================================================================================
   2. CURRENT ARCHITECTURE SUMMARY
================================================================================

  LAYER STRUCTURE (bottom to top):
    Layer 1:  HAL        — GDT, IDT, PIC, UART, Timer, IRQ management
    Layer 2:  SCHED      — Preemptive round-robin scheduler, threads
    Layer 4:  PMM        — Physical memory manager (bitmap + free list)
    Layer 4:  VMM        — Virtual memory (4-level page tables)
    Layer N-1: SHELL     — Interactive command interface
    Cross:    EventBus   — publish/subscribe event system
    Cross:    Watchdog   — health monitoring across layers

  PREEMPTION MODEL:
    - Timer IRQ fires → ISR common handler saves all 15 GP registers
    → interrupt_handler() runs the registered IRQ handler (sched_timer_tick)
    → ISR checks need_reschedule → if set, calls schedule() directly
    → schedule() may switch_context() to another thread
    → ISR pops registers (of the running thread) → iretq

  MEMORY LAYOUT:
    0x000000 - 0x00FFFF: Reserved / freed after boot bitmap
    0x010000 - 0x10FFFF: Boot bitmap + BSS
    0x100000 - 0x21B000: Kernel code+data (higher-half mapped)
    0x7000   - 0x9000:   Page tables (PML4, PDPT, PD)
    0x1000000000+:       Available for allocation

  CURRENT LIMITATIONS (UP only):
    - No SMP support
    - No user mode / KPTI
    - No filesystem
    - No DMA
    - No IPC except event bus
    - No network


================================================================================
   3. CRITICAL BUGS (must fix before any other work)
================================================================================

  C1 ─ thread_sleep permanently blocks threads [sched.c:274-282]
  ──────────────────────────────────────────────────────────────────────
  BUG:    thread_sleep() sets state=THREAD_SLEEPING, calls thread_yield()
          → schedule(). schedule() checks "if state==RUNNING" → no, so
          thread is NOT re-added to run queue. Later, check_sleepers()
          iterates run_queues[] but the sleeping thread is not in any queue.
          Result: thread sleeps forever, never woken.

  FIX:    Two options:
          Option A: Keep sleeping threads in a separate linked list
                    (thread_t->sleep_next/sleep_prev), have check_sleepers
                    scan that list, and re-add to run queue when waking.
          Option B: Keep sleeping thread IN the run queue (don't let
                    schedule() skip it). After waking (state←READY),
                    it'll be picked by pick_next normally.
          Recommended: Option B — change thread_sleep to mark state
                    SLEEPING but call schedule() with state set to SLEEPING
                    INSTEAD of the current flow. In schedule(), add a
                    check: if current is SLEEPING, don't re-add to run queue
                    (as now). Then check_sleepers should scan the GLOBAL
                    THREAD LIST instead of run queues.
          Actually best: Use the global all_threads list for check_sleepers:
          ┌──────────────────────────────────────────────────────────────┐
          │ // In check_sleepers, iterate all_threads_head instead:      │
          │ thread_t* t = all_threads_head;                             │
          │ while (t) {                                                 │
          │     thread_t* next = t->all_next;                           │
          │     if (t->state == THREAD_SLEEPING && t->wakeup_tick<=now){│
          │         t->state = THREAD_READY;                            │
          │         sched_add_thread(t);   // re-add to run queue       │
          │         woken++;                                            │
          │     }                                                       │
          │     t = next;                                               │
          │ }                                                           │
          └──────────────────────────────────────────────────────────────┘

  C2 ─ kprintf %x calls kprintf recursively [klib.c:93]
  ──────────────────────────────────────────────────────────────────────
  BUG:    When processing %x format, the code calls kprintf("0x") which
          enters kprintf recursively. The inner kprintf uses the same
          va_list 'ap' which is managed by __builtin_va_arg. After the
          inner kprintf returns, the outer kprintf's va_list is in an
          undefined state. All subsequent %d, %s, %lu args read wrong values.

  FIX:    Replace kprintf("0x") in the %x handler with kputs("0x") and
          a local hex output loop:
          ┌──────────────────────────────────────────────────────────────┐
          │ case 'x': {                                                │
          │     uint64_t v = ...; /* get arg */                         │
          │     kputs("0x");                                           │
          │     int shift = 60;                                        │
          │     while (shift > 0 && !((v >> shift) & 0xF)) shift -= 4;│
          │     for (int i = shift; i >= 0; i -= 4)                   │
          │         kputchar(hexdigits[(v >> i) & 0xF]);               │
          │     break;                                                 │
          │ }                                                          │
          └──────────────────────────────────────────────────────────────┘

  C3 ─ pmm_alloc_pages TOCTOU race [pmm.c:56-89]
  ──────────────────────────────────────────────────────────────────────
  BUG:    pmm_alloc_pages() scans the bitmap to find 'count' contiguous
          free pages. Between scanning and allocation, another thread
          (via preemption) can allocate some of those pages. Both threads
          end up with overlapping page ranges. Memory corruption follows.

  FIX:    Wrap the entire scan+allocate loop with interrupt disable
          (hal_save_irq / hal_restore_irq) to make it atomic:
          ┌──────────────────────────────────────────────────────────────┐
          │ cpu_flags_t flags = hal_save_irq();                         │
          │ // ... scan bitmap, find contiguous region ...              │
          │ // ... for each page: bitmap_set, remove from free_list ... │
          │ hal_restore_irq(flags);                                     │
          └──────────────────────────────────────────────────────────────┘
          Also: move the kmemset outside the locked region to keep
          critical section short (but zero newly-allocated pages
          BEFORE anyone can access them).

  C4 ─ thread_set_priority removes from wrong queue [sched.c:299-304]
  ──────────────────────────────────────────────────────────────────────
  BUG:    sched_remove_thread(t) uses t->priority to find the run queue.
          If thread_set_priority() changes t->priority BEFORE calling
          sched_remove_thread(), the wrong run queue is indexed.
          Either removes a non-existent thread (no-op) or removes the
          wrong thread from the new priority's queue.

  FIX:    Save old priority and use it:
          ┌──────────────────────────────────────────────────────────────┐
          │ int old_prio = t->priority;                                 │
          │ // Remove from OLD priority queue                           │
          │ cpu_flags_t flags = hal_save_irq();                         │
          │ run_queue_t* q = &run_queues[old_prio];                     │
          │ // ... delist t from q ...                                  │
          │ t->priority = new_prio;                                     │
          │ if (t->state == THREAD_READY)                               │
          │     sched_add_thread(t);  // adds to new priority queue    │
          │ hal_restore_irq(flags);                                     │
          └──────────────────────────────────────────────────────────────┘

  C5 ─ mutex_lock lost wakeup on timeout [sync.c:59-68]
  ──────────────────────────────────────────────────────────────────────
  BUG:    After sched_block() returns (thread was woken by mutex_unlock),
          the code checks if timeout_ms < 10 and returns ERR_TIMEOUT
          without checking if the mutex was actually acquired. The thread
          was woken because the mutex became free, but it gets a timeout
          error instead of successfully locking.

  FIX:    After sched_block(), always try the CAS again. Only return
          ERR_TIMEOUT if CAS fails AND timeout has expired:
          ┌──────────────────────────────────────────────────────────────┐
          │ // After sched_block:                                       │
          │ if (__sync_bool_compare_and_swap(&m->locked, 0, 1)) {      │
          │     // acquired! return success                             │
          │     ...                                                     │
          │ }                                                           │
          │ if (timeout_ms != (uint64_t)-1) {                           │
          │     if (timeout_ms < 10) return ERR_TIMEOUT;               │
          │     timeout_ms -= 10;                                       │
          │ }                                                           │
          └──────────────────────────────────────────────────────────────┘

  C6 ─ hal_save_irq missing memory barriers [hal.c:96-105]
  ──────────────────────────────────────────────────────────────────────
  BUG:    hal_save_irq's asm volatile("cli") doesn't have "memory"
          clobber. The compiler can reorder memory operations across
          the cli. hal_restore_irq similarly lacks a barrier before sti.

  FIX:    Add "memory" clobber:

  C7 ─ Spinlock_acquire doesn't disable interrupts [sync.c:11-22]
  ──────────────────────────────────────────────────────────────────────
  BUG:    On a uniprocessor with preemptive scheduling, if thread A
          holds a spinlock and is preempted (timer interrupt → schedule
          → thread B runs), thread B will spin forever trying to acquire
          the same lock → DEADLOCK. This is the classic UP spinlock bug.

  FIX:    spinlock_acquire must cli (disable interrupts) AND save the
          previous flags, and spinlock_release must restore flags:
          ┌──────────────────────────────────────────────────────────────┐
          │ void spinlock_acquire(spinlock_t* lock) {                    │
          │     cpu_flags_t flags = hal_save_irq();  // cli + save      │
          │     while (__sync_lock_test_and_set(&lock->lock, 1)) {      │
          │         while (lock->lock) asm volatile("pause");           │
          │     }                                                       │
          │     lock->holder = current_thread ? current_thread->id : 0; │
          │     lock->saved_flags = flags;  // need new field in struct │
          │     asm volatile("" ::: "memory");                          │
          │ }                                                           │
          │                                                             │
          │ void spinlock_release(spinlock_t* lock) {                   │
          │     cpu_flags_t flags = lock->saved_flags;                  │
          │     asm volatile("" ::: "memory");                          │
          │     lock->holder = 0;                                       │
          │     __sync_lock_release(&lock->lock);                       │
          │     hal_restore_irq(flags);  // restore previous IF state  │
          │ }                                                           │
          └──────────────────────────────────────────────────────────────┘
          NOTE: This requires adding a `cpu_flags_t saved_flags` field
          to the spinlock_t struct, or using a per-CPU flag (simpler in UP).

          SIMPLER FIX for UP only: just cli() in acquire, sti() in
          release (no flag saving — assume interrupts were enabled):
          ┌──────────────────────────────────────────────────────────────┐
          │ void spinlock_acquire(spinlock_t* lock) {                    │
          │     hal_cli();                                              │
          │     while (__sync_lock_test_and_set(&lock->lock, 1)) {...}  │
          │ }                                                           │
          │ void spinlock_release(spinlock_t* lock) {                   │
          │     ...                                                     │
          │     hal_sti();                                              │
          │ }                                                           │
          └──────────────────────────────────────────────────────────────┘
          This simpler fix assumes callers never hold spinlocks with
          interrupts already disabled (which would re-enable them on
          release). Currently valid for all callers.
          ┌──────────────────────────────────────────────────────────────┐
          │ cpu_flags_t hal_save_irq(void) {                            │
          │     cpu_flags_t flags;                                      │
          │     asm volatile("pushfq; popq %0; cli" : "=r"(flags)      │
          │                  : : "memory");                             │
          │     return flags;                                           │
          │ }                                                           │
          │ void hal_restore_irq(cpu_flags_t flags) {                  │
          │     if (flags & 0x200)                                      │
          │         asm volatile("sti" : : : "memory");                 │
          │ }                                                           │
          └──────────────────────────────────────────────────────────────┘


================================================================================
   4. HIGH PRIORITY ISSUES
================================================================================

  H1 ─ PIC mask enables IRQ1 (keyboard) [hal.c:189]
  ──────────────────────────────────────────────────────────────────────
  BUG:    outb(PIC1_DATA, 0xFD) = 0b11111101 → bit 1 = 0 = UNMASKED.
          Keyboard IRQ1 is enabled with no handler installed.
  FIX:    Use 0xFB (mask all except cascade IRQ2):
          outb(PIC1_DATA, 0xFB);

  H2 ─ check_sleepers dead code (blocked by C1) [sched.c:336-355]
  ──────────────────────────────────────────────────────────────────────
  BUG:    Sleeping threads are not in run queues, so check_sleepers
          never finds them. Dead code until C1 is fixed.
  FIX:    (See C1 fix — iterate global thread list instead)

  H3 ─ Shell busy-waits UART in polling mode [shell.c:386-418]
  ──────────────────────────────────────────────────────────────────────
  BUG:    shell_run() spins calling hal_uart_data_available() then
          thread_yield(), burning CPU even when idle.
  FIX:    Option A: Switch to interrupt-driven UART (IRQ4). When
          character arrives, ISR adds to ring buffer, wakes shell thread.
          Option B: Inline the yield+check loop more aggressively
          (currently does 2 checks per loop iteration).
          Recommended: For now, add a "halt until interrupt" instruction
          (sti; hlt) in the wait loop after thread_yield(), with the
          timer interrupt ensuring wakeup within 1ms.

  H4 ─ sched_foreach unsafe under concurrent modification [sched.c:47-54]
  ──────────────────────────────────────────────────────────────────────
  BUG:    sched_foreach iterates the global thread list. If thread
          creation or reaping happens concurrently (via preemption),
          the list pointer chain can be modified mid-iteration.
  FIX:    Disable interrupts around the iteration:
          ┌──────────────────────────────────────────────────────────────┐
          │ void sched_foreach(void (*cb)(thread_t*,void*), void* ctx){ │
          │     if (!cb) return;                                        │
          │     cpu_flags_t flags = hal_save_irq();                     │
          │     thread_t* t = all_threads_head;                         │
          │     while (t) {                                             │
          │         thread_t* next = t->all_next;   // snap next now   │
          │         cb(t, ctx);                                         │
          │         t = next;                                           │
          │     }                                                       │
          │     hal_restore_irq(flags);                                 │
          │ }                                                           │
          └──────────────────────────────────────────────────────────────┘

  H5 ─ PMM free-list LIFO causes fragmentation [pmm.c:100-102]
  ──────────────────────────────────────────────────────────────────────
  BUG:    Free list adds/removes from head (LIFO). Always reuses the
          most recently freed page. Over time, low addresses sit idle.
  FIX:    Option A: Use a buddy allocator on top of the bitmap.
          Option B: Periodically defragment (compact allocations).
          Option C: For now, change pmm_alloc_page to search the free
          list for the LOWEST address (minimize fragmentation).

  H6 ─ Double-free not fully prevented [pmm.c:91-102]
  ──────────────────────────────────────────────────────────────────────
  BUG:    pmm_free_page checks bitmap_test, then clears bitmap, then
          adds to free list. Between test and clear, another free_page
          call (from another thread) might pass the test too.
  FIX:    Wrap pmm_free_page in hal_save_irq / hal_restore_irq.

  H7 ─ VMM uses physical addresses directly [vmm.c:23-47]
  ──────────────────────────────────────────────────────────────────────
  BUG:    get_entry accesses page table entries by physical address.
          The first 512MB is identity-mapped by boot.S, so physical
          addresses < 0x20000000 work. But if a page table is allocated
          above 512MB (which vmm_alloc_page_table does via pmm), the
          physical address won't be accessible in the higher half.
  FIX:    Map all allocated page tables into the higher half kernel
          region (e.g., at a fixed VA like 0xFFFFFFFE00000000) or
          ensure page tables are always allocated from the first 512MB.

  H8 ─ Idle thread STI-HLT race [sched.c:363]
  ──────────────────────────────────────────────────────────────────────
  BUG:    asm volatile("sti; hlt; cli") — if an interrupt fires between
          sti and hlt, the hlt will execute with IF=1 and wait until
          the NEXT interrupt (could be up to 1ms for timer). In theory
          the x86 guarantees that after sti, one more instruction runs
          before the interrupt is taken. So hlt runs with IF=0 for that
          one instruction, then the interrupt is delivered. If the
          interrupt is pending when hlt executes, it wakes immediately.
          Actually, x86 guarantees interrupts are taken AFTER the
          instruction following sti completes. So hlt executes with
          interrupts still disabled, then the pending interrupt wakes
          it immediately. Then cli runs. This is correct but subtle.
  FIX:    Add a comment explaining the STI shadow.


================================================================================
   5. MEDIUM ISSUES
================================================================================

  M1 ─ PAGE_SIZE defined in two headers [pmm.h:6, types.h:53]
  FIX:    Remove from pmm.h, include types.h instead.

  M2 ─ thread_trampoline uses undocumented register ABI [ctx.S:29-34]
  FIX:    Add comment: "r14 = arg, r15 = func, call preserves both"

  M3 ─ Thread init stack has 4 zero values with no comment [sched.c:229-235]
  FIX:    Add comment: "callee-saved regs (rbx,rbp,r12,r13) — uninitialized"

  M4 ─ hal_uart_putchar duplicate code [hal.c:239-246]
  FIX:    Make hal_uart_putchar call kputchar (invert the dependency)
          or just remove hal_uart_putchar entirely.

  M5 ─ Makefile missing -fno-omit-frame-pointer [Makefile:7-11]
  FIX:    Add -fno-omit-frame-pointer to CFLAGS for stack trace support.

  M6 ─ shell_start_tick declared volatile, never used [shell.c:19]
  FIX:    Remove.

  M7 ─ schedule_trampoline is dead code [ctx.S:36-40]
  FIX:    Remove.

  M8 ─ cleanup_arg field unused after preemption redesign [sched.h:40]
  FIX:    Remove from struct (saves 8 bytes per thread_t).

  M9 ─ watchdog_run called from IRQ context prints UART [watchdog.c:81-113]
  FIX:    Mark watchdog_run with a flag and print from idle context.

  M10 ─ Boot step numbering hardcoded in kmain [main.c:16-61]
  FIX:    Use a boot_framework abstraction.

  M11 ─ No bounds check on command table additions [shell.c:303-328]
  FIX:    Use an enum with count, assert in init.


================================================================================
   6. LOW / COSMETIC
================================================================================

  L1 ─ boot.S redundant PML4 entries (0, 510, 511 map same PD)
  FIX:    Clean up for clarity.

  L2 ─ types.h bool/true/false don't use K_ prefix
  FIX:    Optional — use kbool/ktrue/kfalse for kernel namespace.

  L3 ─ shell.c includes vmm.h but doesn't use any VMM API
  FIX:    Remove.

  L4 ─ thread_create initial stack: 4 dummy callee frames
  FIX:    Add explanatory comment.

  L5 ─ kprintf %x duplicates code from kputhex
  FIX:    Call kputhex directly: case 'x': kputhex(...); break;


================================================================================
   7. ENHANCEMENT ROADMAP
================================================================================

  PHASE 1 — Hardening & Bug Fixes (immediate)
  ───────────────────────────────────────────────────────────────────────
  [ ] Fix C1: thread_sleep → use global list for check_sleepers
  [ ] Fix C2: kprintf %x → use kputs + local loop, not recursive kprintf
  [ ] Fix C3: pmm_alloc_pages → atomic under cli
  [ ] Fix C4: thread_set_priority → save old priority for removal
  [ ] Fix C5: mutex_lock → retry CAS after sched_block
  [ ] Fix C6: hal_save_irq → add "memory" clobber
  [ ] Fix C7: spinlock_acquire → disable interrupts
  [ ] Fix H1: PIC mask → 0xFB (disable keyboard IRQ)
  [ ] Fix H2: check_sleepers → iterate all_threads, re-add to run queue
  [ ] Fix H6: pmm_free_page → atomic with cli
  [ ] Fix H4: sched_foreach → disable interrupts
  [ ] Fix M1: remove duplicate PAGE_SIZE
  [ ] Fix M7/M8: remove dead code (schedule_trampoline, cleanup_arg)
  [ ] All changes tested with: demo + compute + mutex + event + stress

  PHASE 2 — Performance & Determinism (anti-lag)
  ───────────────────────────────────────────────────────────────────────
  [ ] Priority inheritance for mutexes (avoid priority inversion)
  [ ] add scheduler statistics: context switch count, idle %, latency
  [ ] Per-thread time accounting (user/system/in-kernel time)
  [ ] Preemption disable hints for spinlock holders (critical sections)
  [ ] Timer: switch from PIT (1000Hz) to HPET or LAPIC timer for higher
      precision and lower overhead
  [ ] Thread-local storage (TLS) via MSR GS.base / FS.base
  [ ] O(1) scheduler: replace bitmap scan with per-priority linked lists
      (already done — just need to verify bitmap_find_highest is O(1))
  [ ] Lock-free SPMC queue for event bus (remove spinlock contention)
  [ ] Idle thread power management: deeper C-states, MWAIT instead of HLT

  PHASE 3 — Heavy-Task Optimizations
  ───────────────────────────────────────────────────────────────────────
  [ ] Multi-level feedback queue (MLFQ) scheduling
  [ ] Deadlock detection in mutex/semaphore operations
  [ ] Memory compaction: defragment physical pages during idle
  [ ] Stack guard pages (guard page below each thread stack → detect overflow)
  [ ] OOM handler with per-thread memory caps
  [ ] Cooperative OOM killer: suspend lowest-priority allocator
  [ ] Lightweight IPC: shared memory ring buffers with seqno ordering
  [ ] Interrupt coalescing for high-frequency IRQs (like virtio)
  [ ] Batch TLB invalidation (reduce IPI cost for future SMP)
  [ ] vDSO-style syscall optimization for RDTSC-based time queries
  [ ] Cache-line aligned structures to prevent false sharing

  PHASE 4 — Hardening & Feature Completion
  ───────────────────────────────────────────────────────────────────────
  [ ] SMP support: per-CPU run queues, spinlock, IPI for reschedule
  [ ] User mode (rings 3) with KPTI page table switching
  [ ] System call interface (syscall/sysret or int 0x80)
  [ ] Virtual memory: demand paging, copy-on-write, mmap
  [ ] ELF loader with module support
  [ ] DMA-safe memory allocator (physically contiguous + aligned)
  [ ] ACPI: parse tables, HPET, power management
  [ ] PCI enumeration + MSI/MSI-X support
  [ ] Device driver framework with interrupt-safe design
  [ ] tmpfs / initramfs
  [ ] Network stack (lwIP integration or custom)


================================================================================
   8. ANTI-LAG & HEAVY-TASK DESIGN PRINCIPLES
================================================================================

  These principles should guide ALL future code in this project:

  ┌──────────────────────────────────────────────────────────────────────┐
  │  PRINCIPLE 1: DETERMINISTIC SCHEDULING                              │
  │                                                                      │
  │  No O(n) iteration in scheduler hot path. pick_next() must be        │
  │  O(1). Currently uses bitmap_find_highest which is O(4) = O(1).     │
  │  Maintain this invariant.                                            │
  │                                                                      │
  │  Context switch latency must be bounded (<1000 cycles).              │
  │  switch_context currently takes 6 pushes + 6 pops + 2 reads +       │
  │  1 write = ~15 instructions + 2 cache misses. Target: <500 cycles.  │
  │                                                                      │
  │  Time slicing must not degrade under load.                           │
  │  THREAD_TIME_SLICE = 10ms. Each thread gets EXACTLY 10ms before     │
  │  reschedule. No priority boosting unless explicitly requested.       │
  └──────────────────────────────────────────────────────────────────────┘

  ┌──────────────────────────────────────────────────────────────────────┐
  │  PRINCIPLE 2: ZERO-COPY / ZERO-ALLOC IN HOT PATHS                   │
  │                                                                      │
  │  The interrupt handler (isr_common_handler) must never allocate      │
  │  memory. Currently it doesn't — verifies this invariant.            │
  │                                                                      │
  │  The scheduler must never allocate memory. schedule() only uses      │
  │  stack and existing structures.                                      │
  │                                                                      │
  │  Event bus publish: uses a pre-allocated fixed-size queue.           │
  │  MAX_PENDING_EVENTS = 64. MUST NOT grow. If queue full, drop        │
  │  oldest event (or return ERR_BUSY — never OOM).                     │
  │                                                                      │
  │  shell input: pre-allocated line buffer (256 bytes).                 │
  │  History: pre-allocated array (16 × 256 = 4KB).                      │
  └──────────────────────────────────────────────────────────────────────┘

  ┌──────────────────────────────────────────────────────────────────────┐
  │  PRINCIPLE 3: BOUNDED QUEUE DEPTH                                   │
  │                                                                      │
  │  Every queue in the system has a MAX capacity:                       │
  │    - Run queue: unbounded (number of threads) — OK                   │
  │    - Event bus: 64 entries                                           │
  │    - Wait queue: unbounded (threads blocked on mutex) — OK          │
  │    - Shell input buffer: 256 bytes                                   │
  │    - Watchdog layers: 16                                             │
  │    - IRQ handlers: 48                                                │
  │                                                                      │
  │  When any queue is full, the producer must either:                   │
  │    a) Block until space available (push-back)                        │
  │    b) Return error (push-fail)                                       │
  │    c) Drop oldest entry (push-drop)                                  │
  │  NEVER allow unbounded growth.                                       │
  └──────────────────────────────────────────────────────────────────────┘

  ┌──────────────────────────────────────────────────────────────────────┐
  │  PRINCIPLE 4: NO HEAP, NO ALLOCATOR FRAGMENTATION                    │
  │                                                                      │
  │  All allocations are page-granularity (4KB). pmm_alloc_page always   │
  │  returns PAGE_SIZE bytes. This eliminates heap fragmentation.        │
  │                                                                      │
  │  For sub-page allocations (thread_t = 184 bytes), waste is real:     │
  │  184 / 4096 = 4.5% utilization. Consider a slab allocator for       │
  │  fixed-size kernel objects: thread blocks (thread_t), mutexes,       │
  │  wait_queue entries. Slab allocator can pack objects densely         │
  │  within a page, reducing both waste and cache misses.                │
  │                                                                      │
  │  CURRENT WASTE: Each thread_t wastes 4096 − 184 = 3912 bytes/page.  │
  │  For 50 threads: 200KB wasted. Slab would reduce to ~1 page.        │
  │                                                                      │
  │  Target: slab allocator for objects < PAGE_SIZE/2.                   │
  └──────────────────────────────────────────────────────────────────────┘

  ┌──────────────────────────────────────────────────────────────────────┐
  │  PRINCIPLE 5: CACHE-AWARE DATA LAYOUT                               │
  │                                                                      │
  │  thread_t is 184 bytes — spans 3 cache lines (typical 64-byte lines).│
  │  Frequently accessed fields (id, rsp, state, priority, next, prev)  │
  │  should be packed in the first 64 bytes. Move infrequently accessed │
  │  fields (name, kernel_stack, exit_code) to the later part.          │
  │                                                                      │
  │  run_queue_t is hot (checked on every schedule). Keep it aligned     │
  │  to avoid false sharing with other queues when SMP arrives.         │
  │                                                                      │
  │  PMM bitmap: 512MB / 4KB = 131072 bits = 16KB. Fits in 4 pages.    │
  │  Not hot (only touched on alloc/free). No optimization needed.      │
  └──────────────────────────────────────────────────────────────────────┘

  ┌──────────────────────────────────────────────────────────────────────┐
  │  PRINCIPLE 6: INSTRUMENT EVERYTHING                                  │
  │                                                                      │
  │  Every major subsystem must expose counters:                         │
  │    - Scheduler: context switches, preemptions, yields, idle %        │
  │    - PMM: allocations, frees, OOM events                             │
  │    - Event bus: published, dispatched, dropped                       │
  │    - Watchdog: health checks passed/failed                           │
  │    - HAL: timer ticks, IRQ counts by vector                          │
  │                                                                      │
  │  Use uint64_t atomics (locked add) or per-CPU counters for SMP.     │
  │  Expose via /dev/stats or shell command.                             │
  └──────────────────────────────────────────────────────────────────────┘


================================================================================
   9. RUST PORT STRATEGY
================================================================================

  GOAL: Port the entire kernel from C to Rust for embedded targets
        (ARM Cortex-M, RISC-V RV32/RV64) with minimal footprint.

  WHY RUST:
    - Memory safety without GC (no use-after-free, no double-free)
    - No undefined behavior (no dangling pointers, no buffer overflows)
    - Fearless concurrency (ownership model prevents data races)
    - Zero-cost abstractions (no runtime, no hidden allocations)
    - C FFI compatibility for gradual migration

  PORTING PHASES (after C kernel is fully stable):

  Phase R1 — Core Types & Library (Rust no_std)
  ────────────────────────────────────────────────────────────────────────
  [ ] Port types.h → core::types (uint64_t, err_t, etc.)
  [ ] Port klib.c → core::io (kputchar, kprintf, kputs)
  [ ] Port klib.c → core::mem (kmemset, kmemcpy, etc.)
  [ ] Port errno.h → core::error (err_t enum, err_str)
  [ ] All pure-C code with no hardware dependency

  Phase R2 — HAL Layer (Rust with inline assembly)
  ────────────────────────────────────────────────────────────────────────
  [ ] Port hal.c → hal:: (GDT, IDT, PIC, UART, timer)
  [ ] Port boot.S → boot:: (entry point, page tables)
  [ ] Port isr.S → hal::interrupt (ISR stubs, common handler)
  [ ] Port ctx.S → hal::context (switch_context, trampolines)
  [ ] Use core::arch::asm! for inline assembly

  Phase R3 — Kernel Layers (Rust)
  ────────────────────────────────────────────────────────────────────────
  [ ] Port pmm.c → pmm:: (bitmap + free list → better: buddy allocator)
  [ ] Port vmm.c → vmm:: (4-level page tables, map/unmap)
  [ ] Port sched.c → sched:: (thread, run queue, scheduler)
  [ ] Port sync.c → sync:: (spinlock, mutex with wait queue)
  [ ] Port shell.c → shell:: (interactive CLI)
  [ ] Port eventbus.c → eventbus:: (publish/subscribe)
  [ ] Port watchdog.c → watchdog:: (health monitoring)

  Phase R4 — Embedded Target Adaptation
  ────────────────────────────────────────────────────────────────────────
  [ ] Replace HAL with target-specific implementations:
  [ ] Cortex-M: NVIC instead of PIC, SysTick instead of PIT
  [ ] RISC-V: CLINT/PLIC, mtime/mcycle
  [ ] Remove x86-64 specific features (paging, long mode init)
  [ ] Add FPGA/ASIC-friendly design: fixed-address peripherals
  [ ] Size optimization: link-time garbage collection, LTO

  Phase R5 — Certification & Verification
  ────────────────────────────────────────────────────────────────────────
  [ ] Add formal verification with Kani or Verus for critical paths
  [ ] Test with proptest/fuzzcheck for edge-case discovery
  [ ] Document unsafe{} blocks with SAFETY comments
  [ ] Size budget: < 32KB flash, < 8KB RAM for minimal config
  [ ] Real-time: bounded worst-case execution time (WCET) analysis

  TARGET EMBEDDED PLATFORMS:
    - STM32F4 (ARM Cortex-M4, 168MHz, 192KB RAM, 1MB flash)
    - ESP32-C3 (RISC-V, 160MHz, 400KB RAM)
    - FPGA soft-CPU: VexRiscv (RISC-V, ~100MHz, ~50 LUTs)


================================================================================
   10. APPENDIX: FILE-BY-FILE NOTES
================================================================================

  os/src/boot/boot.S  ─── 154 lines
  ────────────────────────────────────────────────────────────────────────
  OK: PVH .note section, CPUID check, long mode transition, GDT lgdt,
      page table setup, jump to kmain
  ISSUES:
    - Lines 80-86: PML4 entries 0 and 511 both point to same PDPT.
      Entries 510 and 511 both point to same PD. Redundant.
    - No .rodata section noted (contents go to .rodata in linker)
    - Stack is 32KB (32768 bytes). Current max ~10 threads. Should be
      adequate but monitor.

  os/src/boot/isr.S  ─── 134 lines
  ────────────────────────────────────────────────────────────────────────
  OK: ISR macros for all 48 vectors + syscall 128. Common handler
      saves 15 regs, calls interrupt_handler, checks need_reschedule,
      pops, iretq.
  ISSUES:
    - Line 99-101: schedule() called from ISR context. schedule()
      internally calls hal_save_irq (does cli again - redundant).
      Works but fragile if schedule() ever enables interrupts.

  os/src/include/types.h  ─── 66 lines
  ────────────────────────────────────────────────────────────────────────
  OK: Standard type aliases, err_t enum, kernel VA macros, page macros
  ISSUES:
    - bool/true/false don't follow K_ prefix convention

  os/src/include/errno.h  ─── 33 lines
  ────────────────────────────────────────────────────────────────────────
  OK: Static inline err_str function

  os/src/include/kernel.h  ─── 32 lines
  ────────────────────────────────────────────────────────────────────────
  OK: Function declarations for klib, kassert, kpanic

  os/src/kernel/hal.c  ─── 383 lines
  ────────────────────────────────────────────────────────────────────────
  OK: GDT setup, IDT with IST for double fault, PIC remap, UART init,
      PIT timer at 1000Hz, interrupt dispatcher
  ISSUES:
    - C6: Missing "memory" clobber in hal_save_irq/restore_irq
    - H1: PIC mask enables IRQ1 (0xFD should be 0xFB)
    - M4: hal_uart_putchar duplicates kputchar's \r logic
    - Line 150: IST=1 for double fault (vec 8) only. Not for NMI (vec 2),
      GP fault (vec 13), or page fault (vec 14). Consider configuring
      IST entries for other critical faults to prevent triple faults.

  os/src/kernel/pmm.c  ─── 218 lines
  ────────────────────────────────────────────────────────────────────────
  OK: Bitmap + free list, mark_region_used, alloc/free page(s),
      parse multiboot memory map, PVH fallback
  ISSUES:
    - C3: pmm_alloc_pages TOCTOU race
    - H6: pmm_free_page TOCTOU race
    - H5: Free list LIFO causes fragmentation
    - Line 196-202: Initial bitmap fill (0xFF → clear all) is O(n) where
      n = total_page_count = 131072. Acceptable for boot (ms range).

  os/src/kernel/vmm.c  ─── 149 lines
  ────────────────────────────────────────────────────────────────────────
  OK: 4-level page table walk, map/unmap/query, TLB flush
  ISSUES:
    - H7: Page table physical addresses not accessible if above 512MB
    - vmm_init just reads CR3; doesn't map kernel .text/.rodata/.data
      with NX bits. Currently everything is RWX.

  os/src/kernel/sched.c  ─── 397 lines
  ────────────────────────────────────────────────────────────────────────
  OK: 256-priority run queues with bitmap O(1) pick_next. Preemption via
      ISR. Global thread list. Zombie reaper. Sleep/wake. Block/wake.
  ISSUES:
    - C1: thread_sleep → check_sleepers mismatch (threads sleep forever)
    - C4: thread_set_priority removes from wrong queue
    - H2: check_sleepers dead code
    - H4: sched_foreach not protected against concurrent modification
    - M3: No comment on init stack dummy values
    - thread_create: missing priority range check (< 0 becomes unsigned)

  os/src/kernel/ctx.S  ─── 46 lines
  ────────────────────────────────────────────────────────────────────────
  OK: switch_context saves/restores callee-saved regs. thread_trampoline
      invokes func(arg) then thread_exit(retval).
  ISSUES:
    - M7: schedule_trampoline dead code (can be removed)
    - M2: thread_trampoline ABI undocumented

  os/src/kernel/sync.c  ─── 80 lines
  ────────────────────────────────────────────────────────────────────────
  OK: Spinlock with PAUSE loop. Mutex with CAS + wait queue.
  ISSUES:
    - C5: mutex_lock lost wakeup on timeout
    - Spinlock doesn't disable interrupts! This means a spinlock held
      by thread A can be preempted, and thread B tries to acquire it
      and spins forever. This is a DEADLOCK.
      → MUST disable interrupts when holding a spinlock in UP kernel.
      → Fix: add cli/sti around spinlock_acquire/release.

  os/src/kernel/eventbus.c  ─── 139 lines
  ────────────────────────────────────────────────────────────────────────
  OK: Fixed subscriber array (32), fixed pending queue (64). Spinlock
      protect. dispatch() processes events in-order.
  ISSUES:
    - M8: None critical. Design is sound.
    - Event loss when queue is full (silent drop — should log/config).

  os/src/kernel/watchdog.c  ─── 142 lines
  ────────────────────────────────────────────────────────────────────────
  OK: 3-layer health monitoring (HAL, Scheduler, PMM). Runs every
      1000 ticks via timer handler.
  ISSUES:
    - M9: watchdog_run prints UART from IRQ context (vulnerable if
          kprintf ever needs IRQ-driven UART)
    - static uint64_t last_check inside function — works for UP

  os/src/kernel/shell.c  ─── 419 lines
  ────────────────────────────────────────────────────────────────────────
  OK: 18 commands, argument parsing, line editing, history (16 entries)
  ISSUES:
    - H3: UART polling busy-loop
    - M6: shell_start_tick unused

  os/src/kernel/main.c  ─── 62 lines
  ────────────────────────────────────────────────────────────────────────
  OK: 6 boot steps, calls each layer init, prints box art, starts shell
  ISSUES:
    - M10: Hardcoded step numbering

  os/src/lib/klib.c  ─── 205 lines
  ────────────────────────────────────────────────────────────────────────
  OK: kputchar, kputs, kputhex, kputdec, kprintf, kpanic, string/memory ops
  ISSUES:
    - C2: kprintf %x calls kprintf recursively (va_list corruption)
    - kputhex prints 0x prefix + 16 hex digits (always 16, even for 0)
    - kpanic format handler only supports %s, %x, %d

  os/Makefile  ─── 75 lines
  ────────────────────────────────────────────────────────────────────────
  OK: Builds all .c + .S, links with linker.ld, run/debug/monitor targets
  ISSUES:
    - M5: Missing -fno-omit-frame-pointer
    - M1: (implicit) no check for duplicate PAGE_SIZE definitions
    - No `test` target (manual QEMU testing only)

  os/linker.ld  ─── 58 lines
  ────────────────────────────────────────────────────────────────────────
  OK: PVH note first, boot sections at physical 0x100000,
      then higher-half sections. _kernel_end_phys exported.
  ISSUES:
    - No stack guard area
    - .boot_text and .boot_data not aligned to page boundary

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
  7. Race Condition Inventory (all shared data)
  8. Integer Overflow & Undefined Behavior Analysis
  9. Stack Depth Analysis (measured worst-case)
  10. Dead Code & Redundancy Inventory
  11. Layering (LLN) Violation Audit
  12. Enhancement Roadmap — Phase 1-4
  13. Anti-Lag & Heavy-Task Design Principles
  14. Rust Port Strategy
  15. Appendix: File-by-File Notes


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

  LAYER ASSIGNMENT (as implemented):
    L0: boot.S       — entry, page tables, 32→64 transition
    L1: hal.c/h      — GDT, IDT, PIC, UART, timer, IRQ dispatch
    L2: sched.c/h    — threads, scheduler, sync primitives
        sync.c/h     — spinlock, mutex (same layer)
        ctx.S        — context switch, thread trampoline
    L4: pmm.c/h      — physical memory allocator
        vmm.c/h      — virtual memory (page tables)
    L(N-1): shell.c/h — interactive CLI
    Cross: eventbus.c/h, watchdog.c/h

  PREEMPTION MODEL:
    - Timer IRQ fires → CPU pushes SS,RSP,RFLAGS,CS,RIP + error
    → ISR stub pushes vector/error → isr_common_handler saves 15 GP regs
    → interrupt_handler() runs IRQ handler (sched_timer_tick)
    → ISR checks need_reschedule → if set, calls schedule() DIRECTLY
    (registers still saved on stack → context switch preserves them)
    → ISR pops registers (of the running thread after switch) → iretq

  MEMORY LAYOUT (physical):
    0x000000 - 0x000FFF: Real-mode data (1 page — reserved)
    0x001000 - 0x006FFF: Free but UNMANAGED (24KB wasted)
    0x007000 - 0x00AFFF: Page tables: PML4(0x7000) PDPT(0x8000) PD(0x9000)
    0x00B000 - 0x00FFFF: Free but UNMANAGED (20KB wasted)
    0x010000 - 0x013FFF: Boot bitmap (4 pages — reserved)
    0x014000 - 0x0FFFFF: Free but UNMANAGED (~960KB wasted)
    0x100000 - 0x21B000: Kernel .text .rodata .data .bss
    0x21B000 - 0x1FFFFFFF: Managed by PMM (~511MB free)
    0x20000000 - ...   : Above 512MB — NOT MAPPED by boot page tables

  CURRENT LIMITATIONS (UP only):
    - No SMP support
    - No user mode / KPTI
    - No filesystem
    - No DMA
    - No IPC except event bus
    - No network
    - ~1MB of memory below 1MB is permanently wasted (never added to PMM)


================================================================================
   3. CRITICAL BUGS (must fix — in priority order)
================================================================================

  TOTAL: 13 CRITICAL bugs found (7 from initial audit + 6 from deep audit)

  C1 ─ PMM has NO synchronization at all [pmm.c:9,11,16,43-54,56-89,91-103]
  ──────────────────────────────────────────────────────────────────────
  BUG:    free_list, free_page_count, and used_bitmap[] are read/written
          by pmm_alloc_page, pmm_free_page, and pmm_alloc_pages WITHOUT
          any cli/atomics. If a timer ISR fires during any PMM operation:
            - free_list linked list gets corrupted (torn write)
            - free_page_count read-modify-write is non-atomic
            - bitmap byte read-modify-write is non-atomic
          Two threads (via preemption) can allocate the SAME page,
          or double-free, or corrupt the freelist.

  FIX:    Wrap ALL PMM alloc/free operations with hal_save_irq/
          hal_restore_irq. Keep critical sections short:
          ┌──────────────────────────────────────────────────────────────┐
          │ uint64_t pmm_alloc_page(void) {                              │
          │     cpu_flags_t flags = hal_save_irq();                     │
          │     if (!free_list) { hal_restore_irq(flags); return 0; }   │
          │     free_page_t* page = free_list;                          │
          │     free_list = page->next;                                 │
          │     free_page_count--;                                      │
          │     uint64_t addr = (uint64_t)page;                         │
          │     bitmap_set(addr / PAGE_SIZE);                           │
          │     hal_restore_irq(flags);                                 │
          │     kmemset((void*)addr, 0, PAGE_SIZE);   // outside lock  │
          │     return addr;                                            │
          │ }                                                           │
          └──────────────────────────────────────────────────────────────┘

  C2 ─ irq_handlers[] read by ISR without barrier vs thread write [hal.c:60,200-201,325-326]
  ──────────────────────────────────────────────────────────────────────
  BUG:    hal_irq_register writes two fields (handler, data) with no
          ordering guarantee. The ISR (interrupt_handler) reads both
          without any barrier. On x86-64 this usually works, but
          formally a torn read could see new handler + old data.
          Fix: use a single-pointer store (pack handler+data) or
          add a release/acquire barrier.

  FIX:    Option A: Pack handler and data into a single uint64_t pointer
          (padded struct) and use a single atomic store/load.
          Option B: Use __sync_synchronize() (full barrier) after
          writing handler/data in hal_irq_register.
          Simplest: just make the irq_reg_t fields volatile and add
          a compiler barrier after writing.

  C3 ─ schedule() re-enables interrupts BEFORE switch_context completes [sched.c:189-190]
  ──────────────────────────────────────────────────────────────────────
  BUG:    schedule() calls hal_restore_irq(flags) at L189, THEN calls
          switch_context at L190. If a timer interrupt fires between
          these two lines, the ISR sets need_reschedule=1 and on return
          calls schedule() AGAIN while the first schedule() hasn't
          finished switching stacks. This corrupts current_thread,
          run queues, and priority bitmap → guaranteed crash.

  FIX:    Move hal_restore_irq(flags) AFTER switch_context, or disable
          interrupts in switch_context itself:
          ┌──────────────────────────────────────────────────────────────┐
          │ // In schedule(), move restore after switch:                 │
          │ switch_context(&old, &current_thread);                       │
          │ hal_restore_irq(flags);    // now safe — new thread runs    │
          └──────────────────────────────────────────────────────────────┘
          BUT: hal_restore_irq runs in the NEW thread's context, so it
          restores the NEW thread's flags, not the old one's. This is
          actually CORRECT because the new thread's saved flags represent
          the interrupt state it should have.

  C4 ─ check_sleepers iterates run queue linked list WITHOUT cli [sched.c:336-355]
  ──────────────────────────────────────────────────────────────────────
  BUG:    check_sleepers follows run_queue t->next pointers without
          disabling interrupts. If an ISR calls sched_remove_thread
          or pick_next during iteration, the node being followed can
          be removed and freed → dangling pointer dereference.

  FIX:    Wrap check_sleepers in hal_save_irq / hal_restore_irq.
          Also: sleeping threads are NOT in the run queue (C1/C7 from
          initial audit), so check_sleepers must iterate the GLOBAL
          thread list instead (which also needs cli — see C5).

  C5 ─ sched_foreach iterates global thread list WITHOUT cli [sched.c:47-54]
  ──────────────────────────────────────────────────────────────────────
  BUG:    sched_foreach follows all_next pointers without disabling
          interrupts. Called from shell's ps command (thread context).
          If an ISR triggers thread_exit during iteration, the node
          being followed is freed → dangling pointer → crash.

  FIX:    Wrap with hal_save_irq:
          ┌──────────────────────────────────────────────────────────────┐
          │ void sched_foreach(...) {                                    │
          │     if (!cb) return;                                        │
          │     cpu_flags_t flags = hal_save_irq();                     │
          │     thread_t* t = all_threads_head;                         │
          │     while (t) {                                             │
          │         thread_t* next = t->all_next;  // snap now         │
          │         cb(t, ctx);                                         │
          │         t = next;                                           │
          │     }                                                       │
          │     hal_restore_irq(flags);                                 │
          │ }                                                           │
          └──────────────────────────────────────────────────────────────┘

  C6 ─ next_thread_id non-atomic increment [sched.c:9,237]
  ──────────────────────────────────────────────────────────────────────
  BUG:    next_thread_id++ is a read-modify-write without any protection.
          Two threads calling thread_create concurrently (preemption
          between read and write) can get the SAME thread ID.

  FIX:    Use __sync_fetch_and_add or protect with cli:
          ┌──────────────────────────────────────────────────────────────┐
          │ cpu_flags_t flags = hal_save_irq();                         │
          │ tcb->id = next_thread_id++;                                 │
          │ hal_restore_irq(flags);                                     │
          └──────────────────────────────────────────────────────────────┘

  C7 ─ thread_sleep permanently blocks threads [sched.c:274-282]
  ──────────────────────────────────────────────────────────────────────
  BUG:    thread_sleep() sets state=THREAD_SLEEPING, calls thread_yield()
          → schedule(). schedule() checks "if state==RUNNING" → no, so
          thread is NOT re-added to run queue. Later, check_sleepers()
          iterates run_queues[] but the sleeping thread is not in any
          queue. Result: thread sleeps forever, never woken.

  FIX:    Change check_sleepers to iterate the global thread list
          (all_threads_head) instead of run_queues[]. When a sleeping
          thread's wakeup_tick has passed, set state=THREAD_READY and
          sched_add_thread() to re-add to run queue.

  C8 ─ kprintf %x calls kprintf recursively [klib.c:93]
  ──────────────────────────────────────────────────────────────────────
  BUG:    %x handler calls kprintf("0x") which recursively enters
          kprintf, corrupting the outer va_list via __builtin_va_arg.
          All subsequent format args (%d, %s, %lu) read wrong values.

  FIX:    Replace kprintf("0x") with kputs("0x") + local hex output:
          ┌──────────────────────────────────────────────────────────────┐
          │ case 'x':                                                  │
          │     kputs("0x");                                           │
          │     int shift = 60;                                        │
          │     while (shift > 0 && !((v>>shift)&0xF)) shift -= 4;    │
          │     for (int i = shift; i >= 0; i -= 4)                   │
          │         kputchar(hexdigits[(v >> i) & 0xF]);               │
          │     break;                                                 │
          └──────────────────────────────────────────────────────────────┘

  C9 ─ pmm_alloc_pages TOCTOU race [pmm.c:56-89]
  ──────────────────────────────────────────────────────────────────────
  BUG:    Scans bitmap for contiguous free pages, then allocates them.
          Between scan and allocation, another thread (preemption) can
          claim pages in the gap → overlapping allocation.

  FIX:    Wrap entire scan+allocate with hal_save_irq.
          (Covered by C1 fix — all PMM ops must be serialized.)

  C10 ─ thread_set_priority removes from wrong queue [sched.c:299-304]
  ──────────────────────────────────────────────────────────────────────
  BUG:    sched_remove_thread(t) uses t->priority to find run queue.
          If priority was already changed, it searches the NEW queue
          but the thread is still in the OLD queue.

  FIX:    Save old priority before modifying:
          int old_prio = t->priority;
          // Use old_prio for sched_remove_thread,
          // then set t->priority = new_prio, then sched_add_thread.

  C11 ─ hal_save_irq missing memory barriers [hal.c:96-105]
  ──────────────────────────────────────────────────────────────────────
  BUG:    asm volatile("cli") lacks "memory" clobber. Compiler can
          reorder memory operations across the cli.
  FIX:    asm volatile("cli" : : : "memory");

  C12 ─ timer_ticks * 1e9 overflows after ~7 months uptime [hal.c:236]
  ──────────────────────────────────────────────────────────────────────
  BUG:    hal_timer_get_ns(): (timer_ticks * 1000000000ULL) / timer_hz.
          timer_ticks is uint64_t. At 1000 Hz, ticks reach 2^64 / 1e9
          ≈ 1.8e10 after ~7 months, causing multiplication overflow.
          After overflow, result wraps to small value → bogus ns.

  FIX:    Use 128-bit arithmetic (GCC __int128) or reduce precision:
          ┌──────────────────────────────────────────────────────────────┐
          │ uint64_t hal_timer_get_ns(void) {                           │
          │     unsigned __int128 ns = (unsigned __int128)timer_ticks   │
          │                         * 1000000000ULL;                    │
          │     return (uint64_t)(ns / timer_hz);                       │
          │ }                                                           │
          └──────────────────────────────────────────────────────────────┘

  C13 ─ Spinlock_acquire doesn't disable interrupts [sync.c:11-28]
  ──────────────────────────────────────────────────────────────────────
  BUG:    On UP with preemption: thread A holds spinlock, gets preempted,
          thread B spins forever → DEADLOCK. Classic UP bug.

  FIX:    Spinlock_acquire must cli; spinlock_release must sti.
          Add cpu_flags_t saved_flags field to spinlock_t:
          ┌──────────────────────────────────────────────────────────────┐
          │ void spinlock_acquire(spinlock_t* lock) {                   │
          │     lock->saved_flags = hal_save_irq();  // cli + save     │
          │     while (__sync_lock_test_and_set(&lock->lock, 1))       │
          │         while (lock->lock) asm volatile("pause");          │
          │     lock->holder = current_thread ? current_thread->id : 0;│
          │     asm volatile("" ::: "memory");                         │
          │ }                                                          │
          │ void spinlock_release(spinlock_t* lock) {                  │
          │     asm volatile("" ::: "memory");                         │
          │     lock->holder = 0;                                      │
          │     __sync_lock_release(&lock->lock);                      │
          │     hal_restore_irq(lock->saved_flags);                    │
          │ }                                                          │
          └──────────────────────────────────────────────────────────────┘
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
   7. RACE CONDITION INVENTORY (all shared mutable data)
================================================================================

  Every global/static variable that is written by more than one context,
  and the protection (if any) covering it.

  ┌─────────────────────────────────────────────────────────────────────────────┐
  │  DATA STRUCTURE          WRITERS               PROTECTION    RISK           │
  ├─────────────────────────────────────────────────────────────────────────────┤
  │  pmm.c: free_list        alloc, free,           NONE (!!)    CRITICAL       │
  │                          alloc_pages, add_region                            │
  │  pmm.c: free_page_count  alloc, free,           NONE (!!)    CRITICAL       │
  │                          mark_region_used                                   │
  │  pmm.c: used_bitmap[]    bitmap_set/clear,      NONE (!!)    CRITICAL       │
  │                          pmm_init                                           │
  │  hal.c: irq_handlers[]   hal_irq_register,      NONE         HIGH           │
  │                          interrupt_handler (read)  (torn read risk)         │
  │  hal.c: timer_ticks      interrupt_handler      volatile     LOW (ISR only) │
  │  sched.c: run_queues[]   sched_add/remove/      cli          OK             │
  │                          pick_next                                          │
  │  sched.c: priority_bitmap bitmap_set/clear      cli (via     OK             │
  │                                                  callers)                   │
  │  sched.c: next_thread_id thread_create          NONE         HIGH (C6)      │
  │  sched.c: all_*_head/tail all_threads_add/remove cli         OK             │
  │  sched.c: current_thread schedule, thread_exit   cli         OK             │
  │  sched.c: need_reschedule sched_tick, schedule   volatile    OK (x86)       │
  │  sched.c: check_sleepers  (reads run_queues)     NONE (!!)   CRITICAL (C4)  │
  │  sched.c: sched_foreach   (reads all_threads)    NONE (!!)   CRITICAL (C5)  │
  │  eventbus: subscribers[]  subscribe/unsubscribe spinlock    OK (with C13)  │
  │  eventbus: pending_events[] publish/dispatch     spinlock    OK (with C13)  │
  │  watchdog: layers[]       register_layer, run    NONE        LOW (init only)│
  │  watchdog: num_layers     register_layer         NONE        LOW            │
  ├─────────────────────────────────────────────────────────────────────────────┤
  │  TOTAL: 17 shared structures   |   5 unprotected |  3 critical races        │
  └─────────────────────────────────────────────────────────────────────────────┘


================================================================================
   8. INTEGER OVERFLOW & UNDEFINED BEHAVIOR ANALYSIS
================================================================================

  8.1 — Arithmetic overflows (all uint64_t context)

  ┌─────────────────────────────────────────────────────────────────────────────┐
  │  EXPRESSION                      FILE:LINE       OVERFLOW AT                │
  ├─────────────────────────────────────────────────────────────────────────────┤
  │  timer_ticks * 1000000000ULL     hal.c:236       ~7 months (C12)            │
  │  (end + PAGE_SIZE - 1) / ...     pmm.c:34        end = UINT64_MAX           │
  │  (first + j) * PAGE_SIZE         pmm.c:72        first+j > 2^52            │
  │  phys_addr + i*PAGE_SIZE         pmm.c:107       near top of address space  │
  │  total * 4 / 1024  (display)     shell.c:78-80   total > 2^62              │
  │  free * 100 / total  (display)   shell.c:85      OK (free ≤ total)          │
  │  base_addr + length (mmap)       hal.c:301       malicious boot data (C)    │
  │  KERNEL_VMA_BASE + P (PHYS_TO..) types.h:50      P > 0x40000000 (1GB)      │
  │  V - KERNEL_VMA_BASE (VIRT_TO..) types.h:51      V < KERNEL_VMA_BASE        │
  │  PAGE_ALIGN(UINT64_MAX)          types.h:56      returns 0 (overflow)       │
  │  sched.c: thread_count * NAME    sched.c:counts  OK (< 2^32)               │
  │  eventbus: event_count wraps     eventbus.c:25   after 2^64 events         │
  └─────────────────────────────────────────────────────────────────────────────┘

  FIXES:
    - timer_ticks: use unsigned __int128 (GCC extension)
    - pmm.c:34: ensure end < UINT64_MAX - PAGE_SIZE before adding
    - pmm.c:72: check first+j won't overflow, or limit total_page_count
    - types.h macros: document preconditions (address < 2^63)
    - hal.c:301: validate mmap entry before using

  8.2 — Undefined behavior (C standard violations)

  ┌─────────────────────────────────────────────────────────────────────────────┐
  │  UB                              FILE:LINE       DETAIL                      │
  ├─────────────────────────────────────────────────────────────────────────────┤
  │  Shift by negative count          sched.c:80,84   prio%64 is negative for    │
  │                                                  negative prio → UB         │
  │  Signed int overflow (negate)     klib.c:33       v = -v when v=INT64_MIN   │
  │  Strict aliasing violation        klib.c:58-121   va_arg reads through      │
  │                                                  type-punned pointers       │
  │  __builtin_clzll(0)               sched.c:90      UNDEFINED (but guarded)   │
  │  NULL pointer write (test only)   shell.c:297     cmd_fault test #2         │
  │  Integer division by zero         shell.c:85,125  if total==0, % operation  │
  │  (implied in checks)                                on zero → fault         │
  └─────────────────────────────────────────────────────────────────────────────┘

  FIXES:
    - bitmap_set/clear_prio: ensure prio ≥ 0 before calling, or use unsigned
    - kprint_int64: handle INT64_MIN as special case
    - Add -fno-strict-aliasing to CFLAGS
    - shell.c: guard against total==0 before %


================================================================================
   9. STACK DEPTH ANALYSIS (worst-case measured)
================================================================================

  9.1 — ISR context (timer IRQ → schedule → context switch)

  LAYER           FRAME CONTENT                    BYTES   CUMULATIVE
  ──────────────  ───────────────────────────────  ─────   ──────────
  CPU push        SS,RSP,RFLAGS,CS,RIP,error       40      40
  ISR stub        vector + error_code               16      56
  isr_common      15 GP registers                   120     176
  call handler    return addr                       8       184
  interrupt_han   local vars (vec, irq)             16      200
  sched_timer_tk  simple cond(opt)                  8       208
  watchdog_run    reason[128] + vars                144     352
  kprintf         va_list+fmt+buf[24]               96      448
  eventbus_pub    frame + spinlock                  48      496
  schedule        frame + pick_next tmp             32      528
  pick_next       local variables                   16      544
  switch_context  callee-saved regs push            48      592

  TOTAL ISR STACK:  ~592 bytes
  IST STACK SIZE:   8192 bytes (ist_stack0)
  HEADROOM:         7600 bytes (92% free)  ✓

  9.2 — Thread context (shell → command → kprintf)

  LAYER           FRAME CONTENT                    BYTES   CUMULATIVE
  ──────────────  ───────────────────────────────  ─────   ──────────
  shell_run       line_pos, hist_idx, c, loop      32      32
  process_line    buf[256] + args[16] (128B)       384     416
  cmd_*           local uint64 vars                40      456
  kprintf         va_list + fmt parse              80      536
  kprint_int64    buf[24] + neg/pos/uv             40      576
  kputchar        char arg                         8       584

  TOTAL THREAD STACK:  ~584 bytes
  THREAD STACK SIZE:  16384 bytes
  HEADROOM:           15800 bytes (96% free)  ✓

  NOTE: No stack guard pages exist. Stack overflow silently corrupts
        adjacent memory. For embedded/Rust targets, add guard pages
        (unmapped page below each stack → page fault on overflow).


================================================================================
   10. DEAD CODE & REDUNDANCY INVENTORY
================================================================================

  10.1 — Unused C functions (29 total)

  FILE              FUNCTION                REASON
  ────────────────  ──────────────────────  ──────────────────────────────
  klib.c            kstrlen                 never called
  klib.c            kstrncmp                never called
  klib.c            kstrcpy                 never called
  klib.c            kmemcpy                 never called
  klib.c            kmemcmp                 never called
  klib.c            kassert_fail            KASSERT never used
  hal.c             hal_cli()               never called (extern hal_sti used)
  hal.c             hal_idt_set_gate()      never called
  hal.c             hal_irq_unregister()    never called
  hal.c             hal_uart_putchar()      never called (kputchar used)
  pmm.c             pmm_debug_dump()        never called
  vmm.c             vmm_free_page_table()   never called
  vmm.c             vmm_get_phys()          never called
  vmm.c             vmm_map_region()        never called
  vmm.c             vmm_unmap_region()      never called
  vmm.c             vmm_flush_tlb()         never called
  vmm.c             vmm_switch_pml4()       never called
  sched.c           sched_tick()            obsoleted by sched_timer_tick
  sched.c           thread_sleep()          never called (broken C1/C7)
  sched.c           thread_wake()           never called (C1/C7 blocks)
  sched.c           thread_join()           never called
  sched.c           thread_set_priority()   never called
  eventbus.c        eventbus_subscribe()    never called
  eventbus.c        eventbus_unsubscribe()  never called
  sync.c            spinlock_try_acquire()  never called
  errno.h           err_str()               never called
  main.c            watchdog_timer_handler  extern duplicate (in watchdog.h)
  ctx.S             schedule_trampoline     dead since preemption redesign
  ctx.S             hal_get_rsp             never called

  10.2 — Unused/Redundant variables, fields, & macros

  SYMBOL                    FILE          SIZE   NOTE
  ────────────────────────  ────────────  ─────  ─────────────────────────
  shell_start_tick (static) shell.c        8B    written, never read
  cleanup_arg (thread_t)    sched.h        8B    preemption redesign orphan
  kernel_pml4 (static)      vmm.c          8B    assigned & printed, never used
  ps_ctx.first (struct)     shell.c        4B    declared, never instantiated
  _text/_rodata/_data/_bss  kernel.h      40B    declared, never referenced
    start/end symbols
  bool, ssize_t, intptr_t   types.h        —     defined, never used
  MAX_PRIORITY, DEFAULT_... types.h        —     superseded by sched.h macros
  PHYS_TO_VIRT, VIRT_TO_..  types.h        —     never used
  PAGE_ALIGN, IS_PAGE_ALIGN types.h        —     never used
  PAGE_NX                   vmm.h          —     never used
  WATCHDOG_INTERVAL_MS      watchdog.h     —     never used; 1000 hardcoded
  EV_PAGE_FAULT..EV_TIMER.. eventbus.h     —     8 enum values never used

  TOTAL dead weight: ~80 bytes data + 29 functions + 20 macros/enums

  10.3 — Unused #include directives

  FILE              UNUSED INCLUDE
  ────────────────  ──────────────────
  pmm.c             #include "hal.h"
  vmm.c             #include "hal.h"
  shell.c           #include "vmm.h"
  sync.c            #include "hal.h"

  FIX: Remove all unused includes and dead code to reduce binary size
       and compilation time.


================================================================================
   11. LAYERING (LLN) VIOLATIONS
================================================================================

  LLN RULE: Layer N calls only Layer N-1. No upward calls, no skipping.

  CURRENT LAYER ASSIGNMENT:
    L0: boot.S
    L1: hal.c/h
    L2: sched.c/h, sync.c/h, ctx.S
    L4: pmm.c/h, vmm.c/h
    L(N-1): shell.c/h
    Cross: eventbus.c/h, watchdog.c/h

  VIOLATIONS:

  1. VIOLATION: sched.c includes pmm.h (Layer 2 → Layer 4, UPWARD)
     ──────────────────────────────────────────────────────────────────
     LOCATION: sched.c:3 (#include "pmm.h")
     FUNCTION: thread_create calls pmm_alloc_page, pmm_free_page,
               pmm_alloc_pages
     PROBLEM: L2 (scheduler) should NOT call L4 (PMM). PMM is above
              scheduler in the layer hierarchy.
     FIX: Introduce a memory allocation interface at L1 (HAL) or L2 that
          PMM implements (inversion of control). OR swap layers so PMM
          is below scheduler (L0). Recommended: move allocator to L0.

  2. VIOLATION: sched.c includes eventbus.h (Layer 2 → Cross, UPWARD)
     ──────────────────────────────────────────────────────────────────
     LOCATION: sched.c:5 (#include "eventbus.h")
     FUNCTION: idle_thread calls eventbus_dispatch
     PROBLEM: Idle thread shouldn't depend on event bus.
     FIX: Register idle hook via callback (eventbus_register_idle_hook).
          Or accept cross-cutting dependency as intentional (maintainer
          decision).

  3. NOT a violation (but fragile):
     eventbus.c includes sync.h (Cross → L2)
     watchdog.c includes sched.h, pmm.h, eventbus.h (Cross → all lower)
     These are cross-cutting by design. Acceptable.

  4. UNNECESSARY includes (not violations, but misleading):
     pmm.c includes hal.h (uses nothing from HAL)
     vmm.c includes hal.h (uses nothing from HAL)
     shell.c includes vmm.h (uses nothing from VMM)
     sync.c includes hal.h (uses nothing from HAL)
     FIX: Remove these includes.
================================================================================

  PHASE 1 — Hardening & Bug Fixes (immediate — ~1 week)
  ───────────────────────────────────────────────────────────────────────
  [ ] Fix C1: PMM synchronization — wrap all alloc/free with hal_save_irq
  [ ] Fix C2: irq_handlers[] barrier — add memory fence in register
  [ ] Fix C3: schedule() re-enables IRQs before switch_context — move
              hal_restore_irq after switch_context
  [ ] Fix C4: check_sleepers cli — wrap iteration, change to use
              all_threads list instead of run_queues
  [ ] Fix C5: sched_foreach cli — wrap with hal_save_irq
  [ ] Fix C6: next_thread_id atomic — use __sync_fetch_and_add
  [ ] Fix C7: thread_sleep → check_sleepers uses global list,
              woken threads re-added to run queue
  [ ] Fix C8: kprintf %x → replace recursive kprintf with kputs + local loop
  [ ] Fix C9: pmm_alloc_pages TOCTOU (covered by C1)
  [ ] Fix C10: thread_set_priority → save old priority before removal
  [ ] Fix C11: hal_save_irq → add "memory" clobber
  [ ] Fix C12: timer_ticks overflow → use unsigned __int128 multiplication
  [ ] Fix C13: spinlock_acquire → disable interrupts (cli)
  [ ] Fix H1: PIC mask → 0xFB (mask keyboard IRQ1)
  [ ] Fix H4: sched_foreach cli (covered by C5)
  [ ] Fix H6: pmm_free_page atomic (covered by C1)
  [ ] Fix H8: idle STI-HLT race → add monitor/mwait support
  [ ] Fix M1: remove duplicate PAGE_SIZE from pmm.h
  [ ] Fix M7/M8: remove schedule_trampoline, cleanup_arg
  [ ] Fix LLN: remove unused includes (pmm.c→hal.h, vmm.c→hal.h,
              shell.c→vmm.h, sync.c→hal.h)
  [ ] Fix race: sched_thread_count should disable interrupts when reading
  [ ] All changes tested with: demo + compute + mutex + event + cleanup
       + stress (20 threads + 100 events + mutex 5×50)

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

  PORTING PHASES (after C kernel is fully stable — audit findings applied):

  Phase R1 — Core Types & Library (Rust no_std)
  ────────────────────────────────────────────────────────────────────────
  [ ] Port types.h → core::types (uint64_t, err_t, etc.)
  [ ] Port klib.c → core::io (kputchar, kprintf, kputs) — FIXED: no recursive
  [ ] Port klib.c → core::mem (kmemset, kmemcpy, etc.)
  [ ] Port errno.h → core::error (err_t enum, err_str)
  [ ] All pure-C code with no hardware dependency
  [ ] AUDIT LESSON: Rust's core::fmt eliminates kprintf UB entirely

  Phase R2 — HAL Layer (Rust with inline assembly)
  ────────────────────────────────────────────────────────────────────────
  [ ] Port hal.c → hal:: (GDT, IDT, PIC, UART, timer)
  [ ] Port boot.S → boot:: (entry point, page tables)
  [ ] Port isr.S → hal::interrupt (ISR stubs, common handler)
  [ ] Port ctx.S → hal::context (switch_context, trampolines)
  [ ] Use core::arch::asm! for inline assembly — SAFETY documented per block
  [ ] AUDIT LESSON: Rust's atomics and memory ordering (+Sync/Send traits)
      eliminate all 6 race-condition criticals (C1-C6) at compile time.
      No cli/sti needed — spinlock uses atomic::Ordering::Acquire/Release.

  Phase R3 — Kernel Layers (Rust)
  ────────────────────────────────────────────────────────────────────────
  [ ] Port pmm.c → pmm:: (buddy allocator instead of bitmap+free list)
  [ ] Port vmm.c → vmm:: (4-level page tables, map/unmap)
  [ ] Port sched.c → sched:: (thread, run queue, scheduler) — FIXED: O(1)
      bitmap + no TOCTOU races
  [ ] Port sync.c → sync:: (spinlock with Ordering::AcqRel, mutex)
  [ ] Port shell.c → shell:: (interactive CLI)
  [ ] Port eventbus.c → eventbus:: (publish/subscribe with AtomicUsize
      counters instead of spinlock)
  [ ] Port watchdog.c → watchdog:: (health monitoring)
  [ ] AUDIT LESSON: thread_sleep fixed by design (Condvar + atomic state).
      Mutex timeout fixed by design (Condvar::wait_timeout).
      Integer overflow impossible with checked/wrapping arithmetic.

  Phase R4 — Embedded Target Adaptation
  ────────────────────────────────────────────────────────────────────────
  [ ] Replace HAL with target-specific implementations:
  [ ] Cortex-M: NVIC instead of PIC, SysTick instead of PIT
  [ ] RISC-V: CLINT/PLIC, mtime/mcycle
  [ ] Remove x86-64 specific features (paging, long mode init)
  [ ] Add stack guard pages (MPU on Cortex-M, PMP on RISC-V)
  [ ] Add FPGA/ASIC-friendly design: fixed-address peripherals,
      no MMU (single address space), all sizes configurable at
      compile time via const generics or feature flags
  [ ] Size optimization: link-time garbage collection, LTO,
      panic = abort, no compiler-rt builtins, no float
  [ ] AUDIT LESSON: Remove ALL dead code (29 functions) to minimize
      binary. Slab allocator for thread_t/mutex_t (pack 21 objects/page
      instead of 1/page). Memory waste eliminated.

  Phase R5 — Certification & Verification
  ────────────────────────────────────────────────────────────────────────
  [ ] Add formal verification with Kani or Verus for:
        - Scheduler: pick_next is O(1), no starvation
        - PMM: no double-free, no overlapping allocations
        - Spinlock: no deadlock (interrupt-safe lock ordering)
  [ ] Test with proptest/fuzzcheck for edge-case discovery
  [ ] Document unsafe{} blocks with SAFETY comments explaining:
        - Why inline asm is safe (preconditions checked)
        - Why pointer dereferences are valid (alignment, liveness)
        - Why memory ordering is correct (which operations pair)
  [ ] Size budget: < 32KB flash, < 8KB RAM for minimal config
        (Cortex-M0 with no MMU, no FPU, no atomic instructions)
  [ ] Real-time: bounded worst-case execution time (WCET) analysis
        for scheduler, ISR, IPC hot paths

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

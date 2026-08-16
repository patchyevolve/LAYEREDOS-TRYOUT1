#include "kernel.h"
#include "sched.h"
#include "pmm.h"
#include "vmm.h"
#include "kmalloc.h"
#include "hal.h"
#include "watchdog.h"
#include "eventbus.h"
#include "process.h"
#include "sync.h"
#include "smp.h"
#include "apic.h"
#include "rcu.h"
#include "kernel_invariant.h"

uint64_t next_thread_id = 1;
static uint64_t kernel_cr3 = 0;
static thread_t* idle_thr = NULL;  /* CPU 0 (BSP) idle thread */
static thread_t* all_threads_head = NULL;
static thread_t* all_threads_tail = NULL;
static uint32_t all_threads_count = 0;
static uint64_t sched_switch_count = 0;
static uint64_t sched_yield_count = 0;
volatile int sched_running = 0;
thread_t* current_thread_global = NULL;

static spinlock_t all_threads_lock;
spinlock_t sched_queue_lock;

static inline per_cpu_data_t* sched_pcp(void) {
    return per_cpu_data[smp_cpu_id()];
}

/* Called after switch_context returns on the new thread's stack.
 * Finalizes the thread retired by this CPU, transitioning it from
 * THREAD_ZOMBIE to THREAD_TERMINATED so sched_reap_zombies() can
 * safely free its stack (now fully vacated by the context switch). */
static inline void sched_finalize_retiring(void) {
    per_cpu_data_t* pcp = sched_pcp();
    uint64_t retiring = pcp->retiring_thread;
    if (retiring) {
        pcp->retiring_thread = 0;
        ((thread_t*)(uintptr_t)retiring)->state = THREAD_TERMINATED;
    }
}

void all_threads_add(thread_t* t) {
    if (!t) return;
    cpu_flags_t flags;
    spinlock_acquire(&all_threads_lock, &flags);
    if (t->all_next || t->all_prev) { spinlock_release(&all_threads_lock, flags); return; }
    t->all_next = NULL;
    t->all_prev = all_threads_tail;
    if (all_threads_tail)
        all_threads_tail->all_next = t;
    else
        all_threads_head = t;
    all_threads_tail = t;
    all_threads_count++;
    spinlock_release(&all_threads_lock, flags);
}

static void all_threads_remove(thread_t* t) {
    if (t->all_prev)
        t->all_prev->all_next = t->all_next;
    else
        all_threads_head = t->all_next;
    if (t->all_next)
        t->all_next->all_prev = t->all_prev;
    else
        all_threads_tail = t->all_prev;
    t->all_next = NULL;
    t->all_prev = NULL;
    all_threads_count--;
}

void sched_foreach(void (*cb)(thread_t* t, void* ctx), void* ctx) {
    if (!cb) return;
    cpu_flags_t flags;
    spinlock_acquire(&all_threads_lock, &flags);
    thread_t* t = all_threads_head;
    while (t) {
        thread_t* next = t->all_next;
        cb(t, ctx);
        t = next;
    }
    spinlock_release(&all_threads_lock, flags);
}

uint32_t sched_thread_count(void) {
    cpu_flags_t flags;
    spinlock_acquire(&all_threads_lock, &flags);
    uint32_t count = all_threads_count;
    spinlock_release(&all_threads_lock, flags);
    return count;
}

void sched_reap_zombies(void) {
    /* Collect reap candidates under all_threads_lock, then process
     * outside the lock.  This avoids an ABBA deadlock: the guard-page
     * remap calls vmm_flush_tlb_page() which may send a cross-CPU IPI
     * and spin-wait for an ack.  If another CPU holds all_threads_lock
     * with IRQs off (spinlock_acquire disables interrupts), it can
     * never service the IPI → deadlock. */
#define MAX_REAP_BATCH 64
    thread_t* to_reap[MAX_REAP_BATCH];
    int n_reap = 0;

    cpu_flags_t flags;
    spinlock_acquire(&all_threads_lock, &flags);
    thread_t* t = all_threads_head;
    while (t && n_reap < MAX_REAP_BATCH) {
        thread_t* next = t->all_next;
        cpu_flags_t jflags;
        if (t->state == THREAD_TERMINATED && t != idle_thr) {
            spinlock_acquire(&t->join_queue.lock, &jflags);
            if (t->join_queue.count == 0) {
                all_threads_remove(t);
                if (t->proc)
                    list_del(&t->threads_node);
                to_reap[n_reap++] = t;
            }
            spinlock_release(&t->join_queue.lock, jflags);
        }
        t = next;
    }
    spinlock_release(&all_threads_lock, flags);

    /* Process outside the lock — guard-page remap may send TLB IPIs */
    for (int i = 0; i < n_reap; i++) {
        thread_t* rt = to_reap[i];
        /* Safety net: ensure the thread is not on any run queue before
         * freeing its TCB and kernel stack.  If thread_exit() missed the
         * removal (e.g., the thread was stolen by sched_balance_push()),
         * we must dequeue it now — otherwise another CPU's pick_next()
         * will later return the freed TCB → switch_context loads corrupt
         * rsp from the reallocated memory → UAF crash. */
        if (rt->rq_next || rt->rq_prev)
            sched_remove_thread(rt);
        if (rt->kernel_stack) {
            uint64_t __guard_phys = rt->block_phys + PAGE_SIZE;
            uint64_t __gv = (uint64_t)PHYS_TO_VIRT(__guard_phys);
            page_entry_t* __gpte = vmm_peek_pte(vmm_get_kernel_pml4(), __gv);
            if (__gpte) {
                *__gpte = __guard_phys | PAGE_PRESENT | PAGE_WRITE;
                vmm_flush_tlb_page(__gv);
            }
            uint32_t __np = (rt->kernel_stack_size + PAGE_SIZE - 1) / PAGE_SIZE + 2;
            pmm_free_pages(rt->block_phys, __np);
        }
    }
#undef MAX_REAP_BATCH
}

static void sched_add_thread_to_cpu_locked(thread_t* t, int cpu) {
    if (!t || t->priority < 0 || t->priority > THREAD_MAX_PRIO)
        return;
    if (cpu < 0 || cpu >= MAX_CPUS || !per_cpu_data[cpu])
        return;

    per_cpu_data_t* pcp = per_cpu_data[cpu];
    uint32_t prio = t->priority;

    thread_t* tail = (thread_t*)pcp->rq_tails[prio];
    if (tail) {
        tail->rq_next = t;
    } else {
        pcp->rq_heads[prio] = (void*)t;
        pcp->priority_bitmap[prio / 64] |= (1ULL << (prio % 64));
    }
    t->rq_prev = tail;
    t->rq_next = NULL;
    pcp->rq_tails[prio] = (void*)t;
    pcp->rq_counts[prio]++;
    pcp->rq_total++;
    t->cpu_queue = cpu;
    t->state = THREAD_READY;
}

static void sched_add_thread_to_cpu(thread_t* t, int cpu) {
    cpu_flags_t flags;
    spinlock_acquire(&sched_queue_lock, &flags);
    sched_add_thread_to_cpu_locked(t, cpu);

    /* If the target CPU is different from the current one and idle,
     * send a reschedule IPI so it picks up the new thread immediately.
     * This handles the normal cross-CPU placement path. */
    if (cpu != smp_cpu_id() && per_cpu_data[cpu]) {
        per_cpu_data_t* tgt = per_cpu_data[cpu];
        if ((thread_t*)tgt->cpu_thread == (thread_t*)tgt->idle_thread) {
            spinlock_release(&sched_queue_lock, flags);
            smp_send_reschedule(cpu);
            return;
        }
    }

    spinlock_release(&sched_queue_lock, flags);
}

void sched_add_thread(thread_t* t) {
    if (!t) return;
    int cpu = smp_cpu_id();
    /* If the thread's affinity doesn't include the current CPU, find
     * the first allowed CPU and place it there.  This ensures threads
     * bound to a specific CPU via sched_set_thread_affinity actually
     * land on the right run queue.
     * If cpu_affinity is 0 (uninitialized / edge case), stay on the
     * current CPU rather than iterating an empty mask. */
    if (t->cpu_affinity != 0 && !(t->cpu_affinity & (1ULL << cpu))) {
        int mask = t->cpu_affinity;
        for (int c = 0; c < nr_cpus; c++) {
            if (mask & (1ULL << c)) { cpu = c; break; }
        }
    }
    sched_add_thread_to_cpu(t, cpu);
}

/* Place a thread directly on a specific CPU's run queue.
 * The caller must ensure the thread is not already queued. */
void sched_place_thread(thread_t* t, int cpu) {
    if (!t || cpu < 0 || cpu >= MAX_CPUS || !per_cpu_data[cpu])
        return;
    sched_add_thread_to_cpu(t, cpu);
}

static void sched_add_thread_locked(thread_t* t) {
    sched_add_thread_to_cpu_locked(t, smp_cpu_id());
}

static void sched_remove_thread_locked(thread_t* t) {
    if (!t) return;
    int cpu = t->cpu_queue;
    if (cpu < 0 || cpu >= MAX_CPUS || !per_cpu_data[cpu]) {
        return;
    }
    per_cpu_data_t* pcp = per_cpu_data[cpu];
    uint32_t prio = t->priority;

    thread_t* head = (thread_t*)pcp->rq_heads[prio];

    if (t->rq_prev == NULL && t->rq_next == NULL && head != t) {
        return;
    }

    if (t->rq_prev)
        t->rq_prev->rq_next = t->rq_next;
    else
        pcp->rq_heads[prio] = (void*)t->rq_next;

    if (t->rq_next)
        t->rq_next->rq_prev = t->rq_prev;
    else
        pcp->rq_tails[prio] = (void*)t->rq_prev;

    pcp->rq_counts[prio]--;
    pcp->rq_total--;

    if (pcp->rq_counts[prio] == 0)
        pcp->priority_bitmap[prio / 64] &= ~(1ULL << (prio % 64));

    t->rq_next = NULL;
    t->rq_prev = NULL;
}

void sched_remove_thread(thread_t* t) {
    if (!t) return;
    cpu_flags_t flags;
    spinlock_acquire(&sched_queue_lock, &flags);
    sched_remove_thread_locked(t);
    spinlock_release(&sched_queue_lock, flags);
}

static int bitmap_find_highest(per_cpu_data_t* pcp) {
    for (int i = 3; i >= 0; i--) {
        if (pcp->priority_bitmap[i]) {
            int bit = 63 - __builtin_clzll(pcp->priority_bitmap[i]);
            return i * 64 + bit;
        }
    }
    return -1;
}

/* Try to steal a thread from another CPU's run queue.
 * Called when the local run queue is empty. Returns the stolen thread
 * (already dequeued from the source CPU) or NULL.
 * NOTE: `this_cpu` must be the CACHED CPU ID from schedule() — never call
 * smp_cpu_id() here because on TCG 4-CPU it can return inconsistent values. */
static thread_t* sched_steal_thread(int this_cpu) {
    int ncpus = smp_enabled ? nr_cpus : 1;
    if (ncpus < 2) return NULL;

    /* Scan other CPUs in round-robin order starting from this_cpu + 1 */
    for (int i = 0; i < ncpus - 1; i++) {
        int target = (this_cpu + 1 + i) % ncpus;
        if (target == this_cpu || !per_cpu_data[target]) continue;

        cpu_flags_t qflags;
        if (!spinlock_try_acquire(&sched_queue_lock, &qflags)) {
            /* Contended — skip this CPU and try the next */
            continue;
        }

        per_cpu_data_t* tgt = per_cpu_data[target];
        int tprio = bitmap_find_highest(tgt);
        if (tprio < 0) {
            spinlock_release(&sched_queue_lock, qflags);
            continue;
        }

        thread_t* t = (thread_t*)tgt->rq_heads[tprio];
        if (!t || t == (thread_t*)tgt->idle_thread) {
            spinlock_release(&sched_queue_lock, qflags);
            continue;
        }

        /* Only steal threads allowed to run on this CPU */
        if (!(t->cpu_affinity & (1ULL << this_cpu))) {
            spinlock_release(&sched_queue_lock, qflags);
            continue;
        }

        /* Dequeue from target */
        if (t->rq_next) {
            tgt->rq_heads[tprio] = (void*)t->rq_next;
            t->rq_next->rq_prev = NULL;
        } else {
            tgt->rq_heads[tprio] = NULL;
            tgt->rq_tails[tprio] = NULL;
        }
        tgt->rq_counts[tprio]--;
        tgt->rq_total--;
        if (tgt->rq_counts[tprio] == 0)
            tgt->priority_bitmap[tprio / 64] &= ~(1ULL << (tprio % 64));
        t->rq_next = NULL;
        t->rq_prev = NULL;
        t->cpu_queue = this_cpu;

        spinlock_release(&sched_queue_lock, qflags);

        /* If the target CPU was idle, send IPI so it re-evaluates */
        if ((thread_t*)tgt->cpu_thread == (thread_t*)tgt->idle_thread)
            smp_send_reschedule(target);

        return t;
    }
    return NULL;
}

/* Push one thread from this CPU's queue to an underloaded sibling.
 * Called from sched_timer_tick when this CPU is overloaded.
 * Uses try_acquire so it is safe in ISR context — skips if contended.
 * NOTE: `this_cpu` and `pcp` must come from the CACHED schedule() values. */
static void sched_balance_push(int this_cpu, per_cpu_data_t* pcp) {
    int ncpus = smp_enabled ? nr_cpus : 1;
    if (ncpus < 2) return;
    if (pcp->rq_total <= 1) return;

    /* Compute average load */
    uint32_t total = 0;
    for (int i = 0; i < ncpus; i++) {
        if (per_cpu_data[i])
            total += per_cpu_data[i]->rq_total;
    }
    uint32_t avg = total / ncpus;
    if (pcp->rq_total <= avg + 2) return;

    /* Find the lowest populated priority level */
    cpu_flags_t qflags;
    if (!spinlock_try_acquire(&sched_queue_lock, &qflags))
        return;

    int low_prio = -1;
    for (int p = THREAD_MAX_PRIO; p >= 0; p--) {
        if (pcp->rq_counts[p] > 0) {
            low_prio = p;
            break;
        }
    }
    if (low_prio < 0) {
        spinlock_release(&sched_queue_lock, qflags);
        return;
    }

    /* Scan for a migratable thread and find the best target CPU */
    thread_t* t = NULL;
    int target = -1;
    for (thread_t* ct = (thread_t*)pcp->rq_heads[low_prio]; ct; ct = ct->rq_next) {
        uint64_t aff = ct->cpu_affinity & ~(1ULL << this_cpu);
        if (aff == 0) continue;

        int best = -1;
        uint32_t best_load = 0xFFFFFFFFU;
        for (int i = 0; i < ncpus; i++) {
            if (i == this_cpu || !per_cpu_data[i]) continue;
            if (!(ct->cpu_affinity & (1ULL << i))) continue;
            uint32_t load = per_cpu_data[i]->rq_total;
            if (load < best_load) {
                best_load = load;
                best = i;
            }
        }
        if (best >= 0 && best_load + 2 < pcp->rq_total) {
            t = ct;
            target = best;
            break;
        }
    }

    if (!t) {
        spinlock_release(&sched_queue_lock, qflags);
        return;
    }

    /* Dequeue from local queue — handles both head and middle removal */
    if (t->rq_prev) {
        t->rq_prev->rq_next = t->rq_next;
    } else {
        pcp->rq_heads[low_prio] = (void*)t->rq_next;
    }
    if (t->rq_next) {
        t->rq_next->rq_prev = t->rq_prev;
    } else {
        pcp->rq_tails[low_prio] = (void*)t->rq_prev;
    }
    pcp->rq_counts[low_prio]--;
    pcp->rq_total--;
    if (pcp->rq_counts[low_prio] == 0)
        pcp->priority_bitmap[low_prio / 64] &= ~(1ULL << (low_prio % 64));
    t->rq_next = NULL;
    t->rq_prev = NULL;

    /* Add to target queue */
    per_cpu_data_t* tgt_pcp = per_cpu_data[target];
    uint32_t prio = t->priority;
    thread_t* tail = (thread_t*)tgt_pcp->rq_tails[prio];
    if (tail) {
        tail->rq_next = t;
    } else {
        tgt_pcp->rq_heads[prio] = (void*)t;
        tgt_pcp->priority_bitmap[prio / 64] |= (1ULL << (prio % 64));
    }
    t->rq_prev = tail;
    t->rq_next = NULL;
    tgt_pcp->rq_tails[prio] = (void*)t;
    tgt_pcp->rq_counts[prio]++;
    tgt_pcp->rq_total++;
    t->cpu_queue = target;

    int tgt_is_idle = (thread_t*)tgt_pcp->cpu_thread == (thread_t*)tgt_pcp->idle_thread;
    spinlock_release(&sched_queue_lock, qflags);

    /* Wake target CPU if idle */
    if (tgt_is_idle)
        smp_send_reschedule(target);
}

static thread_t* pick_next(per_cpu_data_t* pcp) {
    int this_cpu = pcp->cpu_id;
    int prio = bitmap_find_highest(pcp);
    if (prio < 0) {
        /* Local queue is empty — try to steal from another CPU */
        thread_t* stolen = sched_steal_thread(this_cpu);
        if (stolen) return stolen;
        return (thread_t*)pcp->idle_thread;
    }

    /* Proactive stealing: if the best local thread is low-priority,
     * try to steal a higher-priority thread from a loaded sibling. */
    if (prio > 150 && smp_enabled && nr_cpus > 1) {
        thread_t* stolen = sched_steal_thread(this_cpu);
        if (stolen) {
            if (stolen->priority < prio)
                return stolen;
            /* Stolen thread didn't meet priority threshold — re-queue locally */
            sched_add_thread(stolen);
        }
        /* Fall through to local queue. */
    }

    cpu_flags_t qflags;
    if (!spinlock_try_acquire(&sched_queue_lock, &qflags)) {
        /* Contended — skip this round; called from ISR context. */
        thread_t* stolen = sched_steal_thread(this_cpu);
        if (stolen) return stolen;
        return (thread_t*)pcp->idle_thread;
    }
    thread_t* t = (thread_t*)pcp->rq_heads[prio];
    if (!t || t == (thread_t*)pcp->idle_thread) {
        spinlock_release(&sched_queue_lock, qflags);
        /* Race: thread was dequeued between bitmap check and lock.
         * Try stealing instead of returning idle immediately. */
        thread_t* stolen = sched_steal_thread(this_cpu);
        if (stolen) return stolen;
        return (thread_t*)pcp->idle_thread;
    }

    if (t->rq_next) {
        pcp->rq_heads[prio] = (void*)t->rq_next;
        ((thread_t*)pcp->rq_heads[prio])->rq_prev = NULL;
    } else {
        pcp->rq_heads[prio] = NULL;
        pcp->rq_tails[prio] = NULL;
    }
    pcp->rq_counts[prio]--;
    pcp->rq_total--;
    if (pcp->rq_counts[prio] == 0)
        pcp->priority_bitmap[prio / 64] &= ~(1ULL << (prio % 64));
    t->rq_next = NULL;
    t->rq_prev = NULL;
    spinlock_release(&sched_queue_lock, qflags);

    return t;
}

void sched_set_kernel_cr3(uint64_t cr3) {
    kernel_cr3 = cr3;
}

static inline void sched_sync_current(thread_t* old, thread_t* next) {
    (void)old;
    current_thread = next;  /* macro: writes to per-CPU slot when CONFIG_SMP */
}

static inline void sched_check_stack(thread_t* t) {
    if (!t->kernel_stack) return;
    uint64_t rsp;
    asm volatile("mov %%rsp, %0" : "=r"(rsp));
    uint64_t base = (uint64_t)t->kernel_stack;
    uint64_t top = base + t->kernel_stack_size;
    /* Check for UNDERFLOW: if RSP is within 16 KB below the stack base,
     * the thread has overflowed its stack.  If RSP is far below the base
     * (e.g., the thread is running on a bootstrap stack during early init
     * before its own kernel stack is active), just warn — this is normal
     * during thread creation transitions. */
    if (rsp < base) {
        int64_t gap = base - rsp;
        if (gap <= (int64_t)t->kernel_stack_size) {
            kprintf("[SCHED] STACK OVERFLOW on thread '%s' (id=0x%llx): "
                    "rsp=0x%llx stack=[0x%llx-0x%llx) gap=%lld\n",
                    t->name, t->id, rsp, base, top, gap);
            kpanic("Kernel stack overflow detected");
        } else if (gap <= 0x100000) {
            kprintf("[SCHED] WARNING: thread '%s' (id=0x%llx) "
                    "rsp=0x%llx outside stack=[0x%llx-0x%llx) gap=%lld\n",
                    t->name, t->id, rsp, base, top, gap);
        }
    }
    /* Check for near-bottom: within 256 bytes of the base */
    if (rsp >= base && rsp - base < 256) {
        kprintf("[SCHED] WARNING: thread '%s' (id=0x%llx) "
                "has only %llu bytes of stack remaining\n",
                t->name, t->id, rsp - base);
    }
}

void schedule(void) {
    /* Cache CPU ID once — ALL subsequent accesses to per-CPU data use this
     * value.  On TCG, smp_cpu_id() can return different values across
     * successive calls within the same schedule() invocation because
     * apic_read(APIC_REG_ID) may be non-deterministic with 4 TCG vCPUs. */
    int sched_cpu = smp_cpu_id();
    /* Diagnostics: validate current_thread's kernel stack.
     * Only runs on first invocation per context switch (before invariant checks). */
    {   uint64_t _rs;
        asm volatile("mov %%rsp, %0" : "=r"(_rs));
        thread_t* _ct = current_thread;
        if (_ct && _ct->kernel_stack &&
            (uint64_t)_ct > 0xFFFF800000000000ULL &&
            (_rs < (uint64_t)_ct->kernel_stack || _rs > (uint64_t)_ct->kernel_stack + _ct->kernel_stack_size)) {
            kprintf("\n[SCHED STACK] rsp=%p cur='%s' id=0x%llx state=%d "
                    "stack=[%p-%p) rq_prev=%p rq_next=%p\n",
                    (void*)_rs, _ct->name, _ct->id, _ct->state,
                    (void*)_ct->kernel_stack,
                    (void*)((uint64_t)_ct->kernel_stack + _ct->kernel_stack_size),
                    _ct->rq_prev, _ct->rq_next);
        }
    }
    if (sched_cpu < 0 || sched_cpu >= MAX_CPUS) {
        static const char _msg[] = "\n[SCHED FATAL] smp_cpu_id=";
        for (const char* _p = _msg; *_p; _p++) kputchar(*_p);
        kputchar('0' + (sched_cpu > 9 ? 0 : (sched_cpu < 0 ? '-' : sched_cpu % 10)));
        kputchar('\n');
        asm volatile("cli; 1: hlt; jmp 1b");
    }
    per_cpu_data_t* pcp = per_cpu_data[sched_cpu];
    if (!pcp || pcp->cpu_id != sched_cpu) {
        kprintf("\n[SCHED FATAL] per_cpu_data[%d]=%p cpu_id=%d\n",
                sched_cpu, (void*)pcp, pcp ? pcp->cpu_id : -1);
        asm volatile("cli; 1: hlt; jmp 1b");
    }

    if (!sched_running || !pcp->cpu_thread) return;

    /* Report RCU quiescent state — every context switch is a QS */
    rcu_quiescent_state();

    /* Clear watchdog stuck flag — any context switch proves forward
     * progress, clearing any false-positive NMI stuck detection from
     * boot (before the idle thread was recognized). */
    watchdog_clear_stuck();

    kernel_validate_invariants();

    /* Check for expired sleepers every ~10ms via RDTSC — on KVM SMP,
     * APIC/PIT timer interrupts can stop after AP bring-up, freezing
     * hal_timer_get_ticks().  RDTSC always advances.
     * Uses per-CPU last_check_rdtsc (not a shared static) to avoid
     * races between vCPUs on TCG. */
    {
        uint64_t now = rdtsc();
        uint64_t interval = tsc_khz * 10;
        if (now - pcp->last_check_rdtsc >= interval || pcp->last_check_rdtsc == 0) {
            pcp->last_check_rdtsc = now;
            (void)check_sleepers();
        }
    }

    /* Periodic load balancing (~100ms) via RDTSC */
    {
        uint64_t now = rdtsc();
        if (now - pcp->last_balance_rdtsc > (uint64_t)tsc_khz * 100) {
            pcp->last_balance_rdtsc = now;
            sched_balance_push(sched_cpu, pcp);
        }
    }
    pcp->need_reschedule = 0;
    cpu_flags_t flags = hal_save_irq();

    /* Use pcp->cpu_thread directly to avoid redundant smp_cpu_id() calls
     * from the current_thread macro — on TCG with reduced nr_cpus,
     * smp_cpu_id() can return 0 for a CPU whose APIC ID is not in the
     * cpu_info array, causing reads/writes to the wrong per-CPU slot. */
    thread_t* cur = (thread_t*)pcp->cpu_thread;

    sched_check_stack(cur);

    thread_t* next = pick_next(pcp);

    if (next == cur) {
        if (cur->state == THREAD_RUNNING &&
            cur != (thread_t*)pcp->idle_thread)
            sched_add_thread(next);
        hal_restore_irq(flags);
        return;
    }

    if (cur->state == THREAD_RUNNING &&
        cur != (thread_t*)pcp->idle_thread) {
        cur->state = THREAD_READY;
        cur->priority = cur->base_priority;
        cur->age_ticks = 0;
        sched_add_thread(cur);
    }

    /* Validate next pointer — catch UAF / non-canonical before GP */
    if ((uint64_t)next < 0xFFFF800000000000ULL || (uint64_t)next >= 0xFFFFFFFFFFFFF000ULL) {
        kprintf("[SCHED] FATAL: next=%p (non-canonical or NULL) cpu=%d\n", (void*)next, sched_cpu);
        kprintf("[SCHED] cur=%p idle=%p\n", (void*)cur, (void*)pcp->idle_thread);
        kprintf("[SCHED] pcp->rq_heads[0..3]=%p %p %p %p\n",
                (void*)pcp->rq_heads[0], (void*)pcp->rq_heads[1],
                (void*)pcp->rq_heads[2], (void*)pcp->rq_heads[3]);
        kpanic("pick_next returned invalid thread pointer");
    }

    /* Validate next->rsp is within its own kernel stack — catch TCB
     * field corruption (from UAF or stack overflow) before switch_context
     * loads garbage RSP and jumps to a random address. */
    if (next->kernel_stack) {
        uint64_t ns_base = (uint64_t)next->kernel_stack;
        uint64_t ns_top  = ns_base + next->kernel_stack_size;
        if (next->rsp < ns_base || next->rsp >= ns_top) {
            kprintf("[SCHED] FATAL: next='%s' rsp=%p outside stack=[%p-%p) cpu=%d\n",
                    next->name, (void*)next->rsp, (void*)ns_base, (void*)ns_top, sched_cpu);
            kprintf("[SCHED] cur='%s' state=%d idle=%p next->state=%d\n",
                    cur->name, cur->state, (void*)pcp->idle_thread, next->state);
            kpanic("next->rsp corrupted before switch_context");
        }
    }
    uint64_t target_cr3 = next->cr3 ? next->cr3 : kernel_cr3;
    if (target_cr3) {
        asm volatile("mov %0, %%cr3" : : "r"(target_cr3) : "memory");
    }

    thread_t* old = cur;
    pcp->cpu_thread = next;
    next->state = THREAD_RUNNING;
    next->time_slice_remaining = THREAD_TIME_SLICE;
    sched_switch_count++;

    uint64_t kstack_top = (uint64_t)next->kernel_stack + next->kernel_stack_size;
    hal_set_kernel_stack(kstack_top);

    switch_context(&old, (thread_t**)&pcp->cpu_thread);

    /* Finalize any thread retired on this CPU — the previous thread's
     * stack is now fully vacated.  This is reached when a preempted or
     * yielded thread is switched back to. */
    sched_finalize_retiring();

    hal_restore_irq(flags);
}

void thread_yield(void) {
    sched_yield_count++;
    cpu_flags_t flags = hal_save_irq();
    current_thread->time_slice_remaining = 0;
    schedule();
    hal_restore_irq(flags);
}

thread_t* thread_create(void (*func)(void*), void* arg,
                        int priority, const char* name) {
    uint32_t stack_pages = (THREAD_STACK_SIZE + PAGE_SIZE - 1) / PAGE_SIZE;
    /* Layout: [TCB @ 0] [GUARD (unmapped) @ 1 page] [stack @ pages 2..1+stack_pages]
     * Total = 1 (TCB) + 1 (guard) + stack_pages */
    uint32_t total_pages = stack_pages + 2;

    uint64_t block_phys = pmm_alloc_node_pages(total_pages, pmm_current_node());
    if (!block_phys) return NULL;

    /* TCB in the LOW page */
    thread_t* tcb = (thread_t*)PHYS_TO_VIRT(block_phys);
    kmemset(tcb, 0, sizeof(thread_t));

    /* Guard page between TCB and stack — unmapped from kernel identity map.
     * Stack overflow into this page triggers a page fault (→ double fault → kpanic)
     * instead of silently corrupting adjacent data. */
    uint64_t guard_phys = block_phys + PAGE_SIZE;
    uint64_t guard_virt = (uint64_t)PHYS_TO_VIRT(guard_phys);
    page_entry_t* gpte = vmm_walk_pagetable(vmm_get_kernel_pml4(), guard_virt);
    if (gpte) {
        uint64_t saved = *gpte;
        *gpte = 0;
        vmm_flush_tlb_page(guard_virt);
        (void)saved;
    }

    void* kstack = (void*)((uint8_t*)PHYS_TO_VIRT(block_phys) + 2 * PAGE_SIZE);
    kmemset(kstack, 0, THREAD_STACK_SIZE);
    uint64_t kstack_top = (uint64_t)kstack + THREAD_STACK_SIZE;

    uint64_t* sp = (uint64_t*)kstack_top;

    *(--sp) = (uint64_t)thread_trampoline;
    *(--sp) = (uint64_t)func;       /* r15 = func */
    *(--sp) = (uint64_t)arg;        /* r14 = arg */
    *(--sp) = 0;                    /* r13 = 0 (unused callee-saved) */
    *(--sp) = 0;                    /* r12 = 0 (unused callee-saved) */
    *(--sp) = 0;                    /* rbp = 0 (unused callee-saved) */
    *(--sp) = 0;                    /* rbx = 0 (unused callee-saved) */

    tcb->id = __sync_fetch_and_add(&next_thread_id, 1);
    tcb->rsp = (uint64_t)sp;
    tcb->cr3 = 0;
    tcb->state = THREAD_CREATED;
    tcb->base_priority = tcb->priority = (priority < 0 || priority > THREAD_MAX_PRIO) ? THREAD_DEF_PRIO : priority;
    tcb->time_slice_remaining = 0;
    tcb->age_ticks = 0;
    tcb->cpu_affinity = 0xFF;  /* allow all CPUs by default (up to 8) */
    tcb->kernel_stack = kstack;
    tcb->kernel_stack_size = THREAD_STACK_SIZE;
    tcb->block_phys = block_phys;
    wait_queue_init(&tcb->join_queue);
    kstrncpy(tcb->name, name ? name : "thread", THREAD_NAME_MAX - 1);

    all_threads_add(tcb);

    return tcb;
}

void thread_exit(int exit_code) {
    if (!current_thread) return;

    /* Finalize any prior retiring thread on this CPU first (before
     * publishing a new one).  After the switch_context below,
     * execution continues wherever the target thread was last
     * preempted — NOT at the next line — so the finalize must be
     * called from schedule(), the idle loop, or the next thread_exit. */
    sched_finalize_retiring();

    /* Remove from any run queue before setting ZOMBIE — the thread might
     * have been stolen by sched_balance_push() while READY on another CPU.
     * If it stays in a foreign run queue, that CPU's pick_next() will later
     * return a freed TCB → UAF → crash (wrong RSP restored on context switch). */
    cpu_flags_t flags = hal_save_irq();
    sched_remove_thread(current_thread);
    current_thread->exit_code = exit_code;
    current_thread->state = THREAD_ZOMBIE;
    /* IRQs stay disabled through sched_wake (spinlock save/restore preserves) */
    sched_wake(&current_thread->join_queue);

    per_cpu_data_t* pcp_exit = sched_pcp();
    thread_t* next = pick_next(pcp_exit);
    if (!next) {
        kpanic("No thread to schedule after thread exit!");
    }
    thread_t* old = (thread_t*)pcp_exit->cpu_thread;

    /* Publish the retiring thread before switch_context — the reaper
     * on another CPU won't free the stack until finalize sets TERMINATED
     * (after the context switch has fully vacated the old stack). */
    pcp_exit->retiring_thread = (uint64_t)(uintptr_t)old;

    pcp_exit->cpu_thread = next;
    next->state = THREAD_RUNNING;
    next->time_slice_remaining = THREAD_TIME_SLICE;

    uint64_t target_cr3 = next->cr3 ? next->cr3 : kernel_cr3;
    if (target_cr3) {
        asm volatile("mov %0, %%cr3" : : "r"(target_cr3) : "memory");
    }

    hal_set_kernel_stack((uint64_t)next->kernel_stack + next->kernel_stack_size);

    sched_check_stack(old);

    /* NOTE: switch_context below NEVER returns here — execution
     * continues wherever the target thread was last saved (e.g., in
     * schedule() after its own switch_context call, or in
     * thread_trampoline for brand-new threads).  The ZOMBIE→TERMINATED
     * transition happens from sched_finalize_retiring() in schedule(),
     * idle_thread(), or the next thread_exit() on this CPU. */
    switch_context(&old, (thread_t**)&pcp_exit->cpu_thread);
    hal_restore_irq(flags);
}

void thread_sleep(uint64_t ms) {
    if (!current_thread) return;
    /* Use RDTSC for the wakeup deadline — this is critical on KVM SMP
     * where the PIT/APIC timer stops delivering interrupts after AP
     * bring-up, causing hal_timer_get_ticks() to freeze.  RDTSC always
     * advances regardless of timer hardware state. */
    uint64_t wake_rdtsc = rdtsc() + ms * tsc_khz;
    cpu_flags_t flags = hal_save_irq();
    current_thread->wakeup_rdtsc = wake_rdtsc;
    current_thread->wakeup_tick = 0;
    current_thread->state = THREAD_SLEEPING;
    current_thread->time_slice_remaining = 0;
    schedule();
    hal_restore_irq(flags);
}

err_t thread_join(thread_t* t, int* exit_code) {
    if (!t) return ERR_INVAL;
    if (t == current_thread) return ERR_INVAL;
    cpu_flags_t flags;
    for (;;) {
        spinlock_acquire(&t->join_queue.lock, &flags);
        if (t->state == THREAD_ZOMBIE || t->state == THREAD_TERMINATED) {
            int code = t->exit_code;
            spinlock_release(&t->join_queue.lock, flags);
            if (exit_code) *exit_code = code;
            break;
        }
        if (current_thread->state == THREAD_READY ||
            current_thread->state == THREAD_RUNNING)
            sched_remove_thread(current_thread);
        current_thread->state = THREAD_BLOCKED;
        current_thread->wq_next = t->join_queue.waiters;
        t->join_queue.waiters = current_thread;
        t->join_queue.count++;
        spinlock_release(&t->join_queue.lock, flags);
        schedule();
    }
    return ERR_OK;
}

void wait_queue_init(wait_queue_t* wq) {
    if (!wq) return;
    wq->waiters = NULL;
    wq->count = 0;
    spinlock_init(&wq->lock, "wq");
}

void sched_block(wait_queue_t* wq) {
    if (!wq || !current_thread) return;

    cpu_flags_t flags;
    spinlock_acquire(&wq->lock, &flags);
    if (current_thread->state == THREAD_READY || current_thread->state == THREAD_RUNNING) {
        sched_remove_thread(current_thread);
    }
    current_thread->state = THREAD_BLOCKED;
    current_thread->wq_next = wq->waiters;
    wq->waiters = current_thread;
    wq->count++;
    spinlock_release(&wq->lock, flags);

    schedule();
}

void sched_wake(wait_queue_t* wq) {
    if (!wq) return;

    cpu_flags_t flags;
    spinlock_acquire(&wq->lock, &flags);
    while (wq->waiters) {
        thread_t* t = wq->waiters;
        wq->waiters = t->wq_next;
        wq->count--;
        t->wq_next = NULL;

        t->state = THREAD_READY;
        t->priority = t->base_priority;
        t->time_slice_remaining = THREAD_TIME_SLICE;
        t->age_ticks = 0;

        sched_add_thread(t);
    }
    spinlock_release(&wq->lock, flags);
}

void sched_wake_one(wait_queue_t* wq) {
    if (!wq || !wq->waiters) return;

    cpu_flags_t flags;
    spinlock_acquire(&wq->lock, &flags);
    thread_t* t = wq->waiters;
    wq->waiters = t->wq_next;
    wq->count--;
    t->wq_next = NULL;

    t->state = THREAD_READY;
    t->priority = t->base_priority;
    t->time_slice_remaining = THREAD_TIME_SLICE;
    t->age_ticks = 0;

    sched_add_thread(t);
    spinlock_release(&wq->lock, flags);
}

int check_sleepers(void) {
    int woken = 0;
    uint64_t now = hal_timer_get_ticks();

    cpu_flags_t flags;
    if (!spinlock_try_acquire(&all_threads_lock, &flags))
        return 0;  /* Another CPU holds the lock; skip this tick. */

    thread_t* t = all_threads_head;
    while (t) {
        thread_t* next = t->all_next;
        /* Wake on RDTSC deadline (primary) or tick deadline (fallback).
         * RDTSC-based wakeup is essential on KVM SMP where PIT/APIC
         * timer interrupts stop after AP bring-up, freezing timer_ticks. */
        int expired = 0;
        if (t->state == THREAD_SLEEPING && t->wakeup_rdtsc > 0 && rdtsc() >= t->wakeup_rdtsc)
            expired = 1;
        if (t->state == THREAD_SLEEPING && t->wakeup_rdtsc == 0 && t->wakeup_tick > 0 && t->wakeup_tick <= now)
            expired = 1;
        if (expired) {
            /* Wake on the CPU where the thread was last running */
            int target_cpu = t->cpu_queue;
            int this_cpu = smp_cpu_id();
            cpu_flags_t qflags;
            if (!spinlock_try_acquire(&sched_queue_lock, &qflags)) {
                /* Contended: try again next tick */
                t = next;
                continue;
            }
            t->state = THREAD_READY;
            t->priority = t->base_priority;
            t->time_slice_remaining = THREAD_TIME_SLICE;
            t->age_ticks = 0;
            if (target_cpu != this_cpu && target_cpu >= 0 && target_cpu < nr_cpus
                && per_cpu_data[target_cpu]) {
                sched_add_thread_to_cpu_locked(t, target_cpu);
                per_cpu_data[target_cpu]->need_reschedule = 1;
            } else {
                sched_add_thread_to_cpu_locked(t, this_cpu);
                sched_pcp()->need_reschedule = 1;
            }
            spinlock_release(&sched_queue_lock, qflags);
            woken++;
        }
        t = next;
    }
    spinlock_release(&all_threads_lock, flags);
    return woken;
}

/* Called from ISR assembly — checks if the current CPU needs rescheduling */
int sched_isr_check(void) {
    per_cpu_data_t* pcp = sched_pcp();
    if (pcp->need_reschedule) {
        pcp->need_reschedule = 0;
        return 1;
    }
    return 0;
}

static void idle_poll_halt(per_cpu_data_t* pcp) {
    (void)pcp;
    asm volatile("sti; hlt; cli");
}

void idle_thread(void* arg) {
    (void)arg;
    int this_cpu = smp_cpu_id();
    int has_mwait = hal_cpu_has_mwait();
    for (;;) {
        /* Finalize any thread retired on this CPU since the last idle
         * iteration.  Catches threads whose context switch ended in
         * thread_trampoline (brand-new threads) where the schedule()
         * after-switch path is not reached. */
        sched_finalize_retiring();
        /* Check if this CPU has been offlined.  If so, park in a HLT loop
         * until the BSP sets cpu_state back to ONLINE and sends a
         * reschedule IPI to wake us. */
        if (this_cpu != 0 && cpu_state[this_cpu] == CPU_STATE_OFFLINE) {
            while (cpu_state[this_cpu] == CPU_STATE_OFFLINE) {
                /* Report RCU QS even while parked */
                rcu_quiescent_state();
                watchdog_clear_stuck();
                { per_cpu_data_t* pcp_ = sched_pcp(); if (pcp_->need_reschedule) schedule(); }
                { per_cpu_data_t* pcp_ = sched_pcp(); idle_poll_halt(pcp_); }
            }
            /* Wake up from park: the BSP set us ONLINE — fall through to
             * normal idle loop. */
        }

        /* APs now also run check_sleepers and sched_reap_zombies
         * (they use all_threads_lock — a proper spinlock — so
         * concurrent access is safe).  watchdog_flush stays on
         * BSP only (uses raw CLI/STI, not SMP-safe).
         * eventbus_dispatch also stays on BSP (CLI/STI lock). */
        if (check_sleepers()) schedule();
        sched_reap_zombies();
        if (this_cpu == 0) {
            watchdog_flush();
        }
        { per_cpu_data_t* pcp_ = sched_pcp(); if (pcp_->need_reschedule) schedule(); }
        /* Report RCU QS on every idle iteration — idle CPUs are always
         * in a quiescent state even when HLTing. */
        rcu_quiescent_state();
        watchdog_clear_stuck();
        per_cpu_data_t* pcp = sched_pcp();
        (void)has_mwait;
        (void)pcp;
        idle_poll_halt(pcp);
    }
}

uint64_t sched_get_switch_count(void) { return sched_switch_count; }
uint64_t sched_get_yield_count(void) { return sched_yield_count; }

void set_current_thread(thread_t* t) {
    per_cpu_data_t* pcp_st = sched_pcp();
    pcp_st->cpu_thread = t;
}

void sched_timer_tick(void) {
    if (!sched_running || !current_thread) return;

    (void)check_sleepers();

    {
        per_cpu_data_t* _pcp = sched_pcp();
        if (++_pcp->aging_counter >= AGING_INTERVAL) {
            _pcp->aging_counter = 0;
            cpu_flags_t aflags, qflags;
            if (spinlock_try_acquire(&all_threads_lock, &aflags)) {
                if (spinlock_try_acquire(&sched_queue_lock, &qflags)) {
                    thread_t* t = all_threads_head;
                    while (t) {
                        thread_t* next = t->all_next;
                        if (t != idle_thr && t->state == THREAD_READY) {
                            t->age_ticks++;
                            if (t->age_ticks >= AGING_INTERVAL && t->priority < THREAD_MAX_PRIO) {
                                sched_remove_thread_locked(t);
                                t->priority++;
                                sched_add_thread_locked(t);
                                t->age_ticks = 0;
                            }
                        }
                        t = next;
                    }
                    spinlock_release(&sched_queue_lock, qflags);
                }
                spinlock_release(&all_threads_lock, aflags);
            }
        }
    }

    /* Periodic load balancing: push threads to underloaded CPUs */
    {
        per_cpu_data_t* _pcp = sched_pcp();
        if (++_pcp->balance_counter >= 100) {
            _pcp->balance_counter = 0;
            sched_balance_push(_pcp->cpu_id, _pcp);
        }
    }

    per_cpu_data_t* pcp = sched_pcp();
    current_thread->total_ticks++;
    if (current_thread->time_slice_remaining > 0)
        current_thread->time_slice_remaining--;
    pcp->idle_wake_hint = current_thread->total_ticks;
    if (current_thread->time_slice_remaining == 0)
        pcp->need_reschedule = 1;

    /* AP-targeted IPIs (reschedule + NMI watchdog): only fire after ALL
     * expected APs have signaled ready (ap_ready_count).  Sending IPIs
     * or NMIs to an AP before it is fully booted will crash it — the
     * trampoline runs in 16-bit mode with no NMI handler, and even in
     * 64-bit mode the AP's IDT may not be set up yet. */
    if (smp_enabled && smp_cpu_id() == 0 && ap_ready_count >= nr_cpus - 1) {
        /* Periodic reschedule IPI to APs — skip offline CPUs */
        {
            static uint64_t ap_tick_counter = 0;
            if (++ap_tick_counter >= THREAD_TIME_SLICE) {
                ap_tick_counter = 0;
                for (int c = 1; c < nr_cpus; c++) {
                    if (!per_cpu_data[c]) continue;
                    if (cpu_state[c] != CPU_STATE_ONLINE) continue;
                    uint32_t apic_id = cpu_info[c].apic_id;
                    apic_send_ipi(apic_id, IPI_VEC_RESCHEDULE, APIC_ICR_DELIV_FIXED);
                }
            }
        }

        /* NMI watchdog: BSP periodically broadcasts NMI IPIs for lockup detection.
         * Skipped on QEMU (both TCG and KVM) because the APIC ICR broadcast causes
         * stack corruption on TCG (the NMI delivery to APs running on ap_stacks[]
         * during early boot can overflow the trampoline stack, or the ICR write
         * itself triggers a QEMU internal race). */
        if (!hal_is_qemu())
            watchdog_send_nmis_tick();
    }
}

err_t sched_init_ap(void) {
    if (!smp_enabled) return ERR_OK;
    int cpu = smp_cpu_id();

    thread_t* idle = thread_create(idle_thread, NULL, THREAD_IDLE_PRIO, "idle");
    if (!idle) return ERR_NOMEM;
    idle->state = THREAD_READY;

    if (cpu >= 0 && cpu < nr_cpus && per_cpu_data[cpu]) {
        per_cpu_data[cpu]->idle_thread = (void*)idle;
        per_cpu_data[cpu]->cpu_thread = (void*)idle;
    }

    return ERR_OK;
}

err_t sched_init(void) {
    spinlock_init(&all_threads_lock, "all_threads_lock");
    spinlock_init(&sched_queue_lock, "sched_queue_lock");
    asm volatile("mov %%cr3, %0" : "=r"(kernel_cr3));
    kprintf("[SCHED] Kernel CR3 = 0x%llx\n", kernel_cr3);

    idle_thr = thread_create(idle_thread, NULL, THREAD_IDLE_PRIO, "idle");
    if (!idle_thr) return ERR_NOMEM;
    idle_thr->state = THREAD_READY;

    if (per_cpu_data[0])
        per_cpu_data[0]->idle_thread = (void*)idle_thr;

    current_thread = thread_create(NULL, NULL, THREAD_DEF_PRIO, "init");
    if (!current_thread) return ERR_NOMEM;
    current_thread->cpu_affinity = 1;  /* pin init to CPU 0 — prevents work-steal */
    current_thread->state = THREAD_RUNNING;
    set_current_thread(current_thread);

    uint64_t kstack_top = (uint64_t)current_thread->kernel_stack + current_thread->kernel_stack_size;
    hal_set_kernel_stack(kstack_top);

    sched_running = 1;

    kprintf("[SCHED] Layer 2 initialized: %d priority levels\n",
            THREAD_MAX_PRIO + 1);
    return ERR_OK;
}

thread_t* sched_find_thread_by_tid(uint64_t tid) {
    if (!tid) return NULL;
    cpu_flags_t flags;
    spinlock_acquire(&all_threads_lock, &flags);
    thread_t* t = all_threads_head;
    while (t) {
        if (t->id == tid) {
            spinlock_release(&all_threads_lock, flags);
            return t;
        }
        t = t->all_next;
    }
    spinlock_release(&all_threads_lock, flags);
    return NULL;
}

void sched_set_priority(thread_t* t, int priority) {
    if (!t || priority < 0 || priority > THREAD_MAX_PRIO)
        return;
    cpu_flags_t qflags;
    spinlock_acquire(&sched_queue_lock, &qflags);
    if (t->state == THREAD_READY) {
        sched_remove_thread_locked(t);
        t->priority = priority;
        t->base_priority = priority;
        sched_add_thread_locked(t);
    } else {
        t->priority = priority;
        t->base_priority = priority;
    }
    spinlock_release(&sched_queue_lock, qflags);
}

void sched_set_thread_affinity(thread_t* t, uint64_t mask) {
    if (!t) return;
    /* Must allow at least one CPU and only valid bits */
    if (mask == 0) return;
    int ncpus = smp_enabled ? nr_cpus : 1;
    mask &= (1ULL << ncpus) - 1;
    if (mask == 0) return;
    t->cpu_affinity = mask;

    /* If the thread is currently running and no longer allowed on this CPU,
     * set need_reschedule so it gets migrated on the next schedule() call. */
    if (t == current_thread) {
        int this_cpu = smp_cpu_id();
        if (!(mask & (1ULL << this_cpu)))
            sched_pcp()->need_reschedule = 1;
    }
}

thread_t* sched_find_thread(uint64_t id) {
    cpu_flags_t flags;
    spinlock_acquire(&all_threads_lock, &flags);
    thread_t* t = all_threads_head;
    while (t) {
        if (t->id == id) break;
        t = t->all_next;
    }
    spinlock_release(&all_threads_lock, flags);
    return t;
}

int sched_kill_thread(uint64_t id) {
    thread_t* t = sched_find_thread(id);
    if (!t) return -1;
    if (t->state == THREAD_ZOMBIE || t->state == THREAD_TERMINATED)
        return -1;
    cpu_flags_t qflags;
    spinlock_acquire(&sched_queue_lock, &qflags);
    if (t->state == THREAD_READY)
        sched_remove_thread_locked(t);
    t->state = THREAD_ZOMBIE;
    spinlock_release(&sched_queue_lock, qflags);
    /* Wake joiners outside sched_queue_lock to avoid ABBA deadlock
     * with thread_join (which holds join_queue.lock then acquires sched_queue_lock). */
    sched_wake(&t->join_queue);
    return 0;
}

/* Migrate all threads from one CPU's run queue to another.
 * Called from SMP hotplug path (smp_cpu_offline).
 * Must NOT be called with sched_queue_lock already held. */
int sched_migrate_cpu(int from_cpu, int to_cpu) {
    if (from_cpu == to_cpu) return 0;
    if (!per_cpu_data[from_cpu] || !per_cpu_data[to_cpu]) return 0;

    cpu_flags_t qflags;
    spinlock_acquire(&sched_queue_lock, &qflags);

    per_cpu_data_t* src = per_cpu_data[from_cpu];
    per_cpu_data_t* dst = per_cpu_data[to_cpu];
    int migrated = 0;

    for (int prio = 0; prio <= THREAD_MAX_PRIO; prio++) {
        thread_t* t = (thread_t*)src->rq_heads[prio];
        while (t) {
            thread_t* next = t->rq_next;

            /* Dequeue from source */
            if (t->rq_prev) t->rq_prev->rq_next = t->rq_next;
            else src->rq_heads[prio] = (void*)t->rq_next;

            if (t->rq_next) t->rq_next->rq_prev = t->rq_prev;
            else src->rq_tails[prio] = (void*)t->rq_prev;

            t->rq_next = NULL;
            t->rq_prev = NULL;
            src->rq_counts[prio]--;
            src->rq_total--;

            /* Add to destination */
            uint32_t p = t->priority;
            thread_t* tail = (thread_t*)dst->rq_tails[p];
            if (tail) {
                tail->rq_next = t;
            } else {
                dst->rq_heads[p] = (void*)t;
                dst->priority_bitmap[p / 64] |= (1ULL << (p % 64));
            }
            t->rq_prev = tail;
            t->rq_next = NULL;
            dst->rq_tails[p] = (void*)t;
            dst->rq_counts[p]++;
            dst->rq_total++;
            t->cpu_queue = to_cpu;

            migrated++;
            t = next;
        }
        /* Clear this priority level's bitmap on source */
        if (src->rq_heads[prio] == NULL)
            src->priority_bitmap[prio / 64] &= ~(1ULL << (prio % 64));
    }

    spinlock_release(&sched_queue_lock, qflags);
    return migrated;
}

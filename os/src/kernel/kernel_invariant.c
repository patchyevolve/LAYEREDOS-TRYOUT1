#include "kernel_invariant.h"
#include "kernel.h"
#include "sched.h"
#include "smp.h"
#include "pmm.h"
#include "net_ns.h"
#include "sync.h"

#ifndef NDEBUG

/* ------------------------------------------------------------------ */
/* Scheduler: walk run queues, verify rq_counts match linked-list     */
/* ------------------------------------------------------------------ */

static void sched_validate_rq(void) {
    if (!sched_running) return;
    int this_cpu = smp_cpu_id();
    if (this_cpu < 0 || this_cpu >= MAX_CPUS) return;
    per_cpu_data_t* pcp = per_cpu_data[this_cpu];
    if (!pcp) return;

    /* Acquire sched_queue_lock to ensure a consistent view of run queues.
     * Use try_acquire — if contended, another CPU is modifying queues and
     * we'd get a false mismatch; skip instead. */
    cpu_flags_t qflags;
    if (!spinlock_try_acquire(&sched_queue_lock, &qflags))
        return;

    for (int prio = 0; prio < 256; prio++) {
        uint32_t declared = pcp->rq_counts[prio];
        if (declared == 0 && pcp->rq_heads[prio] == NULL) continue;

        uint32_t walked = 0;
        thread_t* t = (thread_t*)pcp->rq_heads[prio];
        while (t) {
            walked++;
            if (walked > declared + 64) break; /* likely cycle */
            if (t->state == THREAD_CREATED) {
                spinlock_release(&sched_queue_lock, qflags);
                kprintf("\n[INVARIANT] rq thread '%s' state=CREATED\n",
                        t->name, t->id);
                kpanic("sched_validate_rq: CREATED thread on run queue");
            }
            t = t->rq_next;
        }
        if (walked > declared + 64) {
            spinlock_release(&sched_queue_lock, qflags);
            kprintf("\n[INVARIANT] rq[%d] likely cycle (walked=%u dec=%u)\n",
                    prio, walked, declared);
            kpanic("sched_validate_rq: cycle in run queue");
        }
        if (walked != declared) {
            spinlock_release(&sched_queue_lock, qflags);
            kprintf("\n[INVARIANT] rq[%d]: declared=%u walked=%u\n",
                    prio, declared, walked);
            kpanic("sched_validate_rq: rq_count mismatch");
        }
    }

    spinlock_release(&sched_queue_lock, qflags);
}

static void sched_validate_all(void) {
    if (!sched_running) return;
    thread_t* cur = current_thread;
    if (!cur) return;
    /* Allow: RUNNING, READY (yield: re-queued but not yet switched),
     * BLOCKED (wq wait), SLEEPING (timer sleep), ZOMBIE (exit in
     * progress).  Reject: CREATED (never started), TERMINATED (reaped). */
    if (cur->state != THREAD_RUNNING && cur->state != THREAD_READY &&
        cur->state != THREAD_BLOCKED && cur->state != THREAD_SLEEPING &&
        cur->state != THREAD_ZOMBIE) {
        kprintf("\n[INVARIANT] current_thread '%s' id=%llu state=%d "
                "(invalid for running thread)\n",
                cur->name, cur->id, cur->state);
        kpanic("sched_validate_all: bad thread state");
    }
}

/* ------------------------------------------------------------------ */
/* PMM: verify per-CPU cache page_owner consistency                   */
/* ------------------------------------------------------------------ */

static void pmm_validate_pages(void) {
    /* Only check the current CPU's cache (hot path, avoid iterating all) */
    int cpu = smp_cpu_id();
    if (cpu < 0 || cpu >= MAX_CPUS) return;
    per_cpu_data_t* pcp = per_cpu_data[cpu];
    if (!pcp) return;
    int count = pcp->pmm_cache_count;
    if (count < 0 || count > 64) {
        kprintf("\n[INVARIANT] CPU %d pmm_cache_count=%d\n", cpu, count);
        kpanic("pmm_validate_pages: invalid cache count");
    }
    for (int i = 0; i < count; i++) {
        uint64_t phys = pcp->pmm_cache[i];
        if (phys == 0 || (phys & 0xFFF) != 0) {
            kprintf("\n[INVARIANT] CPU %d cache[%d]=%llx\n", cpu, i, phys);
            kpanic("pmm_validate_pages: bad cache entry");
        }
    }
}

/* ------------------------------------------------------------------ */
/* Socket: verify fd table integrity                                  */
/* ------------------------------------------------------------------ */

static void sock_validate_table(void) {
    net_ns_t* ns = get_current_ns();
    if (!ns) return;
    cpu_flags_t _sflags;
    /* Acquire sockets_lock for atomic snapshot of the fd table.
     * Use try_acquire — if contended, another CPU is modifying sockets
     * and we'd get a transient false positive; skip instead. */
    if (!spinlock_try_acquire(&ns->sockets_lock, &_sflags))
        return;
    for (int i = 0; i < NET_MAX_SOCKETS; i++) {
        if (!ns->net_sockets[i].used) continue;
        socket_t* s = ns->net_sockets[i].sock;
        if (!s) {
            spinlock_release(&ns->sockets_lock, _sflags);
            kprintf("\n[INVARIANT] socket[%d] used but sock=NULL\n", i);
            kpanic("sock_validate_table: null socket");
        }
        if (s->fd != i) {
            spinlock_release(&ns->sockets_lock, _sflags);
            kprintf("\n[INVARIANT] socket[%d] s->fd=%d\n", i, s->fd);
            kpanic("sock_validate_table: fd mismatch");
        }
        if (s->refcount < 1) {
            spinlock_release(&ns->sockets_lock, _sflags);
            kprintf("\n[INVARIANT] socket[%d] refcount=%d\n", i, s->refcount);
            kpanic("sock_validate_table: zero refcount");
        }
    }
    spinlock_release(&ns->sockets_lock, _sflags);
}

/* ------------------------------------------------------------------ */
/* Top-level: called from schedule() once per context switch          */
/* ------------------------------------------------------------------ */

void kernel_validate_invariants(void) {
    sched_validate_rq();
    sched_validate_all();
    pmm_validate_pages();
    sock_validate_table();
}

#endif /* NDEBUG */

#include "kernel.h"
#include "rcu.h"
#include "sched.h"
#include "smp.h"
#include "sync.h"
#include "hal.h"

/* ============================================================
 * Minimal RCU (Read-Copy-Update) implementation
 *
 * Design:
 *   - A single kthread (`rcu_kthread`) drives grace periods
 *   - call_rcu() enqueues callbacks on a global pending list
 *   - The kthread starts a grace period, saves the pending list,
 *     then waits for all CPUs to report a quiescent state via
 *     rcu_quiescent_state() (called from schedule() and idle loop)
 *   - Once all CPUs have QS >= the current GP number, the saved
 *     callbacks execute and the kthread moves on
 * ============================================================ */

/* Grace-period batch progression */
#define RCU_GP_INTERVAL_MS  50     /* kthread polling interval */
#define RCU_QS_POLL_MS      5      /* QS wait recheck interval */

/* Lock protecting rcu_pending list (may be called from any context) */
static spinlock_t rcu_lock;
static rcu_callback_t* rcu_pending;

/* In-flight GP batch list — saved from pending when GP starts */
static rcu_callback_t* rcu_gp_list;

/* Grace period counter: monotonically increasing.
 * A QS is valid if qs_ctr[cpu] >= gp_ctr. */
static volatile int rcu_gp_ctr;

/* Per-CPU quiescent-state counters */
static volatile int rcu_qs_ctr[MAX_CPUS];

/* Non-zero while a GP is in progress */
static volatile int rcu_gp_active;

/* The RCU kthread */
static thread_t* rcu_thread;

void rcu_quiescent_state(void) {
    int cpu = smp_cpu_id();
    if (cpu >= 0 && cpu < MAX_CPUS) {
        int gp = rcu_gp_ctr;  /* snapshot — QS is valid for current GP */
        rcu_qs_ctr[cpu] = gp;
    }
}

static void rcu_kthread_func(void* arg) {
    (void)arg;
    for (;;) {
        thread_sleep(RCU_GP_INTERVAL_MS);

        cpu_flags_t flags;
        spinlock_acquire(&rcu_lock, &flags);

        int have_work = (rcu_pending != NULL);

        if (have_work) {
            /* Start a new GP: capture pending list */
            rcu_gp_list = rcu_pending;
            rcu_pending = NULL;
            rcu_gp_ctr++;
            rcu_gp_active = 1;
            /* The kthread itself has passed through a QS */
            rcu_qs_ctr[smp_cpu_id()] = rcu_gp_ctr;
        }

        spinlock_release(&rcu_lock, flags);

        if (!have_work)
            continue;

        /* Wait for all CPUs to report QS >= current GP */
        int target_gp = rcu_gp_ctr;
        int all_done;
        do {
            all_done = 1;
            for (int c = 0; c < nr_cpus; c++) {
                if (rcu_qs_ctr[c] < target_gp) {
                    all_done = 0;
                    break;
                }
            }
            if (!all_done)
                thread_sleep(RCU_QS_POLL_MS);
        } while (!all_done);

        /* GP complete — execute saved callbacks */
        rcu_callback_t* cb = rcu_gp_list;
        rcu_gp_list = NULL;
        rcu_gp_active = 0;

        while (cb) {
            rcu_callback_t* next = cb->next;
            cb->func(cb->arg);
            cb = next;
        }
    }
}

void call_rcu(rcu_callback_t* cb, void (*func)(void*), void* arg) {
    if (!cb || !func) return;

    cb->func = func;
    cb->arg = arg;
    cb->next = NULL;

    cpu_flags_t flags;
    spinlock_acquire(&rcu_lock, &flags);

    /* Prepend to pending list */
    cb->next = rcu_pending;
    rcu_pending = cb;

    spinlock_release(&rcu_lock, flags);
}

void rcu_barrier(void) {
    /* Spin until no more pending/in-flight callbacks */
    while (rcu_pending || rcu_gp_active || rcu_gp_list) {
        thread_sleep(RCU_QS_POLL_MS);
        /* Yield to let the RCU kthread run */
        thread_yield();
    }
}

int rcu_cbs_pending(void) {
    return (rcu_pending != NULL) || (rcu_gp_active) || (rcu_gp_list != NULL);
}

void rcu_init(void) {
    spinlock_init(&rcu_lock, "rcu_lock");
    rcu_pending = NULL;
    rcu_gp_list = NULL;
    rcu_gp_ctr = 0;
    rcu_gp_active = 0;

    for (int i = 0; i < MAX_CPUS; i++)
        rcu_qs_ctr[i] = 0;

    rcu_thread = thread_create(rcu_kthread_func, NULL,
                               THREAD_DEF_PRIO, "rcu-gp");
    if (rcu_thread) {
        rcu_thread->cpu_affinity = 1;  /* pin to CPU 0 to avoid migration race */
        sched_add_thread(rcu_thread);
        kprintf("[RCU] Grace-period kthread started\n");
    } else {
        kprintf("[RCU] Failed to create kthread\n");
    }
}

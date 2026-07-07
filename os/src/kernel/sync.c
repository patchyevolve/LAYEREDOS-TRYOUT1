#include "kernel.h"
#include "sync.h"
#include "hal.h"
#include "lockdep.h"

void spinlock_init(spinlock_t* lock, const char* name) {
    lock->lock = 0;
    lock->name = name;
    lock->holder = 0;
}

void spinlock_acquire(spinlock_t* lock, cpu_flags_t* out_flags) {
    cpu_flags_t flags = hal_save_irq();
    if (current_thread && lock->lock && lock->holder == current_thread->id)
        kpanic("RECURSIVE SPINLOCK: '%s' held by thread %x (lock=%p)",
               lock->name ? lock->name : "?", current_thread->id, (void*)lock);
    while (__sync_lock_test_and_set(&lock->lock, 1)) {
        while (lock->lock)
            asm volatile("pause");
    }
    lock->holder = current_thread ? current_thread->id : 0;
    __sync_synchronize();
    lockdep_acquire((void*)lock, lock->name, 0);
    *out_flags = flags;
}

int spinlock_try_acquire(spinlock_t* lock, cpu_flags_t* out_flags) {
    cpu_flags_t flags = hal_save_irq();
    if (__sync_lock_test_and_set(&lock->lock, 1)) {
        hal_restore_irq(flags);
        return 0;
    }
    lock->holder = current_thread ? current_thread->id : 0;
    __sync_synchronize();
    lockdep_acquire((void*)lock, lock->name, 0);
    *out_flags = flags;
    return 1;
}

void spinlock_release(spinlock_t* lock, cpu_flags_t flags) {
    lockdep_release((void*)lock);
    __sync_synchronize();
    lock->holder = 0;
    __sync_lock_release(&lock->lock);
    hal_restore_irq(flags);
}

void mutex_init(mutex_t* m) {
    m->locked = 0;
    m->owner_tid = 0;
    wait_queue_init(&m->wait_queue);
    m->orig_priority = 0;
}

err_t mutex_lock(mutex_t* m, uint64_t timeout_ms) {
    if (!m) return ERR_INVAL;

    uint64_t start_ns = hal_timer_get_ns();
    uint64_t timeout_ns = (timeout_ms == (uint64_t)-1) ? (uint64_t)-1 :
                          timeout_ms * 1000000ULL;

    for (;;) {
        if (__sync_bool_compare_and_swap(&m->locked, 0, 1)) {
            m->owner_tid = current_thread ? current_thread->id : 0;
            if (current_thread) {
                m->orig_priority = current_thread->base_priority;
            }
            return ERR_OK;
        }

        if (timeout_ms == 0) return ERR_BUSY;

        // Priority inheritance: if current thread has higher priority than owner
        if (current_thread) {
            thread_t* owner = sched_find_thread_by_tid(m->owner_tid);
            if (owner && owner->priority > current_thread->priority) {
                // Boost owner's priority
                sched_set_priority(owner, current_thread->priority);
            }
        }

        if (timeout_ns != (uint64_t)-1) {
            uint64_t elapsed = hal_timer_get_ns() - start_ns;
            if (elapsed >= timeout_ns) {
                return ERR_TIMEOUT;
            }
        }

        sched_block(&m->wait_queue);
    }
}

err_t mutex_unlock(mutex_t* m) {
    if (!m) return ERR_INVAL;
    if (current_thread && m->owner_tid != current_thread->id)
        return ERR_PERM;

    m->owner_tid = 0;
    __sync_synchronize();
    m->locked = 0;

    // Restore original priority of current thread
    if (current_thread && current_thread->priority != m->orig_priority) {
        sched_set_priority(current_thread, m->orig_priority);
    }

    sched_wake(&m->wait_queue);

    return ERR_OK;
}

void condvar_init(condvar_t* cv) {
    kmemset(cv, 0, sizeof(condvar_t));
    wait_queue_init(&cv->wait_queue);
}

void condvar_wait(condvar_t* cv, mutex_t* m) {
    mutex_unlock(m);
    sched_block(&cv->wait_queue);
    mutex_lock(m, (uint64_t)-1);
}

void condvar_signal(condvar_t* cv) {
    sched_wake_one(&cv->wait_queue);
}

void condvar_broadcast(condvar_t* cv) {
    sched_wake(&cv->wait_queue);
}

/* ============================================================
 * Read-Write Lock
 * ============================================================ */
void rwlock_init(rwlock_t* rw, const char* name) {
    spinlock_init(&rw->internal, name);
    rw->state = 0;
}

void rwlock_read_acquire(rwlock_t* rw, cpu_flags_t* out_flags) {
    cpu_flags_t flags;
    for (;;) {
        spinlock_acquire(&rw->internal, &flags);
        if (rw->state >= 0) {
            rw->state++;
            spinlock_release(&rw->internal, flags);
            lockdep_acquire((void*)rw, rw->internal.name, 1);
            *out_flags = hal_save_irq();
            return;
        }
        spinlock_release(&rw->internal, flags);
        cpu_relax();
    }
}

void rwlock_read_release(rwlock_t* rw, cpu_flags_t flags) {
    lockdep_release((void*)rw);
    cpu_flags_t tmp;
    spinlock_acquire(&rw->internal, &tmp);
    rw->state--;
    spinlock_release(&rw->internal, tmp);
    hal_restore_irq(flags);
}

void rwlock_write_acquire(rwlock_t* rw, cpu_flags_t* out_flags) {
    cpu_flags_t flags;
    for (;;) {
        spinlock_acquire(&rw->internal, &flags);
        if (rw->state == 0) {
            rw->state = -1;
            spinlock_release(&rw->internal, flags);
            lockdep_acquire((void*)rw, rw->internal.name, 0);
            *out_flags = hal_save_irq();
            return;
        }
        spinlock_release(&rw->internal, flags);
        cpu_relax();
    }
}

void rwlock_write_release(rwlock_t* rw, cpu_flags_t flags) {
    lockdep_release((void*)rw);
    cpu_flags_t tmp;
    spinlock_acquire(&rw->internal, &tmp);
    rw->state = 0;
    spinlock_release(&rw->internal, tmp);
    hal_restore_irq(flags);
}

/* ============================================================
 * Sequence Lock
 * ============================================================ */
void seqlock_init(seqlock_t* sql, const char* name) {
    sql->sequence = 0;
    spinlock_init(&sql->lock, name);
}

uint64_t seqlock_read_begin(seqlock_t* sql) {
    uint64_t seq;
    for (;;) {
        seq = sql->sequence;
        if (!(seq & 1))
            break;
        cpu_relax();
    }
    smp_rmb();
    return seq;
}

int seqlock_read_retry(seqlock_t* sql, uint64_t start) {
    smp_rmb();
    return sql->sequence != start;
}

void seqlock_write_acquire(seqlock_t* sql, cpu_flags_t* out_flags) {
    spinlock_acquire(&sql->lock, out_flags);
    sql->sequence++;
    smp_wmb();
    lockdep_acquire((void*)sql, sql->lock.name, 0);
}

void seqlock_write_release(seqlock_t* sql, cpu_flags_t flags) {
    lockdep_release((void*)sql);
    sql->sequence++;
    smp_wmb();
    spinlock_release(&sql->lock, flags);
}

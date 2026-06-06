#include "kernel.h"
#include "sync.h"
#include "hal.h"

void spinlock_init(spinlock_t* lock, const char* name) {
    lock->lock = 0;
    lock->name = name;
    lock->holder = 0;
}

void spinlock_acquire(spinlock_t* lock) {
    lock->saved_flags = hal_save_irq();
    while (__sync_lock_test_and_set(&lock->lock, 1)) {
        while (lock->lock)
            asm volatile("pause");
    }
    lock->holder = current_thread ? current_thread->id : 0;
    __sync_synchronize();
}

void spinlock_release(spinlock_t* lock) {
    __sync_synchronize();
    lock->holder = 0;
    __sync_lock_release(&lock->lock);
    hal_restore_irq(lock->saved_flags);
}

void mutex_init(mutex_t* m) {
    m->locked = 0;
    m->owner_tid = 0;
    m->wait_queue.waiters = NULL;
    m->wait_queue.count = 0;
    m->orig_priority = 0;
}

err_t mutex_lock(mutex_t* m, uint64_t timeout_ms) {
    if (!m) return ERR_INVAL;

    uint64_t start_ticks = hal_timer_get_ticks();
    uint64_t timeout_ticks = (timeout_ms == (uint64_t)-1) ? (uint64_t)-1 :
                            (timeout_ms * hal_timer_get_hz() + 999) / 1000;

    for (;;) {
        if (__sync_bool_compare_and_swap(&m->locked, 0, 1)) {
            m->owner_tid = current_thread ? current_thread->id : 0;
            if (current_thread) {
                m->orig_priority = current_thread->priority;
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

        if (timeout_ticks != (uint64_t)-1) {
            uint64_t elapsed = hal_timer_get_ticks() - start_ticks;
            if (elapsed >= timeout_ticks) {
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
    cv->wait_queue.waiters = NULL;
    cv->wait_queue.count = 0;
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

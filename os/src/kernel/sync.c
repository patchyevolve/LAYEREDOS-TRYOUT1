#include "kernel.h"
#include "sync.h"
#include "hal.h"

void spinlock_init(spinlock_t* lock, const char* name) {
    lock->lock = 0;
    lock->name = name;
    lock->holder = 0;
}

void spinlock_acquire(spinlock_t* lock) {
    for (;;) {
        while (__sync_lock_test_and_set(&lock->lock, 1)) {
            while (lock->lock) {
                asm volatile("pause");
            }
        }
        lock->holder = current_thread ? current_thread->id : 0;
        asm volatile("" ::: "memory");
        return;
    }
}

void spinlock_release(spinlock_t* lock) {
    asm volatile("" ::: "memory");
    lock->holder = 0;
    __sync_lock_release(&lock->lock);
}

int spinlock_try_acquire(spinlock_t* lock) {
    if (__sync_lock_test_and_set(&lock->lock, 1) == 0) {
        lock->holder = current_thread ? current_thread->id : 0;
        asm volatile("" ::: "memory");
        return 1;
    }
    return 0;
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

    for (;;) {
        if (__sync_bool_compare_and_swap(&m->locked, 0, 1)) {
            m->owner_tid = current_thread ? current_thread->id : 0;
            if (current_thread) {
                m->orig_priority = current_thread->priority;
            }
            return ERR_OK;
        }

        if (timeout_ms == 0) return ERR_BUSY;

        sched_block(&m->wait_queue);

        if (timeout_ms != (uint64_t)-1) {
            if (timeout_ms < 10) return ERR_TIMEOUT;
            timeout_ms -= 10;
        }
    }
}

err_t mutex_unlock(mutex_t* m) {
    if (!m) return ERR_INVAL;

    m->owner_tid = 0;
    asm volatile("" ::: "memory");
    m->locked = 0;

    sched_wake(&m->wait_queue);

    return ERR_OK;
}

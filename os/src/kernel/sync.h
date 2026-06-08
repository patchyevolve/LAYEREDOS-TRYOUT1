#ifndef SYNC_H
#define SYNC_H

#include "types.h"
#include "sched.h"

typedef struct spinlock {
    volatile uint64_t lock;
    const char*       name;
    uint64_t          holder;
} spinlock_t;

typedef struct mutex {
    volatile int      locked;
    uint64_t          owner_tid;
    wait_queue_t      wait_queue;
    int               orig_priority;
} mutex_t;

typedef struct condvar {
    wait_queue_t      wait_queue;
} condvar_t;

void spinlock_init(spinlock_t* lock, const char* name);
void spinlock_acquire(spinlock_t* lock, cpu_flags_t* out_flags);
void spinlock_release(spinlock_t* lock, cpu_flags_t flags);

void mutex_init(mutex_t* m);
err_t mutex_lock(mutex_t* m, uint64_t timeout_ms);
err_t mutex_unlock(mutex_t* m);

void condvar_init(condvar_t* cv);
void condvar_wait(condvar_t* cv, mutex_t* m);
void condvar_signal(condvar_t* cv);
void condvar_broadcast(condvar_t* cv);

#endif

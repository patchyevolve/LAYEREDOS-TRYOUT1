#ifndef SYNC_H
#define SYNC_H

#include "types.h"
#include "sched.h"
#include "barrier.h"

/* ============================================================
 * Spinlock (struct defined in <types.h>)
 * ============================================================ */
void spinlock_init(spinlock_t* lock, const char* name);
void spinlock_acquire(spinlock_t* lock, cpu_flags_t* out_flags);
int  spinlock_try_acquire(spinlock_t* lock, cpu_flags_t* out_flags);
void spinlock_release(spinlock_t* lock, cpu_flags_t flags);

/* ============================================================
 * Read-Write Lock (rwlock)
 *
 * Allows multiple concurrent readers XOR one exclusive writer.
 * Both reader and writer sides disable interrupts.
 * ============================================================ */
typedef struct rwlock {
    spinlock_t        internal;     /* guards state transitions */
    volatile int      state;        /* -1: writer, 0: free, >0: readers */
} rwlock_t;

void rwlock_init(rwlock_t* rw, const char* name);
void rwlock_read_acquire(rwlock_t* rw, cpu_flags_t* out_flags);
void rwlock_read_release(rwlock_t* rw, cpu_flags_t flags);
void rwlock_write_acquire(rwlock_t* rw, cpu_flags_t* out_flags);
void rwlock_write_release(rwlock_t* rw, cpu_flags_t flags);

/* ============================================================
 * Sequence Lock (seqlock)
 *
 * Optimistic lock for frequently-read, rarely-written data.
 * Readers never block writers — they retry if the sequence
 * changed while they were reading. Writer acquires an embedded
 * spinlock for mutual exclusion.
 * ============================================================ */
typedef struct seqlock {
    volatile uint64_t sequence;     /* even = idle, odd = write in progress */
    spinlock_t        lock;         /* writer exclusion */
} seqlock_t;

void seqlock_init(seqlock_t* sql, const char* name);
uint64_t seqlock_read_begin(seqlock_t* sql);
int  seqlock_read_retry(seqlock_t* sql, uint64_t start);
void seqlock_write_acquire(seqlock_t* sql, cpu_flags_t* out_flags);
void seqlock_write_release(seqlock_t* sql, cpu_flags_t flags);

/* ============================================================
 * Mutex / Condvar
 * ============================================================ */
typedef struct mutex {
    volatile int      locked;
    uint64_t          owner_tid;
    wait_queue_t      wait_queue;
    int               orig_priority;
} mutex_t;

typedef struct condvar {
    wait_queue_t      wait_queue;
} condvar_t;

void mutex_init(mutex_t* m);
err_t mutex_lock(mutex_t* m, uint64_t timeout_ms);
err_t mutex_unlock(mutex_t* m);

void condvar_init(condvar_t* cv);
void condvar_wait(condvar_t* cv, mutex_t* m);
void condvar_signal(condvar_t* cv);
void condvar_broadcast(condvar_t* cv);

#endif

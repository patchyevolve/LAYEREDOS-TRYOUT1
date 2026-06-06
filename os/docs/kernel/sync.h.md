# sync.h — Synchronisation Primitives Interface

**Path:** `os/src/kernel/sync.h`  
**Layer:** Layer 2 (Scheduler) — header

---

## Purpose

Declares the spinlock, mutex, and condvar types and their APIs.  Included
by any file that needs concurrent access control: the event bus, watchdog,
shell, and HAL UART RX path.

---

## Type definitions

All three types are defined here (not in `sync.c`) so callers can embed
them in their own structs without dynamic allocation:

```c
// Spinlock — embed in any struct that needs interrupt-safe locking
typedef struct spinlock {
    volatile uint64_t lock;
    const char*       name;
    uint64_t          holder;
    cpu_flags_t       saved_flags;
} spinlock_t;

// Mutex — sleeping lock; do not use in interrupt context
typedef struct mutex {
    volatile int  locked;
    uint64_t      owner_tid;
    wait_queue_t  wait_queue;
    int           orig_priority;
} mutex_t;

// Condition variable — always used with a mutex
typedef struct condvar {
    wait_queue_t wait_queue;
} condvar_t;
```

---

## API reference

### Spinlock

```c
void spinlock_init(spinlock_t* lock, const char* name);
void spinlock_acquire(spinlock_t* lock);   // cli + spin + barrier
void spinlock_release(spinlock_t* lock);   // barrier + unlock + restore irq
```

**Rule:** spinlocks must never be held across a call to `schedule()`,
`thread_yield()`, `sched_block()`, or any function that may sleep.

### Mutex

```c
void  mutex_init(mutex_t* m);
err_t mutex_lock(mutex_t* m, uint64_t timeout_ms);
  // timeout_ms = 0       → non-blocking (returns ERR_BUSY if held)
  // timeout_ms = UINT64_MAX → block indefinitely
err_t mutex_unlock(mutex_t* m);
  // returns ERR_PERM if caller is not the owner
```

**Rule:** mutexes must not be used from interrupt context (they call
`sched_block` which requires a thread context).

### Condvar

```c
void condvar_init(condvar_t* cv);
void condvar_wait(condvar_t* cv, mutex_t* m);    // release m, sleep, re-acquire m
void condvar_signal(condvar_t* cv);              // wake one waiter
void condvar_broadcast(condvar_t* cv);           // wake all waiters
```

**Pattern:**
```c
mutex_lock(&m, -1);
while (!condition_met)
    condvar_wait(&cv, &m);
// critical section with condition guaranteed
mutex_unlock(&m);
```

---

## Choosing the right primitive

| Situation | Use |
|-----------|-----|
| Protecting a few instructions in ISR context | `spinlock` |
| Protecting data accessed from both ISR and thread | `spinlock` |
| Long critical section (e.g., I/O) from thread only | `mutex` |
| Waiting for an event / condition | `condvar` + `mutex` |
| Waiting for thread completion | `thread_join` + `join_queue` |

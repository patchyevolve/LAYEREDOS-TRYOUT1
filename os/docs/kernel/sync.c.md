# sync.c — Spinlock, Mutex, Condition Variable

**Path:** `os/src/kernel/sync.c`  
**Layer:** Layer 2 (Scheduler) — synchronisation primitives

---

## Purpose

Implements the three synchronisation primitives used throughout the kernel:
spinlocks (interrupt-safe, for short critical sections), mutexes (sleeping,
for longer sections), and condition variables (for producer/consumer
patterns).

---

## Spinlock

### Design

```c
typedef struct spinlock {
    volatile uint64_t lock;        // 0 = free, 1 = held
    const char*       name;        // for diagnostics
    uint64_t          holder;      // thread ID of current holder (or 0)
    cpu_flags_t       saved_flags; // RFLAGS saved by acquire
} spinlock_t;
```

### `spinlock_acquire`

```c
lock->saved_flags = hal_save_irq();   // 1. disable interrupts, save flags
while (__sync_lock_test_and_set(&lock->lock, 1))  // 2. atomic TAS
    while (lock->lock) asm volatile("pause");      // 3. spin with pause
lock->holder = current_thread->id;
__sync_synchronize();                              // 4. full memory barrier
```

**Why disable interrupts?**  
On a uniprocessor system without interrupt disabling, a thread holding a
spinlock can be preempted by the timer ISR.  If the ISR (or a function it
calls) tries to acquire the same spinlock, it will spin forever — deadlock.
Disabling interrupts prevents this: the thread holding the spinlock cannot
be preempted, so it will release the lock before any other code runs.

### `spinlock_release`

```c
__sync_synchronize();           // memory barrier before unlock
lock->holder = 0;
__sync_lock_release(&lock->lock);   // atomic store 0
hal_restore_irq(lock->saved_flags); // restore interrupt state
```

Releasing the lock restores interrupts to the state they were in when
`spinlock_acquire` was called.  If interrupts were disabled before the
acquire, they remain disabled.  This makes spinlocks safe to nest with
`hal_save_irq` / `hal_restore_irq`.

---

## Mutex

### Design

```c
typedef struct mutex {
    volatile int locked;       // 0 = free, 1 = held
    uint64_t     owner_tid;    // thread ID of lock holder
    wait_queue_t wait_queue;   // threads waiting to acquire
    int          orig_priority; // for priority inheritance restore
} mutex_t;
```

Unlike a spinlock, a mutex puts the calling thread to sleep when contended.

### `mutex_lock(m, timeout_ms)`

Tries `__sync_bool_compare_and_swap(&m->locked, 0, 1)`.  On success,
records `owner_tid` and saves `orig_priority`.

On failure:
1. If `timeout_ms == 0`, returns `ERR_BUSY` immediately.
2. **Priority inheritance:** if the current thread has higher priority
   than the owner, boosts the owner's priority to avoid priority inversion.
3. Checks timeout, returns `ERR_TIMEOUT` if exceeded.
4. Calls `sched_block(&m->wait_queue)` to sleep until woken.

### `mutex_unlock(m)`

Ownership check: if `current_thread->id != m->owner_tid`, returns
`ERR_PERM` (audit fix C21).

Clears `locked`, restores the owner's original priority (undo inheritance),
then calls `sched_wake(&m->wait_queue)` to wake all waiters.  Waiters
race to re-acquire using CAS; all but one will go back to sleep.

---

## Condition Variable

### Design

```c
typedef struct condvar {
    wait_queue_t wait_queue;
} condvar_t;
```

### `condvar_wait(cv, m)`

```c
mutex_unlock(m);
sched_block(&cv->wait_queue);
mutex_lock(m, (uint64_t)-1);
```

Atomically releases the mutex and sleeps.  The unlock-then-block sequence
has a window: another thread could signal the condvar between `mutex_unlock`
and `sched_block`.  The signal would be lost.  This is a **known limitation**
(not POSIX-correct) — fixing it requires a more complex "prepare to wait"
mechanism that this kernel does not yet implement.

### `condvar_signal(cv)`

Calls `sched_wake_one(&cv->wait_queue)` — wakes exactly one waiter
(FIFO from the head of the wait queue).

### `condvar_broadcast(cv)`

Calls `sched_wake(&cv->wait_queue)` — wakes all waiters.

---

## Known issues (from AUDIT.md)

| Issue | Status |
|-------|--------|
| C13 — spinlock doesn't disable interrupts | Fixed: `hal_save_irq` in acquire |
| C21 — mutex_unlock has no ownership check | Fixed |
| condvar_wait signal-loss window | Known, not yet fixed |

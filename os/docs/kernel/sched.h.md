# sched.h — Scheduler Public Interface

**Path:** `os/src/kernel/sched.h`  
**Layer:** Layer 2 (Scheduler) — header

---

## Purpose

Declares all types, constants, and functions related to threads and
scheduling.  Included by every file that creates threads, yields, blocks,
or inspects thread state.

---

## Constants

| Constant | Value | Meaning |
|----------|-------|---------|
| `THREAD_NAME_MAX` | 64 | Max chars in `thread_t.name` including null terminator |
| `THREAD_STACK_SIZE` | 16384 | Kernel stack size in bytes (4 pages) |
| `THREAD_IDLE_PRIO` | 0 | Priority of the idle thread |
| `THREAD_DEF_PRIO` | 128 | Default priority for new threads |
| `THREAD_MAX_PRIO` | 255 | Highest valid priority |
| `THREAD_TIME_SLICE` | 10 | Timer ticks per time slice |

---

## `thread_state_t`

```c
typedef enum {
    THREAD_CREATED   = 0,  // just allocated, not yet scheduled
    THREAD_READY,          // in a run queue, waiting for CPU
    THREAD_RUNNING,        // currently executing on CPU
    THREAD_BLOCKED,        // waiting on a wait_queue (I/O, mutex, join)
    THREAD_SLEEPING,       // waiting for a timer tick
    THREAD_ZOMBIE,         // exited, waiting for join/reap
    THREAD_TERMINATED      // fully reaped (used as a sentinel)
} thread_state_t;
```

State machine:
```
CREATED → READY (sched_add_thread)
READY   → RUNNING (schedule picks it)
RUNNING → READY (preemption/yield)
RUNNING → BLOCKED (sched_block)
RUNNING → SLEEPING (thread_sleep)
RUNNING → ZOMBIE (thread_exit)
BLOCKED → READY (sched_wake)
SLEEPING → READY (check_sleepers)
ZOMBIE  → TERMINATED (sched_reap_zombies)
```

---

## `wait_queue_t`

```c
typedef struct wait_queue {
    thread_t* waiters;  // singly-linked via thread_t.wq_next
    uint32_t  count;
    uint32_t  pad;
} wait_queue_t;
```

Used by mutexes, condvars, `thread_join`, and UART RX.  Each thread has
exactly one `wq_next` pointer, so a thread can only be on one wait queue
at a time.

---

## `thread_t` — Thread Control Block

See `sched.c.md` for full field documentation.  The two most important
fields for the assembly context switch:

- `rsp` at **offset 8** (after `id`) — saved kernel stack pointer
- `cr3` at **offset 16** — page table base (currently unused / always 0)

The assembly in `ctx.S` must access `rsp` at the correct offset.

---

## Global variables

```c
extern thread_t*     current_thread;   // currently running thread (or NULL before sched_init)
extern volatile int  sched_running;    // 1 after sched_init
extern volatile int  need_reschedule;  // set by timer ISR to trigger schedule()
```

`need_reschedule` is read by `isr_common_handler` after every interrupt.
It is declared `volatile` so the compiler does not optimise away the read.

---

## Key function signatures

```c
err_t     sched_init(void);
thread_t* thread_create(void (*func)(void*), void* arg, int prio, const char* name);
thread_t* thread_create_user(const char* name);
void      sched_add_thread(thread_t* t);
void      sched_remove_thread(thread_t* t);
void      schedule(void);
void      thread_yield(void);
void      thread_exit(int exit_code);
void      thread_sleep(uint64_t ms);
err_t     thread_join(thread_t* t, int* exit_code);
void      sched_block(wait_queue_t* wq);
void      sched_wake(wait_queue_t* wq);
void      sched_foreach(void (*cb)(thread_t*, void*), void*);
void      sched_timer_tick(void);
void      sched_reap_zombies(void);
uint32_t  sched_thread_count(void);
```

Assembly back-ends (defined in `ctx.S`):
```c
void switch_context(thread_t** current, thread_t** next);
void thread_trampoline(void);
void user_thread_entry(void);
```

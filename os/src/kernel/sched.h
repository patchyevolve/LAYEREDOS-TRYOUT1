#ifndef SCHED_H
#define SCHED_H

#include "types.h"
#ifdef CONFIG_SMP
#include "smp.h"
#endif

#define THREAD_NAME_MAX   64
#define THREAD_STACK_SIZE 32768
#define THREAD_IDLE_PRIO  0
#define THREAD_DEF_PRIO   128
#define THREAD_MAX_PRIO   255
#define THREAD_TIME_SLICE 10
#define AGING_INTERVAL    20  /* boost ready thread every 20 ticks if starved (~400ms per boost) */

typedef enum {
    THREAD_CREATED = 0,
    THREAD_READY,
    THREAD_RUNNING,
    THREAD_BLOCKED,
    THREAD_SLEEPING,
    THREAD_ZOMBIE,
    THREAD_TERMINATED
} thread_state_t;

typedef struct thread thread_t;

typedef struct wait_queue {
    thread_t*      waiters;
    uint32_t       count;
    spinlock_t     lock;
} wait_queue_t;

void wait_queue_init(wait_queue_t* wq);

typedef struct thread {
    uint64_t            rsp;   /* must be first field: accessed by switch_context (offset 0) */
    uint64_t            id;
    uint64_t            cr3;
    thread_state_t      state;
    int                 priority;
    int                 base_priority;
    volatile uint64_t   time_slice_remaining;
    uint64_t            total_ticks;
    uint64_t            age_ticks;
    uint64_t            wakeup_tick;
    thread_t*           rq_next;
    thread_t*           rq_prev;
    thread_t*           wq_next;
    thread_t*           all_next;
    thread_t*           all_prev;
    wait_queue_t        join_queue;
    char                name[THREAD_NAME_MAX];
    void*               kernel_stack;
    uint64_t            kernel_stack_size;
    int                 exit_code;
    uint64_t            user_code_page;
    uint64_t            user_stack_page;
    struct list_head    threads_node; /* for process thread list */
    struct process_t*   proc;         /* owning process */
    uint8_t             cpu_queue;    /* which CPU's run queue this thread is on */
    uint64_t            cpu_affinity; /* bitmask of allowed CPUs (bit 0 = CPU 0) */
    uint64_t            block_phys;   /* physical base of TCB+guard+stack block */
#ifdef CONFIG_LOCKDEP
    void*               held_locks[8];       /* lock addresses held by this thread */
    const char*         held_names[8];       /* lock names */
    int                 held_modes[8];       /* 0=exclusive, 1=read */
    int                 held_depth;
#endif
} thread_t;

#ifdef CONFIG_LOCKDEP
/* Convenience macros to access lockdep per-thread data */
#define LOCKDEP_HELD(t)      ((t)->held_locks)
#define LOCKDEP_NAMES(t)     ((t)->held_names)
#define LOCKDEP_MODES(t)     ((t)->held_modes)
#define LOCKDEP_DEPTH(t)     ((t)->held_depth)
#endif

extern volatile int sched_running;

#ifdef CONFIG_SMP
/*
 * Per-CPU current_thread: reads/writes resolve to this CPU's per-CPU slot.
 * The macro is both an lvalue (current_thread = X) and addressable (&current_thread
 * works because &(*ptr) collapses to ptr).
 */
/* Per-CPU current_thread: when SMP is active, each CPU has its own slot in
 * per_cpu_data[].  The global symbol `current_thread_global` exists as a
 * fallback during early boot (before smp_init allocates per-CPU data) and
 * for UP builds.  All code in the tree references `current_thread` — which
 * is redirected via macro to the correct per-CPU slot after smp_init. */
extern thread_t* current_thread_global;
static inline thread_t** __current_thread_ptr(void) {
    int __cpu = smp_cpu_id();
    if (per_cpu_data[__cpu])
        return (thread_t**)&per_cpu_data[__cpu]->cpu_thread;
    return &current_thread_global;
}
#define current_thread (*__current_thread_ptr())
#else
extern thread_t* current_thread;
#endif

void set_current_thread(thread_t* t);
err_t sched_init_ap(void);
void sched_foreach(void (*cb)(thread_t* t, void* ctx), void* ctx);
void sched_timer_tick(void);
uint32_t sched_thread_count(void);
void sched_reap_zombies(void);

err_t  sched_init(void);
thread_t* thread_create(void (*func)(void*), void* arg,
                        int priority, const char* name);
void  thread_yield(void);
void  thread_exit(int exit_code);
void  thread_sleep(uint64_t ms);
err_t  thread_join(thread_t* t, int* exit_code);
void  sched_add_thread(thread_t* t);
void  all_threads_add(thread_t* t);
void  sched_set_kernel_cr3(uint64_t cr3);
void  sched_remove_thread(thread_t* t);
void  sched_block(wait_queue_t* wq);
void  sched_wake(wait_queue_t* wq);
void  sched_wake_one(wait_queue_t* wq);
void  schedule(void);
uint64_t sched_get_switch_count(void);
uint64_t sched_get_yield_count(void);
void  idle_thread(void* arg);
int   sched_isr_check(void);
int   check_sleepers(void);

void switch_context(thread_t** current, thread_t** next);
void thread_trampoline(void);
void user_thread_entry(void);
void fork_child_entry(void);
thread_t* sched_find_thread_by_tid(uint64_t tid);
void  sched_set_priority(thread_t* t, int priority);
thread_t* sched_find_thread(uint64_t id);
int   sched_kill_thread(uint64_t id);
void  sched_set_thread_affinity(thread_t* t, uint64_t mask);
void  sched_place_thread(thread_t* t, int cpu);
int   sched_migrate_cpu(int from_cpu, int to_cpu);
extern uint64_t next_thread_id;
extern spinlock_t sched_queue_lock;

#endif

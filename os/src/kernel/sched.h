#ifndef SCHED_H
#define SCHED_H

#include "types.h"

#define THREAD_NAME_MAX   64
#define THREAD_STACK_SIZE 16384
#define THREAD_IDLE_PRIO  0
#define THREAD_DEF_PRIO   128
#define THREAD_MAX_PRIO   255
#define THREAD_TIME_SLICE 10
#define AGING_INTERVAL    50  /* boost ready thread every 50 ticks if starved */

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
    thread_t* waiters;
    uint32_t  count;
    uint32_t  pad;
} wait_queue_t;

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
} thread_t;

typedef struct {
    thread_t* head;
    thread_t* tail;
    uint32_t  count;
} run_queue_t;

extern thread_t* current_thread;
extern volatile int sched_running;
extern volatile int need_reschedule;
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

void switch_context(thread_t** current, thread_t** next);
void thread_trampoline(void);
void user_thread_entry(void);
void fork_child_entry(void);
thread_t* sched_find_thread_by_tid(uint64_t tid);
void  sched_set_priority(thread_t* t, int priority);
thread_t* sched_find_thread(uint64_t id);
int   sched_kill_thread(uint64_t id);

#endif

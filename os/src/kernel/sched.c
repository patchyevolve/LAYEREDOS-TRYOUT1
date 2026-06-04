#include "kernel.h"
#include "sched.h"
#include "pmm.h"
#include "hal.h"
#include "eventbus.h"

static run_queue_t run_queues[THREAD_MAX_PRIO + 1];
static uint64_t priority_bitmap[4];
static uint64_t next_thread_id = 1;
static thread_t* idle_thr = NULL;
static thread_t* all_threads_head = NULL;
static thread_t* all_threads_tail = NULL;
static uint32_t all_threads_count = 0;
thread_t* current_thread = NULL;
volatile int sched_running = 0;
volatile int need_reschedule = 0;

static void all_threads_add(thread_t* t) {
    cpu_flags_t flags = hal_save_irq();
    t->all_next = NULL;
    t->all_prev = all_threads_tail;
    if (all_threads_tail)
        all_threads_tail->all_next = t;
    else
        all_threads_head = t;
    all_threads_tail = t;
    all_threads_count++;
    hal_restore_irq(flags);
}

static void all_threads_remove(thread_t* t) {
    cpu_flags_t flags = hal_save_irq();
    if (t->all_prev)
        t->all_prev->all_next = t->all_next;
    else
        all_threads_head = t->all_next;
    if (t->all_next)
        t->all_next->all_prev = t->all_prev;
    else
        all_threads_tail = t->all_prev;
    t->all_next = NULL;
    t->all_prev = NULL;
    all_threads_count--;
    hal_restore_irq(flags);
}

void sched_foreach(void (*cb)(thread_t* t, void* ctx), void* ctx) {
    if (!cb) return;
    thread_t* t = all_threads_head;
    while (t) {
        cb(t, ctx);
        t = t->all_next;
    }
}

uint32_t sched_thread_count(void) {
    return all_threads_count;
}

void sched_reap_zombies(void) {
    thread_t* t = all_threads_head;
    while (t) {
        thread_t* next = t->all_next;
        if (t->state == THREAD_ZOMBIE || t->state == THREAD_TERMINATED) {
            if (t == idle_thr || t == current_thread) {
                t = next;
                continue;
            }
            all_threads_remove(t);
            if (t->kernel_stack)
                pmm_free_pages((uint64_t)t->kernel_stack,
                    (t->kernel_stack_size + PAGE_SIZE - 1) / PAGE_SIZE);
            pmm_free_page((uint64_t)t);
        }
        t = next;
    }
}

static void bitmap_set_prio(int prio) {
    priority_bitmap[prio / 64] |= (1ULL << (prio % 64));
}

static void bitmap_clear_prio(int prio) {
    priority_bitmap[prio / 64] &= ~(1ULL << (prio % 64));
}

static int bitmap_find_highest(void) {
    for (int i = 3; i >= 0; i--) {
        if (priority_bitmap[i]) {
            int bit = 63 - __builtin_clzll(priority_bitmap[i]);
            return i * 64 + bit;
        }
    }
    return -1;
}

void sched_add_thread(thread_t* t) {
    if (!t || t->priority < 0 || t->priority > THREAD_MAX_PRIO) return;

    cpu_flags_t flags = hal_save_irq();
    run_queue_t* q = &run_queues[t->priority];

    if (q->tail) {
        q->tail->next = t;
    } else {
        q->head = t;
        bitmap_set_prio(t->priority);
    }
    t->prev = q->tail;
    t->next = NULL;
    q->tail = t;
    q->count++;
    t->state = THREAD_READY;

    hal_restore_irq(flags);
}

void sched_remove_thread(thread_t* t) {
    if (!t) return;
    cpu_flags_t flags = hal_save_irq();
    run_queue_t* q = &run_queues[t->priority];

    if (t->prev) t->prev->next = t->next;
    else q->head = t->next;

    if (t->next) t->next->prev = t->prev;
    else q->tail = t->prev;

    q->count--;

    if (q->count == 0) bitmap_clear_prio(t->priority);

    t->next = NULL;
    t->prev = NULL;
    hal_restore_irq(flags);
}

static thread_t* pick_next(void) {
    int prio = bitmap_find_highest();
    if (prio < 0) return idle_thr;

    run_queue_t* q = &run_queues[prio];
    if (!q->head) return idle_thr;

    thread_t* t = q->head;
    if (t->next) {
        q->head = t->next;
        q->head->prev = NULL;
    } else {
        q->head = NULL;
        q->tail = NULL;
    }
    q->count--;
    if (q->count == 0) bitmap_clear_prio(prio);
    t->next = NULL;
    t->prev = NULL;

    return t;
}

void schedule(void) {
    if (!sched_running || !current_thread) return;

    need_reschedule = 0;
    cpu_flags_t flags = hal_save_irq();

    thread_t* next = pick_next();

    if (next == idle_thr && current_thread != idle_thr) {
        hal_restore_irq(flags);
        return;
    }

    if (next == current_thread) {
        hal_restore_irq(flags);
        return;
    }

    if (current_thread->state == THREAD_RUNNING) {
        current_thread->state = THREAD_READY;
        sched_add_thread(current_thread);
    }

    thread_t* old = current_thread;
    current_thread = next;
    next->state = THREAD_RUNNING;
    next->time_slice_remaining = THREAD_TIME_SLICE;

    hal_restore_irq(flags);
    switch_context(&old, &current_thread);
}

void sched_tick(void) {
    if (!current_thread || current_thread == idle_thr) return;

    current_thread->total_ticks++;
    if (current_thread->time_slice_remaining > 0) {
        current_thread->time_slice_remaining--;
    }

    if (current_thread->time_slice_remaining == 0) {
        need_reschedule = 1;
    }
}

void thread_yield(void) {
    current_thread->time_slice_remaining = 0;
    schedule();
}

thread_t* thread_create(void (*func)(void*), void* arg,
                        int priority, const char* name) {
    thread_t* tcb = (thread_t*)pmm_alloc_page();
    if (!tcb) return NULL;
    kmemset(tcb, 0, sizeof(thread_t));

    void* kstack = (void*)pmm_alloc_pages(
        (THREAD_STACK_SIZE + PAGE_SIZE - 1) / PAGE_SIZE);
    if (!kstack) {
        pmm_free_page((uint64_t)tcb);
        return NULL;
    }

    uint64_t kstack_top = (uint64_t)kstack + THREAD_STACK_SIZE;
    kmemset(kstack, 0, THREAD_STACK_SIZE);

    uint64_t* sp = (uint64_t*)kstack_top;

    *(--sp) = (uint64_t)thread_trampoline;
    *(--sp) = (uint64_t)func;
    *(--sp) = (uint64_t)arg;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;

    tcb->id = next_thread_id++;
    tcb->rsp = (uint64_t)sp;
    tcb->cr3 = 0;
    tcb->state = THREAD_CREATED;
    tcb->priority = (priority > THREAD_MAX_PRIO) ? THREAD_DEF_PRIO : priority;
    tcb->time_slice_remaining = 0;
    tcb->kernel_stack = kstack;
    tcb->kernel_stack_size = THREAD_STACK_SIZE;
    kstrncpy(tcb->name, name ? name : "thread", THREAD_NAME_MAX - 1);

    all_threads_add(tcb);

    return tcb;
}

void thread_exit(int exit_code) {
    if (!current_thread) return;

    current_thread->exit_code = exit_code;
    current_thread->state = THREAD_ZOMBIE;

    cpu_flags_t flags = hal_save_irq();

    thread_t* next = pick_next();
    if (!next) {
        kpanic("No thread to schedule after thread exit!");
    }

    thread_t* old = current_thread;
    current_thread = next;
    next->state = THREAD_RUNNING;
    next->time_slice_remaining = THREAD_TIME_SLICE;

    hal_restore_irq(flags);
    switch_context(&old, &current_thread);
}

void thread_sleep(uint64_t ms) {
    if (!current_thread) return;
    uint64_t wake_tick = hal_timer_get_ticks() + (ms * 1000 / 1000);

    current_thread->wakeup_tick = wake_tick;
    current_thread->state = THREAD_SLEEPING;

    thread_yield();
}

void thread_wake(thread_t* t) {
    if (!t || t->state != THREAD_SLEEPING) return;
    t->state = THREAD_READY;
    sched_add_thread(t);
}

err_t thread_join(thread_t* t, int* exit_code) {
    if (!t) return ERR_INVAL;
    while (t->state != THREAD_ZOMBIE && t->state != THREAD_TERMINATED) {
        thread_yield();
    }
    if (exit_code) *exit_code = t->exit_code;
    return ERR_OK;
}

void thread_set_priority(thread_t* t, int priority) {
    if (!t || priority < 0 || priority > THREAD_MAX_PRIO) return;
    sched_remove_thread(t);
    t->priority = priority;
    if (t->state == THREAD_READY) sched_add_thread(t);
}

void sched_block(wait_queue_t* wq) {
    if (!wq || !current_thread) return;

    cpu_flags_t flags = hal_save_irq();
    current_thread->state = THREAD_BLOCKED;
    current_thread->next = wq->waiters;
    wq->waiters = current_thread;
    wq->count++;
    hal_restore_irq(flags);

    schedule();
}

void sched_wake(wait_queue_t* wq) {
    if (!wq) return;

    cpu_flags_t flags = hal_save_irq();
    while (wq->waiters) {
        thread_t* t = wq->waiters;
        wq->waiters = t->next;
        wq->count--;
        t->next = NULL;

        t->state = THREAD_READY;
        t->time_slice_remaining = THREAD_TIME_SLICE;
        sched_add_thread(t);
    }
    hal_restore_irq(flags);
}

static int check_sleepers(void) {
    int woken = 0;
    uint64_t now = hal_timer_get_ticks();

    for (int i = 0; i <= THREAD_MAX_PRIO; i++) {
        run_queue_t* q = &run_queues[i];
        if (!q->head) continue;

        thread_t** pp = &q->head;
        while (*pp) {
            thread_t* t = *pp;
            if (t->state == THREAD_SLEEPING && t->wakeup_tick <= now) {
                t->state = THREAD_READY;
                woken++;
            }
            pp = &t->next;
        }
    }
    return woken;
}

void idle_thread(void* arg) {
    (void)arg;
    for (;;) {
        check_sleepers();
        sched_reap_zombies();
        eventbus_dispatch();
        asm volatile("sti; hlt; cli");
    }
}

void sched_timer_tick(void) {
    if (sched_running && current_thread && current_thread != idle_thr) {
        current_thread->total_ticks++;
        if (current_thread->time_slice_remaining > 0) {
            current_thread->time_slice_remaining--;
        }
        if (current_thread->time_slice_remaining == 0) {
            need_reschedule = 1;
        }
    }
}

err_t sched_init(void) {
    kmemset(run_queues, 0, sizeof(run_queues));
    kmemset(priority_bitmap, 0, sizeof(priority_bitmap));

    idle_thr = thread_create(idle_thread, NULL, THREAD_IDLE_PRIO, "idle");
    if (!idle_thr) return ERR_NOMEM;
    idle_thr->state = THREAD_READY;
    sched_add_thread(idle_thr);

    current_thread = thread_create(NULL, NULL, THREAD_DEF_PRIO, "init");
    if (!current_thread) return ERR_NOMEM;
    current_thread->state = THREAD_RUNNING;

    sched_running = 1;

    kprintf("[SCHED] Layer 2 initialized: %d priority levels\n",
            THREAD_MAX_PRIO + 1);
    return ERR_OK;
}

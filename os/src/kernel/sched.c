#include "kernel.h"
#include "sched.h"
#include "pmm.h"
#include "vmm.h"
#include "kmalloc.h"
#include "hal.h"
#include "watchdog.h"
#include "eventbus.h"

static run_queue_t run_queues[THREAD_MAX_PRIO + 1];
static uint64_t priority_bitmap[4];
static uint64_t next_thread_id = 1;
static uint64_t kernel_cr3 = 0;
static thread_t* idle_thr = NULL;
static thread_t* all_threads_head = NULL;
static thread_t* all_threads_tail = NULL;
static uint32_t all_threads_count = 0;
static uint64_t sched_switch_count = 0;
static uint64_t sched_yield_count = 0;
thread_t* current_thread = NULL;
volatile int sched_running = 0;
volatile int need_reschedule = 0;
volatile uint64_t idle_wake_hint = 0;

void all_threads_add(thread_t* t) {
    if (!t) return;
    cpu_flags_t flags = hal_save_irq();
    if (t->all_next || t->all_prev) { hal_restore_irq(flags); return; }
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
    cpu_flags_t flags = hal_save_irq(); /* callback runs with interrupts disabled */
    thread_t* t = all_threads_head;
    while (t) {
        thread_t* next = t->all_next;
        cb(t, ctx);
        t = next;
    }
    hal_restore_irq(flags);
}

uint32_t sched_thread_count(void) {
    cpu_flags_t flags = hal_save_irq();
    uint32_t count = all_threads_count;
    hal_restore_irq(flags);
    return count;
}

void sched_reap_zombies(void) {
    cpu_flags_t flags = hal_save_irq();
    thread_t* t = all_threads_head;
    while (t) {
        thread_t* next = t->all_next;
        if (t->state == THREAD_ZOMBIE || t->state == THREAD_TERMINATED) {
            if (t == idle_thr || t == current_thread) {
                t = next;
                continue;
            }
            all_threads_remove(t);
            if (t->proc)
                list_del(&t->threads_node);
            if (t->kernel_stack)
                pmm_free_pages(VIRT_TO_PHYS((uint64_t)t->kernel_stack),
                    (t->kernel_stack_size + PAGE_SIZE - 1) / PAGE_SIZE);
            pmm_free_page(VIRT_TO_PHYS((uint64_t)t));
        }
        t = next;
    }
    hal_restore_irq(flags);
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
        q->tail->rq_next = t;
    } else {
        q->head = t;
        bitmap_set_prio(t->priority);
    }
    t->rq_prev = q->tail;
    t->rq_next = NULL;
    q->tail = t;
    q->count++;
    t->state = THREAD_READY;

    hal_restore_irq(flags);
}

void sched_remove_thread(thread_t* t) {
    if (!t) return;
    cpu_flags_t flags = hal_save_irq();
    run_queue_t* q = &run_queues[t->priority];

    if (t->rq_prev) t->rq_prev->rq_next = t->rq_next;
    else q->head = t->rq_next;

    if (t->rq_next) t->rq_next->rq_prev = t->rq_prev;
    else q->tail = t->rq_prev;

    q->count--;

    if (q->count == 0) bitmap_clear_prio(t->priority);

    t->rq_next = NULL;
    t->rq_prev = NULL;
    hal_restore_irq(flags);
}

static thread_t* pick_next(void) {
    int prio = bitmap_find_highest();
    if (prio < 0) return idle_thr;

    run_queue_t* q = &run_queues[prio];
    if (!q->head) return idle_thr;

    thread_t* t = q->head;
    if (t == idle_thr) return idle_thr;

    if (t->rq_next) {
        q->head = t->rq_next;
        q->head->rq_prev = NULL;
    } else {
        q->head = NULL;
        q->tail = NULL;
    }
    q->count--;
    if (!q->head) bitmap_clear_prio(prio);
    t->rq_next = NULL;
    t->rq_prev = NULL;

    return t;
}

void sched_set_kernel_cr3(uint64_t cr3) {
    kernel_cr3 = cr3;
}

void schedule(void) {
    if (!sched_running || !current_thread) return;

    need_reschedule = 0;
    cpu_flags_t flags = hal_save_irq();

    thread_t* next = pick_next();

    if (next == current_thread) {
        if (current_thread->state == THREAD_RUNNING)
            sched_add_thread(next);
        hal_restore_irq(flags);
        return;
    }

    if (current_thread->state == THREAD_RUNNING && current_thread != idle_thr) {
        current_thread->state = THREAD_READY;
        current_thread->priority = current_thread->base_priority;
        current_thread->age_ticks = 0;
        sched_add_thread(current_thread);
    }

    uint64_t target_cr3 = next->cr3 ? next->cr3 : kernel_cr3;
    if (target_cr3) {
        asm volatile("mov %0, %%cr3" : : "r"(target_cr3) : "memory");
    }

    thread_t* old = current_thread;
    current_thread = next;
    next->state = THREAD_RUNNING;
    next->time_slice_remaining = THREAD_TIME_SLICE;
    sched_switch_count++;

    uint64_t kstack_top = (uint64_t)next->kernel_stack + next->kernel_stack_size;
    hal_set_kernel_stack(kstack_top);

    switch_context(&old, &current_thread);
    hal_restore_irq(flags);
}

void thread_yield(void) {
    sched_yield_count++;
    cpu_flags_t flags = hal_save_irq();
    current_thread->time_slice_remaining = 0;
    hal_restore_irq(flags);
    schedule();
}

thread_t* thread_create(void (*func)(void*), void* arg,
                        int priority, const char* name) {
    uint64_t tcb_phys = pmm_alloc_page();
    if (!tcb_phys) return NULL;
    thread_t* tcb = (thread_t*)PHYS_TO_VIRT(tcb_phys);
    kmemset(tcb, 0, sizeof(thread_t));

    uint64_t kstack_phys = pmm_alloc_pages(
        (THREAD_STACK_SIZE + PAGE_SIZE - 1) / PAGE_SIZE);
    if (!kstack_phys) {
        pmm_free_page((uint64_t)tcb - KERNEL_VMA_BASE);
        return NULL;
    }
    void* kstack = (void*)PHYS_TO_VIRT(kstack_phys);
    kmemset(kstack, 0, THREAD_STACK_SIZE);
    uint64_t kstack_top = (uint64_t)kstack + THREAD_STACK_SIZE;

    uint64_t* sp = (uint64_t*)kstack_top;

    *(--sp) = (uint64_t)thread_trampoline;
    *(--sp) = (uint64_t)func;       /* r15 = func */
    *(--sp) = (uint64_t)arg;        /* r14 = arg */
    *(--sp) = 0;                    /* r13 = 0 (unused callee-saved) */
    *(--sp) = 0;                    /* r12 = 0 (unused callee-saved) */
    *(--sp) = 0;                    /* rbp = 0 (unused callee-saved) */
    *(--sp) = 0;                    /* rbx = 0 (unused callee-saved) */

    tcb->id = __sync_fetch_and_add(&next_thread_id, 1);
    tcb->rsp = (uint64_t)sp;
    tcb->cr3 = 0;
    tcb->state = THREAD_CREATED;
    tcb->base_priority = tcb->priority = (priority < 0 || priority > THREAD_MAX_PRIO) ? THREAD_DEF_PRIO : priority;
    tcb->time_slice_remaining = 0;
    tcb->age_ticks = 0;
    tcb->kernel_stack = kstack;
    tcb->kernel_stack_size = THREAD_STACK_SIZE;
    tcb->join_queue.waiters = NULL;
    tcb->join_queue.count = 0;
    kstrncpy(tcb->name, name ? name : "thread", THREAD_NAME_MAX - 1);

    all_threads_add(tcb);

    return tcb;
}

void thread_exit(int exit_code) {
    if (!current_thread) return;

    current_thread->exit_code = exit_code;
    current_thread->state = THREAD_ZOMBIE;
    sched_wake(&current_thread->join_queue);

    cpu_flags_t flags = hal_save_irq();

    thread_t* next = pick_next();
    if (!next) {
        kpanic("No thread to schedule after thread exit!");
    }

    thread_t* old = current_thread;
    current_thread = next;
    next->state = THREAD_RUNNING;
    next->time_slice_remaining = THREAD_TIME_SLICE;

    hal_set_kernel_stack((uint64_t)next->kernel_stack + next->kernel_stack_size);
    switch_context(&old, &current_thread);
    hal_restore_irq(flags);
}

void thread_sleep(uint64_t ms) {
    if (!current_thread) return;
    uint64_t wake_tick = hal_timer_get_ticks() + (ms * hal_timer_get_hz() / 1000);

    current_thread->wakeup_tick = wake_tick;
    current_thread->state = THREAD_SLEEPING;

    thread_yield();
}

err_t thread_join(thread_t* t, int* exit_code) {
    if (!t) return ERR_INVAL;
    if (t == current_thread) return ERR_INVAL;
    while (t->state != THREAD_ZOMBIE && t->state != THREAD_TERMINATED) {
        sched_block(&t->join_queue);
    }
    if (exit_code) *exit_code = t->exit_code;
    return ERR_OK;
}

void sched_block(wait_queue_t* wq) {
    if (!wq || !current_thread) return;

    cpu_flags_t flags = hal_save_irq();
    if (current_thread->state == THREAD_READY || current_thread->state == THREAD_RUNNING)
        sched_remove_thread(current_thread);
    current_thread->state = THREAD_BLOCKED;
    current_thread->wq_next = wq->waiters;
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
        wq->waiters = t->wq_next;
        wq->count--;
        t->wq_next = NULL;

        t->state = THREAD_READY;
        t->priority = t->base_priority;
        t->time_slice_remaining = THREAD_TIME_SLICE;
        t->age_ticks = 0;
        sched_add_thread(t);
    }
    hal_restore_irq(flags);
}

void sched_wake_one(wait_queue_t* wq) {
    if (!wq || !wq->waiters) return;

    cpu_flags_t flags = hal_save_irq();
    thread_t* t = wq->waiters;
    wq->waiters = t->wq_next;
    wq->count--;
    t->wq_next = NULL;

    t->state = THREAD_READY;
    t->priority = t->base_priority;
    t->time_slice_remaining = THREAD_TIME_SLICE;
    t->age_ticks = 0;
    sched_add_thread(t);
    hal_restore_irq(flags);
}

static int check_sleepers(void) {
    int woken = 0;
    uint64_t now = hal_timer_get_ticks();

    cpu_flags_t flags = hal_save_irq();
    thread_t* t = all_threads_head;
    while (t) {
        thread_t* next = t->all_next;
        if (t->state == THREAD_SLEEPING && t->wakeup_tick <= now) {
            t->state = THREAD_READY;
            t->priority = t->base_priority;
            t->time_slice_remaining = THREAD_TIME_SLICE;
            t->age_ticks = 0;
            sched_add_thread(t);
            woken++;
        }
        t = next;
    }
    hal_restore_irq(flags);
    if (woken) need_reschedule = 1;
    return woken;
}

void idle_thread(void* arg) {
    (void)arg;
    int has_mwait = hal_cpu_has_mwait();
    for (;;) {
        if (check_sleepers()) {
            schedule();
        }
        sched_reap_zombies();
        watchdog_flush();
        eventbus_dispatch();
        if (has_mwait) {
            uint64_t tmp = idle_wake_hint;
            __sync_synchronize();
            asm volatile("monitor" : : "a"(&idle_wake_hint), "c"(0), "d"(0));
            if (!need_reschedule && tmp == idle_wake_hint)
                asm volatile("sti; mwait; cli" : : "a"(0), "c"(0));
        } else {
            asm volatile("sti; hlt; cli");
        }
    }
}

uint64_t sched_get_switch_count(void) { return sched_switch_count; }
uint64_t sched_get_yield_count(void) { return sched_yield_count; }

void sched_timer_tick(void) {
    if (!sched_running || !current_thread) return;

    check_sleepers();

    /* Priority aging: age READY threads so none starve */
    {
        static uint64_t aging_counter = 0;
        if (++aging_counter >= AGING_INTERVAL) {
            aging_counter = 0;
            cpu_flags_t irq_flags = hal_save_irq();
            thread_t* t = all_threads_head;
            while (t) {
                thread_t* next = t->all_next;
                if (t != idle_thr && t->state == THREAD_READY) {
                    t->age_ticks++;
                    if (t->age_ticks >= AGING_INTERVAL && t->priority < THREAD_MAX_PRIO) {
                        sched_remove_thread(t);
                        t->priority++;
                        sched_add_thread(t);
                        t->age_ticks = 0;
                    }
                }
                t = next;
            }
            hal_restore_irq(irq_flags);
        }
    }

    current_thread->total_ticks++;
    if (current_thread->time_slice_remaining > 0) {
        current_thread->time_slice_remaining--;
    }
    if (current_thread->time_slice_remaining == 0) {
        need_reschedule = 1;
    }
    idle_wake_hint = current_thread->total_ticks;
}

err_t sched_init(void) {
    kmemset(run_queues, 0, sizeof(run_queues));
    kmemset(priority_bitmap, 0, sizeof(priority_bitmap));

    asm volatile("mov %%cr3, %0" : "=r"(kernel_cr3));
    kprintf("[SCHED] Kernel CR3 = 0x%llx\n", kernel_cr3);

    idle_thr = thread_create(idle_thread, NULL, THREAD_IDLE_PRIO, "idle");
    if (!idle_thr) return ERR_NOMEM;
    idle_thr->state = THREAD_READY;

    current_thread = thread_create(NULL, NULL, THREAD_DEF_PRIO, "init");
    if (!current_thread) return ERR_NOMEM;
    current_thread->state = THREAD_RUNNING;

    // Set the TSS's RSP0 to point to our init thread's kernel stack,
    // which will be used for interrupt handling!
    uint64_t kstack_top = (uint64_t)current_thread->kernel_stack + current_thread->kernel_stack_size;
    hal_set_kernel_stack(kstack_top);

    sched_running = 1;

    kprintf("[SCHED] Layer 2 initialized: %d priority levels\n",
            THREAD_MAX_PRIO + 1);
    return ERR_OK;
}

thread_t* sched_find_thread_by_tid(uint64_t tid) {
    if (!tid) return NULL;
    cpu_flags_t flags = hal_save_irq();
    thread_t* t = all_threads_head;
    while (t) {
        if (t->id == tid) {
            hal_restore_irq(flags);
            return t;
        }
        t = t->all_next;
    }
    hal_restore_irq(flags);
    return NULL;
}

void sched_set_priority(thread_t* t, int priority) {
    if (!t || priority < 0 || priority > THREAD_MAX_PRIO) {
        return;
    }
    cpu_flags_t flags = hal_save_irq();
    if (t->state == THREAD_READY) {
        sched_remove_thread(t);
        t->priority = priority;
        t->base_priority = priority;
        sched_add_thread(t);
    } else {
        t->priority = priority;
        t->base_priority = priority;
    }
    hal_restore_irq(flags);
}

thread_t* sched_find_thread(uint64_t id) {
    cpu_flags_t flags = hal_save_irq();
    thread_t* t = all_threads_head;
    while (t) {
        if (t->id == id) break;
        t = t->all_next;
    }
    hal_restore_irq(flags);
    return t;
}

int sched_kill_thread(uint64_t id) {
    thread_t* t = sched_find_thread(id);
    if (!t) return -1;
    if (t->state == THREAD_ZOMBIE || t->state == THREAD_TERMINATED)
        return -1;
    t->state = THREAD_ZOMBIE;
    sched_wake(&t->join_queue);
    return 0;
}

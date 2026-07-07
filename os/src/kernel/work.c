#include "work.h"
#include "kernel.h"
#include "sched.h"
#include "kmalloc.h"
#include "hal.h"

work_queue_t system_wq;

/* Deferred task tracking */
static struct list_head deferred_list;
static spinlock_t deferred_lock;
static struct work_item deferred_tick_item;
static volatile uint64_t deferred_tick_count;

err_t work_queue_init(work_queue_t* wq, const char* name) {
    if (!wq || !name) return ERR_INVAL;
    list_init(&wq->items);
    spinlock_init(&wq->lock, name);
    wq->worker = NULL;
    wq->name = name;
    return ERR_OK;
}

err_t work_queue_schedule(work_queue_t* wq, work_item_t* item) {
    if (!wq || !item || !item->func) return ERR_INVAL;
    item->state = 0;
    cpu_flags_t _sflags; spinlock_acquire(&wq->lock, &_sflags);
    list_add_tail(&item->node, &wq->items);
    spinlock_release(&wq->lock, _sflags);
    return ERR_OK;
}

err_t work_flush(work_item_t* item) {
    if (!item) return ERR_INVAL;
    /* Spin-wait for completion */
    while (item->state != 2) {
        thread_yield();
    }
    return ERR_OK;
}

static void work_worker_thread(void* arg) {
    work_queue_t* wq = (work_queue_t*)arg;
    if (!wq) return;

    kprintf("[WORK] Worker thread '%s' started\n", wq->name);

    for (;;) {
        work_item_t* item = NULL;
        cpu_flags_t _sflags;

        spinlock_acquire(&wq->lock, &_sflags);
        if (wq->items.next != &wq->items) {
            struct list_head* first = wq->items.next;
            list_del(first);
            item = container_of(first, work_item_t, node);
        }
        spinlock_release(&wq->lock, _sflags);

        if (item) {
            item->state = 1;
            item->func(item->data);
            item->state = 2;
        }

        thread_yield();
    }
}

static void deferred_poller_func(void* arg) {
    (void)arg;
    uint64_t now = hal_timer_get_ticks();
    cpu_flags_t _sflags;

    spinlock_acquire(&deferred_lock, &_sflags);
    struct list_head* iter = deferred_list.next;
    while (iter != &deferred_list) {
        deferred_task_t* task = container_of(iter, deferred_task_t, node);
        struct list_head* next = iter->next;
        if (task->active && now >= task->expires) {
            task->active = 0;
            list_del(&task->node);
            /* Execute function directly (we're in worker context) */
            spinlock_release(&deferred_lock, _sflags);
            task->func(task->data);
            spinlock_acquire(&deferred_lock, &_sflags);
        }
        iter = next;
    }
    spinlock_release(&deferred_lock, _sflags);

    /* Re-schedule poller in ~5ms */
    deferred_tick_item.func = deferred_poller_func;
    deferred_tick_item.data = NULL;
    work_queue_schedule(&system_wq, &deferred_tick_item);
}

void deferred_task_init(deferred_task_t* task, work_func_t func, void* data) {
    if (!task) return;
    task->func = func;
    task->data = data;
    task->expires = 0;
    task->active = 0;
}

err_t deferred_task_schedule(deferred_task_t* task, uint64_t delay_ms) {
    if (!task || !task->func) return ERR_INVAL;
    uint32_t hz = hal_timer_get_hz();
    if (hz == 0) hz = 1000;
    task->expires = hal_timer_get_ticks() + (hz * delay_ms / 1000);
    task->active = 1;
    cpu_flags_t _sflags; spinlock_acquire(&deferred_lock, &_sflags);
    list_add_tail(&task->node, &deferred_list);
    spinlock_release(&deferred_lock, _sflags);
    return ERR_OK;
}

err_t work_init(void) {
    err_t err = work_queue_init(&system_wq, "system_wq");
    if (err != ERR_OK) return err;

    system_wq.worker = thread_create(work_worker_thread, &system_wq,
                                     THREAD_DEF_PRIO, "kworker");
    if (!system_wq.worker) {
        kprintf("[WORK] Failed to create worker thread\n");
        return ERR_NOMEM;
    }
    sched_set_thread_affinity(system_wq.worker, 1ULL); /* pin to CPU 0 — not SMP-safe */

    list_init(&deferred_list);
    spinlock_init(&deferred_lock, "deferred");
    deferred_tick_item.func = deferred_poller_func;
    deferred_tick_item.data = NULL;
    work_queue_schedule(&system_wq, &deferred_tick_item);

    kprintf("[WORK] System work queue initialized\n");
    return ERR_OK;
}
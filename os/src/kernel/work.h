#ifndef WORK_H
#define WORK_H

#include "types.h"
#include "sync.h"

typedef void (*work_func_t)(void* data);

typedef struct work_item {
    struct list_head node;
    work_func_t func;
    void* data;
    volatile int state; /* 0=pending, 1=running, 2=done */
} work_item_t;

typedef struct {
    struct list_head items;
    spinlock_t lock;
    thread_t* worker;
    const char* name;
} work_queue_t;

err_t work_queue_init(work_queue_t* wq, const char* name);
err_t work_queue_schedule(work_queue_t* wq, work_item_t* item);
err_t work_flush(work_item_t* item);

extern work_queue_t system_wq;

err_t work_init(void);

/* Deferred tasks — schedule a one-shot function to run on the system work queue */
typedef struct deferred_task {
    struct list_head node;
    work_func_t func;
    void* data;
    uint64_t expires;
    int active;
} deferred_task_t;

void deferred_task_init(deferred_task_t* task, work_func_t func, void* data);
err_t deferred_task_schedule(deferred_task_t* task, uint64_t delay_ms);

#endif
#include "kernel.h"
#include "futex.h"
#include "sched.h"
#include "hal.h"
#include "errno.h"
#include "sync.h"

#define FUTEX_BUCKETS 64

typedef struct {
    spinlock_t   lock;
    thread_t*    waiters;
    uint32_t     count;
} futex_bucket_t;

static futex_bucket_t futex_buckets[FUTEX_BUCKETS];
static int futex_initialized = 0;

#define USER_VIRT_START 0x40000000UL
#define USER_VIRT_END   0x80000000UL

static uint32_t futex_hash(const void* addr) {
    return (((uintptr_t)addr) >> 2) & (FUTEX_BUCKETS - 1);
}

static int read_word(int32_t* addr, int32_t* out) {
    uint64_t a = (uint64_t)addr;
    if (a >= USER_VIRT_START && a < USER_VIRT_END)
        return copy_from_user(out, addr, 4);
    *out = *addr;
    return 0;
}

void futex_init(void) {
    for (int i = 0; i < FUTEX_BUCKETS; i++) {
        spinlock_init(&futex_buckets[i].lock, "futex");
        futex_buckets[i].waiters = NULL;
        futex_buckets[i].count = 0;
    }
    futex_initialized = 1;
}

int futex_wait(int32_t* uaddr, int32_t val) {
    if (!futex_initialized) return ERR_NOSYS;

    int32_t tmp;
    if (read_word(uaddr, &tmp) != 0)
        return ERR_FAULT;
    if (tmp != val)
        return ERR_AGAIN;

    futex_bucket_t* bucket = &futex_buckets[futex_hash(uaddr)];
    cpu_flags_t flags;
    spinlock_acquire(&bucket->lock, &flags);

    if (read_word(uaddr, &tmp) != 0) {
        spinlock_release(&bucket->lock, flags);
        return ERR_FAULT;
    }
    if (tmp != val) {
        spinlock_release(&bucket->lock, flags);
        return ERR_AGAIN;
    }

    sched_remove_thread(current_thread);
    current_thread->state = THREAD_BLOCKED;
    current_thread->wq_next = bucket->waiters;
    bucket->waiters = current_thread;
    bucket->count++;

    spinlock_release(&bucket->lock, flags);
    schedule();
    return ERR_OK;
}

int futex_wake(int32_t* uaddr, int32_t max_wake) {
    if (!futex_initialized) return 0;

    futex_bucket_t* bucket = &futex_buckets[futex_hash(uaddr)];
    cpu_flags_t flags;
    spinlock_acquire(&bucket->lock, &flags);

    int woken = 0;
    while (bucket->waiters && woken < max_wake) {
        thread_t* t = bucket->waiters;
        bucket->waiters = t->wq_next;
        bucket->count--;
        t->wq_next = NULL;
        t->state = THREAD_READY;
        sched_add_thread(t);
        woken++;
    }

    spinlock_release(&bucket->lock, flags);
    return woken;
}

uint64_t sys_futex(int_frame_t* frame) {
    int32_t* uaddr   = (int32_t*)frame->rdi;
    int      op      = (int)frame->rsi;
    int32_t  val     = (int32_t)frame->rdx;
    int32_t* uaddr2  = (int32_t*)frame->r8;
    int32_t  val3    = (int32_t)frame->r9;

    (void)uaddr2;
    (void)val3;

    op &= ~FUTEX_PRIVATE_FLAG;

    switch (op) {
    case FUTEX_WAIT: {
        int ret = futex_wait(uaddr, val);
        if (ret < 0) return (uint64_t)(int64_t)ret;
        return 0;
    }
    case FUTEX_WAKE: {
        int woken = futex_wake(uaddr, val);
        return (uint64_t)woken;
    }
    default:
        return (uint64_t)(int64_t)ERR_NOSYS;
    }
}

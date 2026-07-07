#include "kernel.h"
#include "lockdep.h"
#include "sched.h"

#ifdef CONFIG_LOCKDEP

static lockdep_edge_t graph[LOCKDEP_GRAPH_SIZE];
static int graph_count;
/* Raw lock: use __sync ops directly to avoid recursion (lockdep's
 * graph_lock must not itself go through lockdep instrumentation). */
static volatile int graph_locked;
static int lockdep_initialized;

static inline void raw_lock_acquire(volatile int* l) {
    while (__sync_lock_test_and_set(l, 1))
        while (*l) asm volatile("pause");
}

static inline void raw_lock_release(volatile int* l) {
    __sync_lock_release(l);
}

void lockdep_init(void) {
    graph_locked = 0;
    graph_count = 0;
    lockdep_initialized = 1;
}

#define LOCKDEP_MAX_HELD 8

void lockdep_acquire(void* lock, const char* name, int read_mode) {
    if (!lockdep_initialized || !current_thread) return;
    int depth = current_thread->held_depth;

    /* Check for recursive acquire of same lock */
    for (int i = 0; i < depth; i++) {
        if (current_thread->held_locks[i] == lock) {
            if (!current_thread->held_modes[i] || !read_mode) {
                kprintf("[LOCKDEP] recursive exclusive acquire of %s by thread %llu\n",
                        name, current_thread->id);
            }
            return;
        }
    }

    /* Check for reverse ordering (potential ABBA deadlock) */
    for (int i = 0; i < depth; i++) {
        void* held_addr = current_thread->held_locks[i];
        const char* held_name = current_thread->held_names[i];
        raw_lock_acquire(&graph_locked);
        int deadlock = 0;
        for (int j = 0; j < graph_count; j++) {
            if (graph[j].from == lock && graph[j].to == held_addr) {
                deadlock = 1;
                break;
            }
        }
        raw_lock_release(&graph_locked);
        if (deadlock) {
            kprintf("[LOCKDEP] DEADLOCK: thread %llu trying %s while holding %s, "
                    "but previously observed %s -> %s\n",
                    current_thread->id, name, held_name, name, held_name);
            kprintf("[LOCKDEP] Held locks (%d):\n", depth);
            for (int k = 0; k < depth; k++) {
                kprintf("  [%d] %s (addr %p, %s)\n", k,
                        current_thread->held_names[k] ? current_thread->held_names[k] : "?",
                        current_thread->held_locks[k],
                        current_thread->held_modes[k] ? "read" : "write");
            }
            return;
        }
    }

    /* Record ordering edges for all already-held locks */
    for (int i = 0; i < depth; i++) {
        void* held_addr = current_thread->held_locks[i];
        raw_lock_acquire(&graph_locked);
        if (graph_count < LOCKDEP_GRAPH_SIZE) {
            int found = 0;
            for (int j = 0; j < graph_count; j++) {
                if (graph[j].from == held_addr && graph[j].to == lock) {
                    found = 1;
                    break;
                }
            }
            if (!found) {
                graph[graph_count].from = held_addr;
                graph[graph_count].to = lock;
                graph_count++;
            }
        }
        raw_lock_release(&graph_locked);
    }

    /* Record the acquisition */
    if (depth < LOCKDEP_MAX_HELD) {
        current_thread->held_locks[depth] = lock;
        current_thread->held_names[depth] = name;
        current_thread->held_modes[depth] = read_mode;
        current_thread->held_depth = depth + 1;
    } else {
        kprintf("[LOCKDEP] WARN: lock depth %d exceeded for thread %llu\n",
                LOCKDEP_MAX_HELD, current_thread->id);
    }
}

void lockdep_release(void* lock) {
    if (!lockdep_initialized || !current_thread) return;
    int depth = current_thread->held_depth;

    for (int i = 0; i < depth; i++) {
        if (current_thread->held_locks[i] == lock) {
            for (int j = i; j < depth - 1; j++) {
                current_thread->held_locks[j] = current_thread->held_locks[j + 1];
                current_thread->held_names[j] = current_thread->held_names[j + 1];
                current_thread->held_modes[j] = current_thread->held_modes[j + 1];
            }
            current_thread->held_depth = depth - 1;
            return;
        }
    }

    kprintf("[LOCKDEP] WARN: release of untracked lock %p by thread %llu\n",
            lock, current_thread->id);
}

void lockdep_dump(void) {
    kprintf("[LOCKDEP] Graph edges (%d):\n", graph_count);
    for (int i = 0; i < graph_count; i++) {
        kprintf("  %p -> %p\n", graph[i].from, graph[i].to);
    }
}

#endif

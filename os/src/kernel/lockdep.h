#ifndef LOCKDEP_H
#define LOCKDEP_H

#include "types.h"

#ifdef CONFIG_LOCKDEP

#define LOCKDEP_GRAPH_SIZE  256

typedef struct lockdep_edge {
    void* from;
    void* to;
} lockdep_edge_t;

void lockdep_init(void);
void lockdep_acquire(void* lock, const char* name, int read_mode);
void lockdep_release(void* lock);
void lockdep_dump(void);

#else

static inline void lockdep_init(void) {}
static inline void lockdep_acquire(void* lock, const char* name, int read_mode) { (void)lock; (void)name; (void)read_mode; }
static inline void lockdep_release(void* lock) { (void)lock; }
static inline void lockdep_dump(void) {}

#endif

#endif

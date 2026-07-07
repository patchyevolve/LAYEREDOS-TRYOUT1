#ifndef RCU_H
#define RCU_H

#include "types.h"

/* RCU callback: user embeds this in their data, or allocates it on stack/heap.
 * Must remain valid until the callback fires. */
typedef struct rcu_callback {
    struct rcu_callback* next;
    void (*func)(void* arg);
    void* arg;
} rcu_callback_t;

void rcu_init(void);
void call_rcu(rcu_callback_t* cb, void (*func)(void*), void* arg);
void rcu_quiescent_state(void);
void rcu_barrier(void);
int  rcu_cbs_pending(void);

#endif /* RCU_H */

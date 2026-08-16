#ifndef _FUTEX_H
#define _FUTEX_H

#include "types.h"
#include "hal.h"

#define FUTEX_WAIT      0
#define FUTEX_WAKE      1
#define FUTEX_REQUEUE   3
#define FUTEX_PRIVATE_FLAG 128

void futex_init(void);
int  futex_wait(int32_t* uaddr, int32_t val);
int  futex_wake(int32_t* uaddr, int32_t max_wake);
uint64_t sys_futex(int_frame_t* frame);

#endif

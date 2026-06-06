#ifndef SYSCALL_H
#define SYSCALL_H

#include "types.h"
#include "syscall_defs.h"

struct int_frame;

void syscall_init(void);
void syscall_handler(struct int_frame* frame);

#endif

#ifndef PIPE_H
#define PIPE_H

#include "types.h"
#include "vfs.h"
#include "sync.h"
#include "sched.h"

#define PIPE_BUF_SIZE 4096

typedef struct pipe {
    uint8_t         buf[PIPE_BUF_SIZE];
    uint32_t        read_pos;
    uint32_t        write_pos;
    uint32_t        count;
    spinlock_t      lock;
    wait_queue_t    readers;
    wait_queue_t    writers;
    int             read_closed;
    int             write_closed;
} pipe_t;

int pipe_create(int fds[2]);

#endif

#ifndef _SYS_TIME_H
#define _SYS_TIME_H

#include <sys/types.h>

#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1

struct timespec {
    time_t tv_sec;
    long   tv_nsec;
};

#endif

#ifndef EPOLL_H
#define EPOLL_H

#include "types.h"

#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_MOD 2
#define EPOLL_CTL_DEL 3

#define EPOLLIN    0x001
#define EPOLLOUT   0x004
#define EPOLLERR   0x008
#define EPOLLHUP   0x010
#define EPOLLET    0x80000000u

#define MAX_EPOLL_ITEMS 64
#define MAX_EPOLL_INSTANCES 32

typedef struct epoll_event {
    uint32_t events;
    uint64_t data;
} epoll_event_t;

void epoll_init(void);

/* Internal API (for kernel self-tests and other kernel modules) */
int do_epoll_create1(int flags);
int do_epoll_ctl(int epfd, int op, int fd, epoll_event_t* event);
int do_epoll_wait(int epfd, epoll_event_t* events, int maxevents, int timeout);

#endif

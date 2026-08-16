#include "kernel.h"
#include "epoll.h"
#include "vfs.h"
#include "sched.h"
#include "net.h"
#include "kmalloc.h"
#include "process.h"
#include "hal.h"

#define USER_VIRT_START 0x40000000UL
#define USER_VIRT_END   0x80000000UL

/* Copy helpers that work for both user and kernel addresses.
 * When called from a syscall (user address), uses copy_from/to_user.
 * When called from kernel context (self-test), does direct memcpy. */
static int epoll_copy_from_user(void* dst, const void* src, size_t n) {
    uint64_t ua = (uint64_t)src;
    if (ua >= USER_VIRT_START && ua < USER_VIRT_END)
        return copy_from_user(dst, src, n);
    kmemcpy(dst, src, n);
    return 0;
}

static int epoll_copy_to_user(void* dst, const void* src, size_t n) {
    uint64_t ua = (uint64_t)dst;
    if (ua >= USER_VIRT_START && ua < USER_VIRT_END)
        return copy_to_user(dst, src, n);
    kmemcpy(dst, src, n);
    return 0;
}

typedef struct {
    int fd;
    uint32_t events;
    uint64_t data;
    int used;
} epoll_item_t;

typedef struct {
    int used;
    spinlock_t lock;
    wait_queue_t wq;
    epoll_item_t items[MAX_EPOLL_ITEMS];
    int item_count;
} epoll_instance_t;

static epoll_instance_t epoll_instances[MAX_EPOLL_INSTANCES];
static int epoll_used[MAX_EPOLL_INSTANCES];

/* VFS file ops for epoll fds */
static int epoll_vfs_open(vfs_node_t* node) {
    (void)node;
    return 0;
}

static int epoll_vfs_close(vfs_node_t* node) {
    epoll_instance_t* ep = (epoll_instance_t*)node->private_data;
    if (ep)
        ep->used = 0;
    return 0;
}

static int64_t epoll_vfs_read(vfs_node_t* node, void* buf, uint64_t count, uint64_t offset) {
    (void)node; (void)buf; (void)count; (void)offset;
    return -1;
}

static int64_t epoll_vfs_write(vfs_node_t* node, const void* buf, uint64_t count, uint64_t offset) {
    (void)node; (void)buf; (void)count; (void)offset;
    return -1;
}

static vfs_file_ops_t epoll_vfs_ops = {
    .open  = epoll_vfs_open,
    .close = epoll_vfs_close,
    .read  = epoll_vfs_read,
    .write = epoll_vfs_write,
};

static vfs_fs_t epoll_fs = {
    .name = "epoll",
    .ops  = &epoll_vfs_ops,
};

void epoll_init(void) {
    kmemset(epoll_instances, 0, sizeof(epoll_instances));
    kmemset(epoll_used, 0, sizeof(epoll_used));
    kprintf("[EPOLL] epoll subsystem initialized\n");
}

static epoll_instance_t* epoll_lookup(int fd) {
    if (fd < 0) return NULL;
    cpu_flags_t _sf;
    spinlock_acquire(&vfs_global_lock, &_sf);
    if (fd >= VFS_MAX_FDS || !vfs_get_fd_table()[fd].used) {
        spinlock_release(&vfs_global_lock, _sf);
        return NULL;
    }
    vfs_node_t* node = vfs_get_fd_table()[fd].node;
    spinlock_release(&vfs_global_lock, _sf);
    if (node && node->fs == &epoll_fs)
        return (epoll_instance_t*)node->private_data;
    return NULL;
}

int do_epoll_create1(int flags) {
    (void)flags;

    int idx = -1;
    for (int i = 0; i < MAX_EPOLL_INSTANCES; i++) {
        if (!epoll_used[i]) {
            idx = i;
            break;
        }
    }
    if (idx < 0) return ERR_NOMEM;

    epoll_used[idx] = 1;
    epoll_instance_t* ep = &epoll_instances[idx];
    kmemset(ep, 0, sizeof(epoll_instance_t));
    spinlock_init(&ep->lock, "epoll_lock");
    wait_queue_init(&ep->wq);

    vfs_node_t* node = (vfs_node_t*)kmalloc(sizeof(vfs_node_t));
    if (!node) {
        epoll_used[idx] = 0;
        return ERR_NOMEM;
    }
    kmemset(node, 0, sizeof(vfs_node_t));
    kstrncpy(node->name, "epoll", VFS_MAX_NAME - 1);
    node->fs = &epoll_fs;
    node->private_data = ep;
    node->dynamic = 1;

    vfs_fd_t* ft = vfs_get_fd_table();
    int fd = -1;
    cpu_flags_t _sf;
    spinlock_acquire(&vfs_global_lock, &_sf);
    for (int i = 3; i < VFS_MAX_FDS; i++) {
        if (!ft[i].used) {
            ft[i].node = node;
            ft[i].offset = 0;
            ft[i].flags = 0;
            ft[i].used = 1;
            __sync_fetch_and_add(&node->refcount, 1);
            fd = i;
            break;
        }
    }
    spinlock_release(&vfs_global_lock, _sf);

    if (fd < 0) {
        kfree(node);
        epoll_used[idx] = 0;
        return ERR_NOMEM;
    }

    return fd;
}

int do_epoll_ctl(int epfd, int op, int fd, epoll_event_t* user_ev) {
    if (epfd == fd && op != EPOLL_CTL_DEL)
        return ERR_INVAL;

    epoll_instance_t* ep = epoll_lookup(epfd);
    if (!ep) return ERR_BADFD;

    epoll_event_t ev;
    if (op == EPOLL_CTL_ADD || op == EPOLL_CTL_MOD) {
        if (!user_ev) return ERR_FAULT;
        if (epoll_copy_from_user(&ev, user_ev, sizeof(ev)) != 0)
            return ERR_FAULT;
    }

    int ret = 0;
    cpu_flags_t _sf;
    spinlock_acquire(&ep->lock, &_sf);

    int idx = -1;
    for (int i = 0; i < ep->item_count; i++) {
        if (ep->items[i].used && ep->items[i].fd == fd) {
            idx = i;
            break;
        }
    }

    switch (op) {
    case EPOLL_CTL_ADD:
        if (idx >= 0) { ret = ERR_EXIST; break; }
        if (ep->item_count >= MAX_EPOLL_ITEMS) { ret = ERR_NOSPACE; break; }
        idx = ep->item_count++;
        ep->items[idx].fd = fd;
        ep->items[idx].used = 1;
        ep->items[idx].events = ev.events;
        ep->items[idx].data = ev.data;
        break;

    case EPOLL_CTL_MOD:
        if (idx < 0) { ret = ERR_NOENT; break; }
        ep->items[idx].events = ev.events;
        ep->items[idx].data = ev.data;
        break;

    case EPOLL_CTL_DEL:
        if (idx < 0) { ret = ERR_NOENT; break; }
        ep->items[idx].used = 0;
        if (idx < ep->item_count - 1)
            ep->items[idx] = ep->items[ep->item_count - 1];
        kmemset(&ep->items[ep->item_count - 1], 0, sizeof(epoll_item_t));
        ep->item_count--;
        break;

    default:
        ret = ERR_INVAL;
    }

    spinlock_release(&ep->lock, _sf);
    return ret;
}

int do_epoll_wait(int epfd, epoll_event_t* user_events, int maxevents, int timeout) {
    epoll_instance_t* ep = epoll_lookup(epfd);
    if (!ep) return ERR_BADFD;
    if (maxevents <= 0) return ERR_INVAL;
    if (maxevents > MAX_EPOLL_ITEMS) maxevents = MAX_EPOLL_ITEMS;

    uint64_t deadline = 0;
    if (timeout > 0)
        deadline = rdtsc() + (uint64_t)timeout * tsc_khz;

    while (1) {
        epoll_event_t evbuf[MAX_EPOLL_ITEMS];
        int ready = 0;

        cpu_flags_t _sf;
        spinlock_acquire(&ep->lock, &_sf);

        for (int i = 0; i < ep->item_count && ready < maxevents; i++) {
            epoll_item_t* item = &ep->items[i];
            if (!item->used) continue;

            int revents = 0;
            socket_t* s = sock_lookup(item->fd);
            if (s) {
                sock_poll(s, item->events, &revents);
            } else {
                vfs_poll(item->fd, item->events, &revents);
            }

            if (revents) {
                evbuf[ready].events = (uint32_t)(revents & item->events);
                evbuf[ready].data = item->data;
                ready++;
            }
        }

        spinlock_release(&ep->lock, _sf);

        if (ready > 0) {
            if (epoll_copy_to_user(user_events, evbuf, (size_t)ready * sizeof(epoll_event_t)) != 0)
                return ERR_FAULT;
            return ready;
        }

        if (timeout == 0) return 0;
        if (timeout > 0 && rdtsc() >= deadline) return 0;

        {
            int sleep_ms = 10;
            if (timeout > 0) {
                int64_t remaining = (int64_t)((deadline - rdtsc()) / tsc_khz);
                if (remaining <= 0) return 0;
                if (remaining < sleep_ms) sleep_ms = (int)remaining;
            }
            thread_sleep((uint64_t)sleep_ms);
        }
    }
}

/* Syscall entry points */
uint64_t sys_epoll_create1(int_frame_t* frame) {
    int flags = (int)frame->rdi;
    return (uint64_t)(int64_t)do_epoll_create1(flags);
}

uint64_t sys_epoll_ctl(int_frame_t* frame) {
    int epfd = (int)frame->rdi;
    int op = (int)frame->rsi;
    int fd = (int)frame->rdx;
    epoll_event_t* ev = (epoll_event_t*)frame->r10;
    return (uint64_t)(int64_t)do_epoll_ctl(epfd, op, fd, ev);
}

uint64_t sys_epoll_wait(int_frame_t* frame) {
    int epfd = (int)frame->rdi;
    epoll_event_t* events = (epoll_event_t*)frame->rsi;
    int maxevents = (int)frame->rdx;
    int timeout = (int)frame->r10;
    return (uint64_t)(int64_t)do_epoll_wait(epfd, events, maxevents, timeout);
}

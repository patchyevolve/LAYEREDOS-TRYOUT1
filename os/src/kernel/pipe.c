#include "kernel.h"
#include "pipe.h"
#include "pmm.h"
#include "kmalloc.h"
#include "sched.h"
#include "vfs.h"
#include "net.h"
#include "fcntl.h"

static int pipe_open(vfs_node_t* node) {
    (void)node;
    return 0;
}

/* Called on every vfs_close to signal read/write side closure.
 * pipe_destructor handles freeing pipe_t when both nodes are freed. */
static int pipe_close(vfs_node_t* node) {
    if (!node || !node->private_data) return 0;
    pipe_t* p = (pipe_t*)node->private_data;
    int is_read = (node->flags & 1);
    cpu_flags_t _sflags; spinlock_acquire(&p->lock, &_sflags);
    if (is_read)
        p->read_closed = 1;
    else
        p->write_closed = 1;
    int both_closed = p->read_closed && p->write_closed;
    spinlock_release(&p->lock, _sflags);
    if (both_closed) {
        sched_wake(&p->readers);
        sched_wake(&p->writers);
    }
    return 0;
}

/* Called when the last reference to a pipe vfs_node is freed.
 * Decrements pipe_t's refcount; frees pipe_t when both nodes are gone. */
static void pipe_destructor(void* private_data) {
    if (!private_data) return;
    pipe_t* p = (pipe_t*)private_data;
    if (__sync_fetch_and_sub(&p->refcount, 1) == 1) {
        kfree(p);
    }
}

static int64_t pipe_read(vfs_node_t* node, void* buf, uint64_t count, uint64_t offset) {
    (void)offset;
    pipe_t* p = (pipe_t*)node->private_data;
    if (!p) return -1;
    uint64_t done = 0;
    while (done < count) {
        cpu_flags_t _sflags; spinlock_acquire(&p->lock, &_sflags);
        if (p->count > 0) {
            uint32_t chunk = count - done > p->count ? p->count : (uint32_t)(count - done);
            for (uint32_t i = 0; i < chunk; i++) {
                ((uint8_t*)buf)[done++] = p->buf[p->read_pos];
                p->read_pos = (p->read_pos + 1) % PIPE_BUF_SIZE;
            }
            p->count -= chunk;
            if (p->writers.waiters) sched_wake(&p->writers);
            spinlock_release(&p->lock, _sflags);
        } else {
            if (p->write_closed) { spinlock_release(&p->lock, _sflags); break; }
            spinlock_release(&p->lock, _sflags);
            if (done > 0) break; /* Don't block — we already have data to return */
            sched_block(&p->readers);
        }
    }
    return (int64_t)done;
}

static int64_t pipe_write(vfs_node_t* node, const void* buf, uint64_t count, uint64_t offset) {
    (void)offset;
    pipe_t* p = (pipe_t*)node->private_data;
    if (!p) return -1;
    uint64_t done = 0;
    while (done < count) {
        cpu_flags_t _sflags; spinlock_acquire(&p->lock, &_sflags);
        if (p->read_closed) { spinlock_release(&p->lock, _sflags); return -1; }
        if (p->count < PIPE_BUF_SIZE) {
            uint32_t space = PIPE_BUF_SIZE - p->count;
            uint32_t chunk = count - done > space ? space : (uint32_t)(count - done);
            for (uint32_t i = 0; i < chunk; i++) {
                p->buf[p->write_pos] = ((const uint8_t*)buf)[done++];
                p->write_pos = (p->write_pos + 1) % PIPE_BUF_SIZE;
            }
            p->count += chunk;
            if (p->readers.waiters) sched_wake(&p->readers);
            spinlock_release(&p->lock, _sflags);
        } else {
            spinlock_release(&p->lock, _sflags);
            sched_block(&p->writers);
        }
    }
    return (int64_t)done;
}

static int pipe_poll(vfs_node_t* node, int events, int* revents) {
    pipe_t* p = (pipe_t*)node->private_data;
    if (!p) { *revents = POLLNVAL; return -1; }
    int r = 0;
    cpu_flags_t _sf;
    spinlock_acquire(&p->lock, &_sf);
    if ((events & POLLIN) && p->count > 0)
        r |= POLLIN;
    if ((events & POLLOUT) && !p->read_closed)
        r |= POLLOUT;
    if (p->write_closed)
        r |= POLLHUP;
    spinlock_release(&p->lock, _sf);
    *revents = r;
    return 0;
}

static vfs_file_ops_t pipe_ops = {
    .open  = pipe_open,
    .close = pipe_close,
    .read  = pipe_read,
    .write = pipe_write,
    .poll  = pipe_poll,
};

static vfs_fs_t pipe_fs = {
    .name = "pipe",
    .ops  = &pipe_ops,
};

int pipe_create(int fds[2]) {
    pipe_t* p = (pipe_t*)kmalloc(sizeof(pipe_t));
    if (!p) return -1;
    kmemset(p, 0, sizeof(pipe_t));
    spinlock_init(&p->lock, "pipe_lock");
    wait_queue_init(&p->readers);
    wait_queue_init(&p->writers);
    p->refcount = 2; /* one for rnode, one for wnode */

    vfs_node_t* rnode = (vfs_node_t*)kmalloc(sizeof(vfs_node_t));
    vfs_node_t* wnode = (vfs_node_t*)kmalloc(sizeof(vfs_node_t));
    if (!rnode || !wnode) {
        kfree(p);
        if (rnode) kfree(rnode);
        if (wnode) kfree(wnode);
        return -1;
    }
    kmemset(rnode, 0, sizeof(vfs_node_t));
    kmemset(wnode, 0, sizeof(vfs_node_t));
    kstrncpy(rnode->name, "pipe:r", VFS_MAX_NAME - 1);
    kstrncpy(wnode->name, "pipe:w", VFS_MAX_NAME - 1);
    rnode->fs = &pipe_fs;
    wnode->fs = &pipe_fs;
    rnode->private_data = p;
    wnode->private_data = p;
    rnode->flags = 1;
    wnode->flags = 0;
    rnode->dynamic = 1;
    wnode->dynamic = 1;
    rnode->destructor = pipe_destructor;
    wnode->destructor = pipe_destructor;

    vfs_fd_t* ft = vfs_get_fd_table();
    cpu_flags_t _sf;
    spinlock_acquire(&vfs_global_lock, &_sf);
    for (int i = 0; i < VFS_MAX_FDS; i++) {
        if (!ft[i].used) {
            ft[i].node = rnode;
            ft[i].offset = 0;
            ft[i].flags = O_RDONLY;
            ft[i].used = 1;
            __sync_fetch_and_add(&rnode->refcount, 1);
            fds[0] = i;
            break;
        }
    }
    for (int i = 0; i < VFS_MAX_FDS; i++) {
        if (!ft[i].used) {
            ft[i].node = wnode;
            ft[i].offset = 0;
            ft[i].flags = O_WRONLY;
            ft[i].used = 1;
            __sync_fetch_and_add(&wnode->refcount, 1);
            fds[1] = i;
            spinlock_release(&vfs_global_lock, _sf);
            return 0;
        }
    }
    /* Error: no slot for write fd — clean up the read fd that was already assigned */
    ft[fds[0]].used = 0;
    spinlock_release(&vfs_global_lock, _sf);
    pipe_close(rnode);
    pipe_close(wnode);
    kfree(rnode);
    kfree(wnode);
    return -1;
}

#include "kernel.h"
#include "pipe.h"
#include "pmm.h"
#include "kmalloc.h"
#include "sched.h"
#include "vfs.h"

static int pipe_open(vfs_node_t* node) {
    (void)node;
    return 0;
}

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
        wait_queue_t r = p->readers, w = p->writers;
        kfree(p);
        if (r.waiters) sched_wake(&r);
        if (w.waiters) sched_wake(&w);
    }
    return 0;
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

static vfs_file_ops_t pipe_ops = {
    .open  = pipe_open,
    .close = pipe_close,
    .read  = pipe_read,
    .write = pipe_write,
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

    for (int i = 0; i < VFS_MAX_FDS; i++) {
        extern vfs_fd_t fd_table[VFS_MAX_FDS];
        if (!fd_table[i].used) {
            fd_table[i].node = rnode;
            fd_table[i].offset = 0;
            fd_table[i].flags = 0;
            fd_table[i].used = 1;
            fds[0] = i;
            break;
        }
    }
    for (int i = 0; i < VFS_MAX_FDS; i++) {
        extern vfs_fd_t fd_table[VFS_MAX_FDS];
        if (!fd_table[i].used) {
            fd_table[i].node = wnode;
            fd_table[i].offset = 0;
            fd_table[i].flags = 0;
            fd_table[i].used = 1;
            fds[1] = i;
            return 0;
        }
    }
    pipe_close(rnode);
    pipe_close(wnode);
    kfree(rnode);
    kfree(wnode);
    return -1;
}

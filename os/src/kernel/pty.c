#include "kernel.h"
#include "pty.h"
#include "vfs.h"
#include "sched.h"
#include "sync.h"
#include "process.h"
#include "signal.h"
#include "kmalloc.h"
#include "tty.h"
#include "hal.h"
#include "net.h"

/*
 * Pseudo-terminal (PTY) implementation.
 *
 * A PTY is a pair of endpoints — master and slave.
 *   - Master read:  returns data written by slave
 *   - Master write: pushes data into slave's raw input buffer
 *   - Slave read:   line-discipline processed (canon/raw, echo, signal chars)
 *   - Slave write:  pushes data into master's read buffer
 *   - Slave ioctl:  termios get/set, process group get/set
 *
 * A userspace program (terminal emulator) opens a PTY pair via SYS_PTY_PAIR,
 * gets back [master_fd, slave_fd], then typically fork+exec a child with
 * slave_fd duped onto 0/1/2.
 */

/* ── PTY control block ────────────────────────────────────────────────── */

typedef struct pty {
    int used;
    int id;

    /* Master buffer — slave writes land here, master reads from here */
    char m_buf[PTY_BUF_SIZE];
    int  m_head;
    int  m_tail;
    spinlock_t m_lock;
    wait_queue_t m_waitq;

    /* Slave raw buffer — master writes land here, slave line-discipline reads */
    char s_raw_buf[PTY_BUF_SIZE];
    int  s_raw_head;
    int  s_raw_tail;
    spinlock_t s_raw_lock;
    wait_queue_t s_raw_waitq;

    /* Slave canonical line buffer (completed lines for canon mode) */
    char line_buf[PTY_LINE_BUF_SIZE];
    int  line_pos;
    int  line_count;
    wait_queue_t canon_waitq;

    /* Termios settings */
    uint32_t lflag;
    char cc[PTY_CC_NCCS];

    /* Job control */
    uint64_t fg_pgid;
    uint64_t session;

    /* VFS nodes for master and slave endpoints */
    vfs_node_t* master_node;
    vfs_node_t* slave_node;

    /* Close tracking */
    int master_closed;
    int slave_closed;
} pty_t;

static pty_t pty_pool[PTY_MAX];
static spinlock_t pty_pool_lock;

/* ── Forward declarations ──────────────────────────────────────────────── */

static int  pty_master_open(vfs_node_t* node);
static int  pty_master_close(vfs_node_t* node);
static int64_t pty_master_read(vfs_node_t* node, void* buf, uint64_t count, uint64_t offset);
static int64_t pty_master_write(vfs_node_t* node, const void* buf, uint64_t count, uint64_t offset);

static int  pty_slave_open(vfs_node_t* node);
static int  pty_slave_close(vfs_node_t* node);
static int64_t pty_slave_read(vfs_node_t* node, void* buf, uint64_t count, uint64_t offset);
static int64_t pty_slave_write(vfs_node_t* node, const void* buf, uint64_t count, uint64_t offset);
static int  pty_slave_ioctl(vfs_node_t* node, uint64_t request, void* argp);
static int  pty_master_poll(vfs_node_t* node, int events, int* revents);
static int  pty_slave_poll(vfs_node_t* node, int events, int* revents);

/* ── VFS file operations ──────────────────────────────────────────────── */

static vfs_file_ops_t pty_master_ops = {
    .open  = pty_master_open,
    .close = pty_master_close,
    .read  = pty_master_read,
    .write = pty_master_write,
    .poll  = pty_master_poll,
};

static vfs_file_ops_t pty_slave_ops = {
    .open  = pty_slave_open,
    .close = pty_slave_close,
    .read  = pty_slave_read,
    .write = pty_slave_write,
    .ioctl = pty_slave_ioctl,
    .poll  = pty_slave_poll,
};

static vfs_fs_t pty_master_fs = {
    .name = "pty:m",
    .ops  = &pty_master_ops,
};

static vfs_fs_t pty_slave_fs = {
    .name = "pty:s",
    .ops  = &pty_slave_ops,
};

/* ── Initialisation ────────────────────────────────────────────────────── */

void pty_init(void) {
    kmemset(pty_pool, 0, sizeof(pty_pool));
    spinlock_init(&pty_pool_lock, "pty_pool");
    kprintf("[PTY] Pseudo-terminal subsystem initialized (%d slots)\n", PTY_MAX);
}

/* ── PTY allocation / deallocation ────────────────────────────────────── */

static pty_t* pty_alloc(void) {
    cpu_flags_t _sflags;
    spinlock_acquire(&pty_pool_lock, &_sflags);
    for (int i = 0; i < PTY_MAX; i++) {
        if (!pty_pool[i].used) {
            pty_t* p = &pty_pool[i];
            kmemset(p, 0, sizeof(pty_t));
            p->used = 1;
            p->id = i;
            spinlock_init(&p->m_lock, "pty_m");
            spinlock_init(&p->s_raw_lock, "pty_s_raw");
            wait_queue_init(&p->m_waitq);
            wait_queue_init(&p->s_raw_waitq);
            wait_queue_init(&p->canon_waitq);
            p->lflag = TTY_ECHO | TTY_ICANON | TTY_ISIG | TTY_ECHOE;
            p->cc[TTY_VINTR]  = TTY_DEF_INTR;
            p->cc[TTY_VQUIT]  = TTY_DEF_QUIT;
            p->cc[TTY_VERASE] = TTY_DEF_ERASE;
            p->cc[TTY_VEOF]   = TTY_DEF_EOF;
            p->cc[TTY_VSUSP]  = TTY_DEF_SUSP;
            p->fg_pgid = 0;
            p->session = 0;
            spinlock_release(&pty_pool_lock, _sflags);
            return p;
        }
    }
    spinlock_release(&pty_pool_lock, _sflags);
    return NULL;
}

static void pty_free(pty_t* p) {
    if (!p) return;
    p->used = 0;
    p->master_node = NULL;
    p->slave_node = NULL;
}

/* ── Check if background process (helper for slave read/write) ────────── */

static int pty_is_bg(pty_t* p) {
    process_t* proc = current_thread ? current_thread->proc : NULL;
    if (!proc) return 0;
    uint64_t fg = p->fg_pgid;
    if (fg <= 0) return 0;
    return (proc->pgid != fg) ? 1 : 0;
}

/* ── Send signal to foreground process group of this PTY ──────────────── */

static void pty_signal_fg(pty_t* p, int sig) {
    uint64_t pgid = p->fg_pgid;
    if (pgid <= 0) return;
    signal_send_pgid((pid_t)pgid, sig);
}

/* ── Master operations ─────────────────────────────────────────────────── */

static int pty_master_open(vfs_node_t* node) {
    (void)node;
    return 0;
}

static int pty_master_close(vfs_node_t* node) {
    pty_t* p = (pty_t*)node->private_data;
    if (!p) return 0;
    p->master_closed = 1;
    sched_wake(&p->s_raw_waitq);
    sched_wake(&p->canon_waitq);
    if (p->slave_closed)
        pty_free(p);
    return 0;
}

static int64_t pty_master_read(vfs_node_t* node, void* buf, uint64_t count, uint64_t offset) {
    (void)offset;
    pty_t* p = (pty_t*)node->private_data;
    if (!p) return -1;

    uint64_t nread = 0;
    while (nread < count) {
        cpu_flags_t _sflags;
        spinlock_acquire(&p->m_lock, &_sflags);
        if (p->m_head != p->m_tail) {
            ((uint8_t*)buf)[nread++] = p->m_buf[p->m_tail];
            p->m_tail = (p->m_tail + 1) % PTY_BUF_SIZE;
            spinlock_release(&p->m_lock, _sflags);
        } else {
            spinlock_release(&p->m_lock, _sflags);
            if (p->slave_closed) break;
            sched_block(&p->m_waitq);
        }
    }
    return (int64_t)nread;
}

static int64_t pty_master_write(vfs_node_t* node, const void* buf, uint64_t count, uint64_t offset) {
    (void)offset;
    pty_t* p = (pty_t*)node->private_data;
    if (!p) return -1;

    uint64_t written = 0;
    while (written < count) {
        cpu_flags_t _sflags;
        spinlock_acquire(&p->s_raw_lock, &_sflags);
        int next = (p->s_raw_head + 1) % PTY_BUF_SIZE;
        if (next != p->s_raw_tail) {
            p->s_raw_buf[p->s_raw_head] = ((const uint8_t*)buf)[written++];
            p->s_raw_head = next;
            spinlock_release(&p->s_raw_lock, _sflags);
            sched_wake(&p->s_raw_waitq);
        } else {
            spinlock_release(&p->s_raw_lock, _sflags);
            if (p->slave_closed) return (int64_t)written;
            sched_block(&p->s_raw_waitq);
        }
    }
    return (int64_t)written;
}

/* ── Slave raw byte read (internal, used by line discipline) ──────────── */

static char pty_read_raw_byte(pty_t* p) {
    for (;;) {
        cpu_flags_t _sflags;
        spinlock_acquire(&p->s_raw_lock, &_sflags);
        if (p->s_raw_head != p->s_raw_tail) {
            char c = p->s_raw_buf[p->s_raw_tail];
            p->s_raw_tail = (p->s_raw_tail + 1) % PTY_BUF_SIZE;
            spinlock_release(&p->s_raw_lock, _sflags);
            return c;
        }
        spinlock_release(&p->s_raw_lock, _sflags);
        if (p->master_closed) return 0;
        sched_block(&p->s_raw_waitq);
    }
}

/* ── Slave line discipline (canonical mode processing) ─────────────────── */

static int pty_process_canon(pty_t* p, char* buf, uint64_t count) {
    int nread = 0;

    for (;;) {
        if (p->line_count > 0) {
            cpu_flags_t _sflags;
            spinlock_acquire(&p->s_raw_lock, &_sflags);
            if (p->line_count > 0) {
                int end = 0;
                while (end < p->line_pos && p->line_buf[end] != '\0')
                    end++;

                int to_copy = end;
                if (to_copy > (int)count) to_copy = (int)count;

                for (int i = 0; i < to_copy; i++)
                    buf[nread++] = p->line_buf[i];

                int remaining = p->line_pos - (end + 1);
                if (remaining > 0) {
                    for (int i = 0; i < remaining; i++)
                        p->line_buf[i] = p->line_buf[end + 1 + i];
                }
                p->line_pos = remaining;
                p->line_count--;

                spinlock_release(&p->s_raw_lock, _sflags);
                return nread;
            }
            spinlock_release(&p->s_raw_lock, _sflags);
        }

        char c = pty_read_raw_byte(p);

        if (p->lflag & TTY_ISIG) {
            int sig = 0;
            if (c == p->cc[TTY_VINTR])  sig = SIGINT;
            else if (c == p->cc[TTY_VQUIT]) sig = SIGQUIT;
            else if (c == p->cc[TTY_VSUSP]) sig = SIGTSTP;
            if (sig) {
                if (p->lflag & TTY_ECHO) {
                    if (sig == SIGINT)  kprintf("^C\n");
                    else if (sig == SIGQUIT) kprintf("^\\\n");
                    else if (sig == SIGTSTP) kprintf("^Z\n");
                }
                pty_signal_fg(p, sig);
                return -1;
            }
        }

        if (p->lflag & TTY_ICANON) {
            if ((c == '\b' || c == 127) && p->line_pos > 0) {
                p->line_pos--;
                if (p->lflag & TTY_ECHO)
                    kprintf("\b \b");
                continue;
            }

            if (c == '\r') c = '\n';

            if (c == '\n') {
                if (p->line_pos < PTY_LINE_BUF_SIZE - 1) {
                    p->line_buf[p->line_pos] = '\0';
                    p->line_pos++;
                }
                p->line_count++;
                if (p->lflag & TTY_ECHO) kputchar('\n');
                continue;
            }

            if (c == p->cc[TTY_VEOF]) {
                if (p->line_pos > 0) {
                    p->line_buf[p->line_pos] = '\0';
                    p->line_pos++;
                    p->line_count++;
                    continue;
                }
                return 0;
            }

            if (p->line_pos < PTY_LINE_BUF_SIZE - 1) {
                p->line_buf[p->line_pos++] = c;
                if (p->lflag & TTY_ECHO) {
                    if (c < 32 && c != '\t')
                        kprintf("^%c", (char)('A' + c - 1));
                    else
                        kputchar(c);
                }
            }
        } else {
            if (nread < (int)count) {
                buf[nread++] = c;
                if (p->lflag & TTY_ECHO) kputchar(c);
            }
            if (nread > 0) return nread;
        }
    }
}

/* ── Slave operations ──────────────────────────────────────────────────── */

static int pty_slave_open(vfs_node_t* node) {
    (void)node;
    return 0;
}

static int pty_slave_close(vfs_node_t* node) {
    pty_t* p = (pty_t*)node->private_data;
    if (!p) return 0;
    p->slave_closed = 1;
    sched_wake(&p->m_waitq);
    if (p->master_closed)
        pty_free(p);
    return 0;
}

static int64_t pty_slave_read(vfs_node_t* node, void* buf, uint64_t count, uint64_t offset) {
    (void)offset;
    pty_t* p = (pty_t*)node->private_data;
    if (!p) return -1;

    if (pty_is_bg(p)) {
        process_t* proc = current_thread ? current_thread->proc : NULL;
        if (proc) {
            signal_send(proc->pid, SIGTTIN);
            signal_process(proc);
            return 0;
        }
    }

    if (p->lflag & TTY_ICANON) {
        int ret = pty_process_canon(p, (char*)buf, count);
        if (ret < 0) return 0;
        return ret;
    }

    int nread = 0;
    while (nread < (int)count) {
        char c = pty_read_raw_byte(p);
        if (p->lflag & TTY_ISIG) {
            int sig = 0;
            if (c == p->cc[TTY_VINTR])  sig = SIGINT;
            else if (c == p->cc[TTY_VSUSP]) sig = SIGTSTP;
            else if (c == p->cc[TTY_VQUIT]) sig = SIGQUIT;
            if (sig) {
                pty_signal_fg(p, sig);
                return nread > 0 ? nread : 0;
            }
        }
        ((uint8_t*)buf)[nread++] = c;
        if (p->lflag & TTY_ECHO) kputchar(c);
    }
    return nread;
}

static int64_t pty_slave_write(vfs_node_t* node, const void* buf, uint64_t count, uint64_t offset) {
    (void)offset;
    pty_t* p = (pty_t*)node->private_data;
    if (!p) return -1;

    if (pty_is_bg(p) && (p->lflag & TTY_TOSTOP)) {
        process_t* proc = current_thread ? current_thread->proc : NULL;
        if (proc) {
            signal_send(proc->pid, SIGTTOU);
            signal_process(proc);
            return (int64_t)count;
        }
    }

    uint64_t written = 0;
    while (written < count) {
        cpu_flags_t _sflags;
        spinlock_acquire(&p->m_lock, &_sflags);
        int next = (p->m_head + 1) % PTY_BUF_SIZE;
        if (next != p->m_tail) {
            p->m_buf[p->m_head] = ((const uint8_t*)buf)[written++];
            p->m_head = next;
            spinlock_release(&p->m_lock, _sflags);
            sched_wake(&p->m_waitq);
        } else {
            spinlock_release(&p->m_lock, _sflags);
            if (p->master_closed) return (int64_t)written;
            sched_block(&p->m_waitq);
        }
    }
    return (int64_t)written;
}

static int pty_slave_ioctl(vfs_node_t* node, uint64_t request, void* argp) {
    pty_t* p = (pty_t*)node->private_data;
    if (!p) return -1;

    switch (request) {
        case TCGETATTR: {
            termios_t ti;
            ti.c_lflag = p->lflag;
            for (int i = 0; i < PTY_CC_NCCS; i++)
                ti.c_cc[i] = p->cc[i];
            if (copy_to_user(argp, &ti, sizeof(ti)) != 0)
                return -1;
            return 0;
        }
        case TCSETATTR: {
            termios_t ti;
            if (copy_from_user(&ti, argp, sizeof(ti)) != 0)
                return -1;
            p->lflag = ti.c_lflag;
            for (int i = 0; i < PTY_CC_NCCS; i++)
                p->cc[i] = ti.c_cc[i];
            return 0;
        }
        case TIOCGPGRP: {
            pid_t pgid = (pid_t)p->fg_pgid;
            if (copy_to_user(argp, &pgid, sizeof(pgid)) != 0)
                return -1;
            return 0;
        }
        case TIOCSPGRP: {
            pid_t pgid;
            if (copy_from_user(&pgid, argp, sizeof(pgid)) != 0)
                return -1;
            p->fg_pgid = (uint64_t)pgid;
            return 0;
        }
        default:
            return -1;
    }
}

/* ── PTY poll ──────────────────────────────────────────────────────────── */

static int pty_master_poll(vfs_node_t* node, int events, int* revents) {
    pty_t* p = (pty_t*)node->private_data;
    if (!p) { *revents = POLLNVAL; return -1; }
    int r = 0;
    if (events & POLLIN) {
        cpu_flags_t _sf;
        spinlock_acquire(&p->m_lock, &_sf);
        if (p->m_head != p->m_tail) r |= POLLIN;
        spinlock_release(&p->m_lock, _sf);
    }
    if (events & POLLOUT)
        r |= POLLOUT;
    if (p->slave_closed)
        r |= POLLHUP;
    *revents = r;
    return 0;
}

static int pty_slave_poll(vfs_node_t* node, int events, int* revents) {
    pty_t* p = (pty_t*)node->private_data;
    if (!p) { *revents = POLLNVAL; return -1; }
    int r = 0;
    if (events & POLLIN) {
        cpu_flags_t _sf;
        spinlock_acquire(&p->s_raw_lock, &_sf);
        if (p->s_raw_head != p->s_raw_tail) r |= POLLIN;
        if (p->line_count > 0) r |= POLLIN;
        spinlock_release(&p->s_raw_lock, _sf);
    }
    if (events & POLLOUT)
        r |= POLLOUT;
    if (p->master_closed)
        r |= POLLHUP;
    *revents = r;
    return 0;
}

/* ── PTY pair creation (allocates FDs for master + slave) ──────────────── */

int pty_pair_create(int fds[2]) {
    pty_t* p = pty_alloc();
    if (!p) return -1;

    p->master_node = (vfs_node_t*)kmalloc(sizeof(vfs_node_t));
    p->slave_node  = (vfs_node_t*)kmalloc(sizeof(vfs_node_t));
    if (!p->master_node || !p->slave_node) {
        kmemset(fds, 0, sizeof(int) * 2);
        kfree(p->master_node);
        kfree(p->slave_node);
        pty_free(p);
        return -1;
    }

    kmemset(p->master_node, 0, sizeof(vfs_node_t));
    kstrncpy(p->master_node->name, "pty:master", VFS_MAX_NAME - 1);
    p->master_node->fs = &pty_master_fs;
    p->master_node->private_data = p;
    p->master_node->flags = 0;
    p->master_node->dynamic = 1;

    kmemset(p->slave_node, 0, sizeof(vfs_node_t));
    kstrncpy(p->slave_node->name, "pty:slave", VFS_MAX_NAME - 1);
    p->slave_node->fs = &pty_slave_fs;
    p->slave_node->private_data = p;
    p->slave_node->flags = 0;
    p->slave_node->dynamic = 1;

    int mfd = -1, sfd = -1;
    vfs_fd_t* ft = vfs_get_fd_table();
    for (int i = 0; i < VFS_MAX_FDS; i++) {
        if (!ft[i].used) {
            ft[i].node   = p->master_node;
            ft[i].offset = 0;
            ft[i].flags  = 0;
            ft[i].used   = 1;
            __sync_fetch_and_add(&p->master_node->refcount, 1);
            mfd = i;
            break;
        }
    }

    for (int i = 0; i < VFS_MAX_FDS; i++) {
        if (!ft[i].used) {
            ft[i].node   = p->slave_node;
            ft[i].offset = 0;
            ft[i].flags  = 0;
            ft[i].used   = 1;
            __sync_fetch_and_add(&p->slave_node->refcount, 1);
            sfd = i;
            break;
        }
    }

    if (mfd < 0 || sfd < 0) {
        if (mfd >= 0) { ft[mfd].used = 0; }
        kfree(p->master_node);
        kfree(p->slave_node);
        pty_free(p);
        return -1;
    }

    fds[0] = mfd;
    fds[1] = sfd;
    return 0;
}

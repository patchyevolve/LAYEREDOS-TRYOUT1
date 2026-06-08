#include "kernel.h"
#include "tty.h"
#include "process.h"
#include "kmalloc.h"
#include "hal.h"
#include "vfs.h"

tty_t tty_console;

/* TTY VFS filesystem — wraps the line-discipline operations */
static vfs_file_ops_t tty_file_ops = {
    .open  = tty_vfs_open,
    .close = tty_vfs_close,
    .read  = tty_vfs_read,
    .write = tty_vfs_write,
};

static vfs_fs_t tty_fs = {
    .name = "tty",
    .ops  = &tty_file_ops,
};

/* ── Worker (process context) for ISR-detected signals ────────────────── */

static void tty_signal_fg(int sig);
static void tty_signal_worker(void* data) {
    tty_t* t = (tty_t*)data;
    int sig;
    while ((sig = t->sig_pending) > 0) {
        t->sig_pending = 0;
        if (t->lflag & TTY_ECHO) {
            if (sig == SIGINT)  kprintf("^C\n");
            else if (sig == SIGQUIT) kprintf("^\\\n");
            else if (sig == SIGTSTP) kprintf("^Z\n");
        }
        tty_signal_fg(sig);
    }
    /* If a new signal arrived during the loop, reschedule */
    if (t->sig_pending > 0)
        work_queue_schedule(&system_wq, &t->sig_work);
}

void tty_init(void) {
    tty_t* t = &tty_console;

    spinlock_init(&t->raw_lock, "tty_raw");

    /* Initialize termios defaults */
    t->lflag = TTY_ECHO | TTY_ICANON | TTY_ISIG | TTY_ECHOE;
    t->cc[TTY_VINTR]  = TTY_DEF_INTR;
    t->cc[TTY_VQUIT]  = TTY_DEF_QUIT;
    t->cc[TTY_VERASE] = TTY_DEF_ERASE;
    t->cc[TTY_VEOF]   = TTY_DEF_EOF;
    t->cc[TTY_VSUSP]  = TTY_DEF_SUSP;

    /* Job control: default foreground pgid = 0 (kernel) */
    t->fg_pgid = 0;
    t->session = 0;

    /* Signal work item */
    t->sig_pending = 0;
    list_init(&t->sig_work.node);
    t->sig_work.func  = tty_signal_worker;
    t->sig_work.data  = t;
    t->sig_work.state = 2;  /* start as "done" so first schedule works */

    /* Set up the VFS node */
    vfs_node_t* n = &t->node;
    kmemset(n, 0, sizeof(*n));
    kstrncpy(n->name, "ttyS0", VFS_MAX_NAME - 1);
    n->flags = 0;
    n->size  = 0;
    n->fs    = &tty_fs;
    n->private_data = t;

    /* Wire FDs 0/1/2 to the console TTY */
    extern vfs_fd_t fd_table[VFS_MAX_FDS];
    for (int i = 0; i < 3; i++) {
        fd_table[i].node   = &tty_console.node;
        fd_table[i].offset = 0;
        fd_table[i].flags  = 0;
        fd_table[i].used   = 1;
    }

    kprintf("[TTY] Console terminal initialised\n");
}

/* ── ISR interface: push byte into raw ring buffer ──────────────────────── */

void tty_input_push(char c) {
    tty_t* t = &tty_console;

    /* ISR context: check for immediate signal delivery to foreground pg */
    if ((t->lflag & TTY_ISIG) && t->fg_pgid > 0) {
        int sig = 0;
        if (c == t->cc[TTY_VINTR])  sig = SIGINT;
        else if (c == t->cc[TTY_VQUIT]) sig = SIGQUIT;
        else if (c == t->cc[TTY_VSUSP]) sig = SIGTSTP;
        if (sig) {
            t->sig_pending = sig;
            /* Schedule work only if the previous item completed */
            if (t->sig_work.state == 2)
                work_queue_schedule(&system_wq, &t->sig_work);
            return; /* consume the byte — don't push to buffer */
        }
    }

    /* Normal path: push into raw ring buffer */
    cpu_flags_t _sflags;
    spinlock_acquire(&t->raw_lock, &_sflags);
    int next = (t->raw_head + 1) % TTY_BUF_SIZE;
    if (next != t->raw_tail) {
        t->raw_buf[t->raw_head] = c;
        t->raw_head = next;
    }
    spinlock_release(&t->raw_lock, _sflags);
    /* Wake any waiting reader (shell or canonical) */
    sched_wake(&t->raw_waitq);
}

/* ── Raw byte read (for shell – bypasses line discipline) ───────────────── */

char tty_getchar(void) {
    tty_t* t = &tty_console;
    for (;;) {
        cpu_flags_t flags = hal_save_irq();
        if (t->raw_head != t->raw_tail) {
            char c = t->raw_buf[t->raw_tail];
            t->raw_tail = (t->raw_tail + 1) % TTY_BUF_SIZE;
            hal_restore_irq(flags);
            return c;
        }
        /* Interrupts disabled: ISR cannot insert a wakeup
         * between check (above) and block (below). */
        sched_block(&t->raw_waitq);
        hal_restore_irq(flags);
    }
}

/* ── Internal: read one raw byte (used by line discipline) ──────────────── */

static char tty_read_raw_byte(void) {
    return tty_getchar();
}

/* ── Internal: send signal to foreground process group ──────────────────── */

static void tty_signal_fg(int sig) {
    uint64_t pgid = tty_console.fg_pgid;
    if (pgid <= 0) return;
    signal_send_pgid((pid_t)pgid, sig);
}

/* ── Line discipline processing (returns when a line is ready) ──────────── */

static int tty_process_canon(char* buf, uint64_t count) {
    tty_t* t = &tty_console;
    int nread = 0;

    for (;;) {
        /* If we already have a complete line and copied some data, return */
        if (t->line_count > 0) {
            cpu_flags_t _sflags;
            spinlock_acquire(&t->raw_lock, &_sflags);
            if (t->line_count > 0) {
                /* Copy from line_buf — the completed line is at the start
                 * of line_buf followed by a '\0'.  After copy we compact
                 * any remaining partial line. */

                /* Find the end of the first completed line */
                int end = 0;
                while (end < t->line_pos && t->line_buf[end] != '\0')
                    end++;

                int to_copy = end;
                if (to_copy > (int)count) to_copy = (int)count;

                /* Copy line to user buffer */
                for (int i = 0; i < to_copy; i++)
                    buf[nread++] = t->line_buf[i];

                /* Remove the completed line from the buffer */
                int remaining = t->line_pos - (end + 1); /* skip past \0 */
                if (remaining > 0) {
                    for (int i = 0; i < remaining; i++)
                        t->line_buf[i] = t->line_buf[end + 1 + i];
                }
                t->line_pos = remaining;
                t->line_count--;

                spinlock_release(&t->raw_lock, _sflags);
                return nread;
            }
            spinlock_release(&t->raw_lock, _sflags);
        }

        /* Read a raw byte (blocks until available) */
        char c = tty_read_raw_byte();

        /* Check for signal characters */
        if (t->lflag & TTY_ISIG) {
            if (c == t->cc[TTY_VINTR]) {
                if (t->lflag & TTY_ECHO) kprintf("^C\n");
                tty_signal_fg(SIGINT);
                return -1; /* read interrupted */
            }
            if (c == t->cc[TTY_VQUIT]) {
                if (t->lflag & TTY_ECHO) kprintf("^\\\n");
                tty_signal_fg(SIGQUIT);
                return -1;
            }
            if (c == t->cc[TTY_VSUSP]) {
                if (t->lflag & TTY_ECHO) kprintf("^Z\n");
                tty_signal_fg(SIGTSTP);
                return -1;
            }
        }

        /* Canonical mode processing */
        if (t->lflag & TTY_ICANON) {
            if ((c == '\b' || c == 127) && t->line_pos > 0) {
                /* Backspace / erase */
                t->line_pos--;
                if (t->lflag & TTY_ECHOE) {
                    kprintf("\b \b");
                } else if (t->lflag & TTY_ECHO) {
                    kprintf("\b \b");
                }
                continue;
            }

            if (c == '\r') c = '\n';

            if (c == '\n') {
                /* End of line */
                if (t->line_pos < (int)sizeof(t->line_buf) - 1) {
                    t->line_buf[t->line_pos] = '\0';
                    t->line_pos++;
                }
                t->line_count++;
                if (t->lflag & TTY_ECHO) kputchar('\n');

                /* A line is ready — next loop iteration will copy it */
                continue;
            }

            if (c == t->cc[TTY_VEOF]) {
                /* Ctrl-D: return EOF condition */
                if (t->line_pos > 0) {
                    /* If there's partial line data, flush it as if newline */
                    t->line_buf[t->line_pos] = '\0';
                    t->line_pos++;
                    t->line_count++;
                    continue;
                }
                /* Empty line → return 0 (EOF) */
                return 0;
            }

            /* Regular character */
            if (t->line_pos < (int)sizeof(t->line_buf) - 1) {
                t->line_buf[t->line_pos++] = c;
                if (t->lflag & TTY_ECHO) {
                    if (c < 32 && c != '\t') {
                        kprintf("^%c", (char)('A' + c - 1));
                    } else {
                        kputchar(c);
                    }
                }
            }
            /* If buffer full, we just drop the character */
        } else {
            /* Raw mode: return bytes immediately */
            if (nread < (int)count) {
                buf[nread++] = c;
                if (t->lflag & TTY_ECHO) kputchar(c);
            }
            if (nread > 0) return nread;
        }
    }
}

/* ── VFS operations ─────────────────────────────────────────────────────── */

int tty_vfs_open(vfs_node_t* node) {
    (void)node;
    return 0;
}

int tty_vfs_close(vfs_node_t* node) {
    (void)node;
    return 0;
}

int64_t tty_vfs_read(vfs_node_t* node, void* buf, uint64_t count, uint64_t offset) {
    (void)offset;
    (void)node; /* always uses tty_console */

    tty_t* t = &tty_console;

    if (t->lflag & TTY_ICANON) {
        int ret = tty_process_canon((char*)buf, count);
        if (ret < 0) return 0; /* signal interrupted → return 0 bytes */
        return ret;
    }

    /* Raw mode: copy bytes from raw buffer */
    int nread = 0;
    while (nread < (int)count) {
        char c = tty_read_raw_byte();
        if (t->lflag & TTY_ISIG) {
            if (c == t->cc[TTY_VINTR]) {
                tty_signal_fg(SIGINT);
                return nread > 0 ? nread : 0;
            }
            if (c == t->cc[TTY_VSUSP]) {
                tty_signal_fg(SIGTSTP);
                return nread > 0 ? nread : 0;
            }
            if (c == t->cc[TTY_VQUIT]) {
                tty_signal_fg(SIGQUIT);
                return nread > 0 ? nread : 0;
            }
        }
        ((char*)buf)[nread++] = c;
        if (t->lflag & TTY_ECHO) kputchar(c);
    }
    return nread;
}

int64_t tty_vfs_write(vfs_node_t* node, const void* buf, uint64_t count, uint64_t offset) {
    (void)node;
    (void)offset;
    const char* p = (const char*)buf;
    for (uint64_t i = 0; i < count; i++) {
        char c = p[i];
        if (c == '\n') kputchar('\r');
        kputchar(c);
    }
    return (int64_t)count;
}

/* ── Job control ────────────────────────────────────────────────────────── */

uint64_t tty_get_fg_pgid(void) {
    return tty_console.fg_pgid;
}

void tty_set_fg_pgid(uint64_t pgid) {
    tty_console.fg_pgid = pgid;
}

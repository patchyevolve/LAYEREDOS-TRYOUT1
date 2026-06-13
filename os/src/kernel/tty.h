#ifndef TTY_H
#define TTY_H

#include "types.h"
#include "vfs.h"
#include "sched.h"
#include "sync.h"
#include "work.h"

#define TTY_BUF_SIZE 4096
#define TTY_CC_NCCS 32

/* c_cc indices */
#define TTY_VINTR   0
#define TTY_VQUIT   1
#define TTY_VERASE  2
#define TTY_VKILL   3
#define TTY_VEOF    4
#define TTY_VEOL    5
#define TTY_VSUSP   6

/* lflag bits */
#define TTY_ECHO    0x001
#define TTY_ICANON  0x002
#define TTY_ISIG    0x004
#define TTY_ECHOE   0x008
#define TTY_ECHOK   0x010
#define TTY_ECHONL  0x020
#define TTY_TOSTOP  0x100

/* Default control characters */
#define TTY_DEF_INTR  3   /* Ctrl-C */
#define TTY_DEF_QUIT  28  /* Ctrl-\ */
#define TTY_DEF_ERASE 127 /* DEL */
#define TTY_DEF_EOF   4   /* Ctrl-D */
#define TTY_DEF_SUSP  26  /* Ctrl-Z */

/* Termios ioctl constants */
#define TCGETATTR   0x5401
#define TCSETATTR   0x5402
#define TCGETS      TCGETATTR
#define TCSETS      TCSETATTR
#define TIOCGPGRP   0x540F
#define TIOCSPGRP   0x5410

/* Termios struct (minimal — only what we use) */
typedef struct termios {
    uint32_t c_lflag;
    char     c_cc[TTY_CC_NCCS];
} termios_t;

typedef struct tty {
    /* Raw input ring buffer (filled by ISR) */
    char raw_buf[TTY_BUF_SIZE];
    volatile int raw_head;
    volatile int raw_tail;
    spinlock_t raw_lock;
    wait_queue_t raw_waitq;

    /* Canonical processing state */
    char line_buf[256];
    int line_pos;
    int line_count;   /* number of completed lines */
    wait_queue_t canon_waitq;

    /* Termios-like settings */
    uint32_t lflag;
    char cc[TTY_CC_NCCS];

    /* Job control */
    uint64_t fg_pgid;
    uint64_t session;

    /* ISR -> process-context signal delivery */
    volatile int sig_pending;
    work_item_t sig_work;

    /* VFS node */
    vfs_node_t node;
} tty_t;

extern tty_t tty_console;

void tty_init(void);
void tty_input_push(char c);
char tty_getchar(void);

/* VFS operations */
int  tty_vfs_open(vfs_node_t* node);
int  tty_vfs_close(vfs_node_t* node);
int64_t tty_vfs_read(vfs_node_t* node, void* buf, uint64_t count, uint64_t offset);
int64_t tty_vfs_write(vfs_node_t* node, const void* buf, uint64_t count, uint64_t offset);

/* Job control */
uint64_t tty_get_fg_pgid(void);
void  tty_set_fg_pgid(uint64_t pgid);

#endif

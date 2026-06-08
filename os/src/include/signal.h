#ifndef SIGNAL_H
#define SIGNAL_H

#ifdef TYPES_H
/* Kernel context - included via types.h */
#include "types.h"

/* Signal numbers */
#define SIGHUP    1
#define SIGINT    2
#define SIGQUIT   3
#define SIGILL    4
#define SIGTRAP   5
#define SIGABRT   6
#define SIGBUS    7
#define SIGFPE    8
#define SIGKILL   9
#define SIGUSR1  10
#define SIGSEGV  11
#define SIGUSR2  12
#define SIGPIPE  13
#define SIGALRM  14
#define SIGTERM  15
#define SIGSTKFLT 16
#define SIGCHLD  17
#define SIGCONT  18
#define SIGSTOP  19
#define SIGTSTP  20
#define SIGTTIN  21
#define SIGTTOU  22
#define SIGURG   23
#define SIGXCPU  24
#define SIGXFSZ  25
#define SIGVTALRM 26
#define SIGPROF  27
#define SIGWINCH 28
#define SIGIO    29
#define SIGPWR   30
#define SIGSYS   31
#define NSIG     32

/* Signal actions */
#define SIG_DFL ((void*)0)
#define SIG_IGN ((void*)1)

/* Flags for sigaction */
#define SA_NOCLDSTOP 1
#define SA_NOCLDWAIT 2
#define SA_SIGINFO   4
#define SA_ONSTACK   8
#define SA_RESTART  16
#define SA_NODEFER  32

typedef void (*sighandler_t)(int);

typedef struct sigaction {
    void (*sa_handler)(int);
    uint64_t sa_flags;
    void (*sa_restorer)(void);
} sigaction_t;

/* Signal action codes for default handling */
#define SIGACT_TERM   0
#define SIGACT_IGN    1
#define SIGACT_CORE   2
#define SIGACT_STOP   3
#define SIGACT_CONT   4

/* Signal frame (pushed on user stack during signal delivery) */
typedef struct sigframe {
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rip, cs, rflags, rsp, ss;
    int sig;
    int pad;
} sigframe_t;

#define SIGNAL_TRAMPOLINE_ADDR 0x7FFF0000UL

#else
/* User-space context - POSIX signal types */
#include <sys/types.h>

#define SIGABRT 1
#define SIGALRM 2
#define SIGBUS  3
#define SIGCHLD 4
#define SIGCONT 5
#define SIGFPE  6
#define SIGHUP  7
#define SIGILL  8
#define SIGINT  9
#define SIGKILL 10
#define SIGPIPE 11
#define SIGQUIT 12
#define SIGSEGV 13
#define SIGSTOP 14
#define SIGTERM 15
#define SIGTSTP 16
#define SIGUSR1 17
#define SIGUSR2 18

#define SIG_DFL ((__sighandler_t)0)
#define SIG_IGN ((__sighandler_t)1)
#define SIG_ERR ((__sighandler_t)-1)

typedef void (*__sighandler_t)(int);

struct sigaction {
    void (*sa_handler)(int);
    unsigned long sa_flags;
    void (*sa_restorer)(void);
};

extern __sighandler_t signal(int sig, __sighandler_t handler);
extern int sigaction(int sig, const struct sigaction* act, struct sigaction* oldact);

#endif

#endif

#ifndef SIGNAL_H
#define SIGNAL_H

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

#endif

#include <signal.h>
#include <errno.h>
#include "syscall.h"

__sighandler_t signal(int sig, __sighandler_t handler) {
    struct sigaction new_act;
    struct sigaction old_act;
    new_act.sa_handler = handler;
    new_act.sa_flags = 0;
    long ret = __syscall3(SYS_SIGACTION, sig, (long)&new_act, (long)&old_act);
    if (ret < 0) { errno = (int)(-ret); return SIG_ERR; }
    return old_act.sa_handler;
}

int sigaction(int sig, const struct sigaction* act, struct sigaction* oldact) {
    long ret = __syscall3(SYS_SIGACTION, sig, (long)act, (long)oldact);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return 0;
}

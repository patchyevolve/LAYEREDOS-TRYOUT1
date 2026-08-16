#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include "syscall.h"


ssize_t read(int fd, void* buf, size_t count) {
    long ret = __syscall3(SYS_READ, fd, (long)buf, count);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (ssize_t)ret;
}

ssize_t write(int fd, const void* buf, size_t count) {
    long ret = __syscall3(SYS_WRITE, fd, (long)buf, count);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (ssize_t)ret;
}

int open(const char* path, int flags, ...) {
    long ret = __syscall2(SYS_OPEN, (long)path, flags);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (int)ret;
}

int close(int fd) {
    long ret = __syscall1(SYS_CLOSE, fd);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return 0;
}

pid_t fork(void) {
    long ret = __syscall0(SYS_FORK);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (pid_t)ret;
}

pid_t getpid(void) {
    return (pid_t)__syscall0(SYS_GETPID);
}

pid_t getppid(void) {
    return (pid_t)__syscall0(SYS_GETPPID);
}

int execve(const char* path, char* const argv[], char* const envp[]) {
    (void)argv; (void)envp;
    long ret = __syscall1(SYS_EXECVE, (long)path);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return 0;
}

pid_t waitpid(pid_t pid, int* status, int options) {
    long ret = __syscall3(SYS_WAITPID, pid, (long)status, options);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (pid_t)ret;
}

void _exit(int status) {
    __syscall1(SYS_EXIT, status);
    for (;;);
}

int kill(pid_t pid, int sig) {
    long ret = __syscall2(SYS_KILL, pid, sig);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return 0;
}

void* sbrk(intptr_t increment) {
    long ret = __syscall1(SYS_SBRK, increment);
    if (ret < 0) { errno = (int)(-ret); return (void*)-1; }
    return (void*)ret;
}

off_t lseek(int fd, off_t offset, int whence) {
    long ret = __syscall3(SYS_LSEEK, fd, offset, whence);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (off_t)ret;
}

int pipe(int fds[2]) {
    long ret = __syscall1(SYS_PIPE, (long)fds);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return 0;
}

int dup2(int oldfd, int newfd) {
    long ret = __syscall2(SYS_DUP2, oldfd, newfd);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (int)ret;
}

int getcwd(char* buf, size_t size) {
    long ret = __syscall2(SYS_GETCWD, (long)buf, size);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (int)ret;
}

int chdir(const char* path) {
    long ret = __syscall1(SYS_CHDIR, (long)path);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return 0;
}

unsigned int sleep(unsigned int seconds) {
    __syscall1(SYS_SLEEP, seconds * 1000ULL);
    return 0;
}

int ioctl(int fd, unsigned long request, void* argp) {
    long ret = __syscall3(SYS_IOCTL, fd, (long)request, (long)argp);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (int)ret;
}

pid_t getpgid(pid_t pid) {
    long ret = __syscall1(SYS_GETPGID, (long)pid);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (pid_t)ret;
}

int setpgid(pid_t pid, pid_t pgid) {
    long ret = __syscall2(SYS_SETPGID, (long)pid, (long)pgid);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (int)ret;
}

int pty_pair(int fds[2]) {
    long ret = __syscall1(SYS_PTY_PAIR, (long)fds);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (int)ret;
}

/* ---- Socket wrappers ---- */

int socket(int domain, int type, int protocol) {
    long ret = __syscall3(SYS_SOCKET, domain, type, protocol);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (int)ret;
}

int bind(int sockfd, const struct sockaddr* addr, socklen_t addrlen) {
    long ret = __syscall3(SYS_BIND, sockfd, (long)addr, addrlen);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (int)ret;
}

int connect(int sockfd, const struct sockaddr* addr, socklen_t addrlen) {
    long ret = __syscall3(SYS_CONNECT, sockfd, (long)addr, addrlen);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (int)ret;
}

int listen(int sockfd, int backlog) {
    long ret = __syscall2(SYS_LISTEN, sockfd, backlog);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (int)ret;
}

int accept(int sockfd, struct sockaddr* addr, socklen_t* addrlen) {
    long ret = __syscall3(SYS_ACCEPT, sockfd, (long)addr, (long)addrlen);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (int)ret;
}

ssize_t send(int sockfd, const void* buf, size_t len, int flags) {
    (void)flags;
    long ret = __syscall3(SYS_SEND, sockfd, (long)buf, len);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (ssize_t)ret;
}

ssize_t recv(int sockfd, void* buf, size_t len, int flags) {
    (void)flags;
    long ret = __syscall3(SYS_RECV, sockfd, (long)buf, len);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (ssize_t)ret;
}

ssize_t sendto(int sockfd, const void* buf, size_t len, int flags,
               const struct sockaddr* dest_addr, socklen_t addrlen) {
    (void)flags;
    long ret = __syscall6(SYS_SENDTO, sockfd, (long)buf, len, 0,
                          (long)dest_addr, addrlen);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (ssize_t)ret;
}

ssize_t recvfrom(int sockfd, void* buf, size_t len, int flags,
                 struct sockaddr* src_addr, socklen_t* addrlen) {
    (void)flags;
    long ret = __syscall6(SYS_RECVFROM, sockfd, (long)buf, len, 0,
                          (long)src_addr, (long)addrlen);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (ssize_t)ret;
}

int setsockopt(int sockfd, int level, int optname,
               const void* optval, socklen_t optlen) {
    long ret = __syscall5(SYS_SETSOCKOPT, sockfd, level, optname,
                          (long)optval, optlen);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return 0;
}

int getsockopt(int sockfd, int level, int optname,
               void* optval, socklen_t* addrlen) {
    long ret = __syscall5(SYS_GETSOCKOPT, sockfd, level, optname,
                          (long)optval, (long)addrlen);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return 0;
}

int clock_gettime(int clk_id, struct timespec* tp) {
    long ret = __syscall2(SYS_CLOCK_GETTIME, clk_id, (long)tp);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return 0;
}

int getsockname(int sockfd, struct sockaddr* addr, socklen_t* addrlen) {
    long ret = __syscall3(SYS_GETSOCKNAME, sockfd, (long)addr, (long)addrlen);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return 0;
}

int getpeername(int sockfd, struct sockaddr* addr, socklen_t* addrlen) {
    long ret = __syscall3(SYS_GETPEERNAME, sockfd, (long)addr, (long)addrlen);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return 0;
}

int poll(struct pollfd* fds, int nfds, int timeout_ms) {
    long ret = __syscall3(SYS_POLL, (long)fds, nfds, timeout_ms);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (int)ret;
}

/* ---- Memory mapping ---- */

void* mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset) {
    long ret = __syscall6(SYS_MMAP, (long)addr, length, prot, flags, fd, offset);
    if (ret < 0 && ret > -4096) { errno = (int)(-ret); return MAP_FAILED; }
    return (void*)ret;
}

int munmap(void* addr, size_t length) {
    long ret = __syscall2(SYS_MUNMAP, (long)addr, length);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return 0;
}

int mprotect(void* addr, size_t len, int prot) {
    long ret = __syscall3(SYS_MPROTECT, (long)addr, len, prot);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return 0;
}

/* ---- fcntl ---- */

int fcntl(int fd, int cmd, long arg) {
    long ret = __syscall3(SYS_FCNTL, fd, cmd, arg);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (int)ret;
}

/* ---- Signal handling ---- */

int sigaction(int sig, const struct sigaction* act, struct sigaction* oldact) {
    long ret = __syscall3(SYS_SIGACTION, sig, (long)act, (long)oldact);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return 0;
}

void (*signal(int sig, void (*handler)(int)))(int) {
    struct sigaction new_act;
    struct sigaction old_act;
    new_act.sa_handler = handler;
    new_act.sa_flags = 0;
    new_act.sa_restorer = 0;
    if (sigaction(sig, &new_act, &old_act) < 0)
        return SIG_ERR;
    return old_act.sa_handler;
}

/* ---- Threading ---- */

pid_t clone(int (*fn)(void*), void* stack, int flags, void* arg) {
    long ret = __syscall2(SYS_CLONE, flags, (long)stack);
    if (ret == 0) {
        /* Child: call the function and exit */
        int rc = fn(arg);
        _exit(rc);
    }
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (pid_t)ret;
}

/* ---- Security / Capabilities ---- */

int capget(unsigned long pid, unsigned long* caps) {
    long ret = __syscall2(SYS_CAPGET, pid, (long)caps);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return 0;
}

int capset(unsigned long caps) {
    long ret = __syscall1(SYS_CAPSET, caps);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return 0;
}

int audit_read(audit_entry_t* buf, unsigned long count) {
    long ret = __syscall2(SYS_AUDIT_READ, (long)buf, count);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (int)ret;
}

/* ---- UID/GID ---- */

uid_t getuid(void) {
    return (uid_t)__syscall0(SYS_GETUID);
}

uid_t geteuid(void) {
    return (uid_t)__syscall0(SYS_GETEUID);
}

gid_t getgid(void) {
    return (gid_t)__syscall0(SYS_GETGID);
}

gid_t getegid(void) {
    return (gid_t)__syscall0(SYS_GETEGID);
}

int setuid(uid_t uid) {
    long ret = __syscall1(SYS_SETUID, uid);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return 0;
}

int setgid(gid_t gid) {
    long ret = __syscall1(SYS_SETGID, gid);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return 0;
}

/* ---- Random ---- */

ssize_t getrandom(void* buf, size_t count, unsigned int flags) {
    long ret = __syscall3(SYS_GETRANDOM, (long)buf, count, flags);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (ssize_t)ret;
}

/* ---- Syscall filter ---- */

int set_syscall_filter(const unsigned long long* masks, size_t count) {
    long ret = __syscall2(SYS_SET_SSF, (long)masks, count);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return 0;
}

/* ---- socketpair ---- */
int socketpair(int domain, int type, int protocol, int sv[2]) {
    long ret = __syscall5(SYS_SOCKETPAIR, domain, type, protocol, (long)sv, 0);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return 0;
}

/* ---- prctl ---- */
int prctl(int option, unsigned long arg2, unsigned long arg3,
          unsigned long arg4, unsigned long arg5) {
    long ret = __syscall5(SYS_PRCTL, option, arg2, arg3, arg4, arg5);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (int)ret;
}

/* ---- veth_pair ---- */
int veth_pair(netconfig_req_t* req) {
    long ret = __syscall1(SYS_VETH_PAIR, (long)req);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (int)ret;
}

/* ---- netconfig ---- */
int netconfig(netconfig_req_t* req) {
    long ret = __syscall1(SYS_NETCONFIG, (long)req);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (int)ret;
}

/* ---- veth_move ---- */
int veth_move(int pair_idx, int end_sel) {
    long ret = __syscall2(SYS_VETH_MOVE, pair_idx, end_sel);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (int)ret;
}

/* ---- extra POSIX wrappers ---- */
int chmod(const char* path, unsigned int mode) {
    long ret = __syscall2(SYS_CHMOD, (long)path, mode);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (int)ret;
}

int link(const char* target, const char* linkpath) {
    long ret = __syscall2(SYS_LINK, (long)target, (long)linkpath);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (int)ret;
}

int symlink(const char* target, const char* linkpath) {
    long ret = __syscall2(SYS_SYMLINK, (long)target, (long)linkpath);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (int)ret;
}

int readlink(const char* path, char* buf, size_t size) {
    long ret = __syscall3(SYS_READLINK, (long)path, (long)buf, size);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (int)ret;
}

int rmdir(const char* path) {
    long ret = __syscall1(SYS_RMDIR, (long)path);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (int)ret;
}

int ftruncate(int fd, uint64_t size) {
    long ret = __syscall2(SYS_FTRUNCATE, fd, (long)size);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (int)ret;
}

int dup(int oldfd) {
    long ret = __syscall1(SYS_DUP, oldfd);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (int)ret;
}

int access(const char* path, int mode) {
    long ret = __syscall2(SYS_ACCESS, (long)path, mode);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (int)ret;
}

int nanosleep(const struct timespec* req, struct timespec* rem) {
    long ret = __syscall2(SYS_NANOSLEEP, (long)req, (long)rem);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (int)ret;
}

int uname(struct utsname* buf) {
    long ret = __syscall1(SYS_UNAME, (long)buf);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (int)ret;
}

void sync(void) {
    __syscall0(SYS_SYNC);
}

/* ---- secure_boot ---- */
int unshare(int flags) {
    long ret = __syscall1(SYS_UNSHARE, flags);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (int)ret;
}

int secure_boot(int cmd, unsigned long arg) {
    long ret = __syscall2(SYS_SECURE_BOOT, cmd, arg);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (int)ret;
}

/* ---- futex ---- */
long futex(int* uaddr, int op, int val,
           const struct timespec* timeout,
           int* uaddr2, int val3)
{
    return __syscall6(SYS_FUTEX, (long)uaddr, op, val,
                      (long)timeout, (long)uaddr2, val3);
}

/* ---- epoll ---- */
int epoll_create1(int flags) {
    long ret = __syscall1(SYS_EPOLL_CREATE1, flags);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (int)ret;
}

int epoll_ctl(int epfd, int op, int fd, struct epoll_event* event) {
    long ret = __syscall5(SYS_EPOLL_CTL, epfd, op, fd, (long)event, 0);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (int)ret;
}

int epoll_wait(int epfd, struct epoll_event* events, int maxevents, int timeout) {
    long ret = __syscall5(SYS_EPOLL_WAIT, epfd, (long)events, maxevents, timeout, 0);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (int)ret;
}

int shm_open(const char* name, int oflag, mode_t mode) {
    long ret = __syscall3(SYS_SHM_OPEN, (long)name, oflag, mode);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (int)ret;
}

int shm_unlink(const char* name) {
    long ret = __syscall1(SYS_SHM_UNLINK, (long)name);
    if (ret < 0) { errno = (int)(-ret); return -1; }
    return (int)ret;
}

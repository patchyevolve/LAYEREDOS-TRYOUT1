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

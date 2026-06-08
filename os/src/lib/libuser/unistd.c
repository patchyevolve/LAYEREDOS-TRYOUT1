#include <unistd.h>
#include <errno.h>
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

#include "user.h"
#include "syscall_defs.h"

void _exit(int code) {
    syscall1(SYS_EXIT, code);
    for (;;);
}

int putchar(int c) {
    char ch = (char)c;
    long ret = syscall3(SYS_WRITE, 1, (long)&ch, 1);
    return (int)ret;
}

int puts(const char* s) {
    long n = 0;
    while (s[n]) n++;
    syscall3(SYS_WRITE, 1, (long)s, n);
    syscall3(SYS_WRITE, 1, (long)"\n", 1);
    return (int)(n + 1);
}

long write(int fd, const void* buf, unsigned long count) {
    return syscall3(SYS_WRITE, fd, (long)buf, count);
}

long read(int fd, void* buf, unsigned long count) {
    return syscall3(SYS_READ, fd, (long)buf, count);
}

long open(const char* path, int flags) {
    return syscall2(SYS_OPEN, (long)path, flags);
}

long close(int fd) {
    return syscall1(SYS_CLOSE, fd);
}

long readfile(int fd, void* buf, unsigned long count) {
    return syscall3(SYS_READFILE, fd, (long)buf, count);
}

long writefile(int fd, const void* buf, unsigned long count) {
    return syscall3(SYS_WRITEFILE, fd, (long)buf, count);
}

long execve(const char* path) {
    return syscall1(SYS_EXECVE, (long)path);
}

long fork(void) {
    return syscall0(SYS_FORK);
}

long waitpid(long pid, int* status) {
    return syscall2(SYS_WAITPID, pid, (long)status);
}

long getpid(void) {
    return syscall0(SYS_GETPID);
}

long getppid(void) {
    return syscall0(SYS_GETPPID);
}

long sbrk(long increment) {
    return syscall1(SYS_SBRK, increment);
}

void yield(void) {
    syscall0(SYS_YIELD);
}

void sleep_ms(unsigned long ms) {
    syscall1(SYS_SLEEP, ms);
}

unsigned long uptime_ms(void) {
    return (unsigned long)syscall0(SYS_UPTIME);
}

void reboot(void) {
    syscall0(SYS_REBOOT);
}

void poweroff(void) {
    syscall0(SYS_PWRDOWN);
}

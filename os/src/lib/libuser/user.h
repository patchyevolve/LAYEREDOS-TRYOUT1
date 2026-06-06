#ifndef _USER_H
#define _USER_H

typedef unsigned long size_t;
typedef signed long ssize_t;
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;

#ifdef __cplusplus
extern "C" {
#endif

static inline long syscall0(long n) {
    long ret;
    asm volatile("int $0x80" : "=a"(ret) : "a"(n) : "memory");
    return ret;
}

static inline long syscall1(long n, long a1) {
    long ret;
    asm volatile("int $0x80" : "=a"(ret) : "a"(n), "D"(a1) : "memory");
    return ret;
}

static inline long syscall2(long n, long a1, long a2) {
    long ret;
    asm volatile("int $0x80" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2) : "memory");
    return ret;
}

static inline long syscall3(long n, long a1, long a2, long a3) {
    long ret;
    asm volatile("int $0x80" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2), "d"(a3) : "memory");
    return ret;
}

void _exit(int code);
int putchar(int c);
int puts(const char* s);
long write(int fd, const void* buf, unsigned long count);
long read(int fd, void* buf, unsigned long count);
long open(const char* path, int flags);
long close(int fd);
long readfile(int fd, void* buf, unsigned long count);
long writefile(int fd, const void* buf, unsigned long count);
long execve(const char* path);
long fork(void);
long waitpid(long pid, int* status);
long getpid(void);
long getppid(void);
long sbrk(long increment);
void yield(void);
void sleep_ms(unsigned long ms);
unsigned long uptime_ms(void);
void reboot(void);
void poweroff(void);

#ifdef __cplusplus
}
#endif

#endif

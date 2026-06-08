#ifndef _LIBC_SYSCALL_H
#define _LIBC_SYSCALL_H

#include "syscall_defs.h"

static inline long __syscall0(long n) {
    long ret;
    asm volatile("int $0x80" : "=a"(ret) : "a"(n) : "memory");
    return ret;
}

static inline long __syscall1(long n, long a1) {
    long ret;
    asm volatile("int $0x80" : "=a"(ret) : "a"(n), "D"(a1) : "memory");
    return ret;
}

static inline long __syscall2(long n, long a1, long a2) {
    long ret;
    asm volatile("int $0x80" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2) : "memory");
    return ret;
}

static inline long __syscall3(long n, long a1, long a2, long a3) {
    long ret;
    asm volatile("int $0x80" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2), "d"(a3) : "memory");
    return ret;
}

static inline long __syscall6(long n, long a1, long a2, long a3, long a4, long a5, long a6) {
    long ret;
    register long r10 asm("r10") = a4;
    register long r8  asm("r8")  = a5;
    register long r9  asm("r9")  = a6;
    asm volatile("int $0x80" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9) : "memory");
    return ret;
}

#endif

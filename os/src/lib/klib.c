#include "kernel.h"

#define UART_LSR 0x3FD
#define UART_THR 0x3F8

static char hexdigits[] = "0123456789ABCDEF";

static inline void outb(uint16_t port, uint8_t val) {
    asm volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

void kputchar(char c) {
    if (c == '\n') {
        while (!(inb(UART_LSR) & 0x20));
        outb(UART_THR, '\r');
    }
    while (!(inb(UART_LSR) & 0x20));
    outb(UART_THR, c);
}

void kputs(const char* s) {
    while (*s) { kputchar(*s++); }
}

static void kprint_int64(int64_t v, int base, int pad) {
    char buf[24];
    int neg = 0;
    int pos = 0;
    if (base == 10 && v < 0) { neg = 1; v = -v; }
    uint64_t uv = (uint64_t)v;
    if (uv == 0) { buf[pos++] = '0'; }
    while (uv > 0) {
        buf[pos++] = hexdigits[uv % base];
        uv /= base;
    }
    while (pos < pad) buf[pos++] = '0';
    if (neg) buf[pos++] = '-';
    for (int i = pos - 1; i >= 0; i--) kputchar(buf[i]);
}

void kputhex(uint64_t v) {
    kputs("0x");
    for (int i = 60; i >= 0; i -= 4) {
        kputchar(hexdigits[(v >> i) & 0xF]);
    }
}

void kputdec(uint64_t v, int pad) {
    kprint_int64((int64_t)v, 10, pad);
}

void kprintf(const char* fmt, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    for (const char* p = fmt; *p; p++) {
        if (*p != '%') { kputchar(*p); continue; }
        p++;
        int pad = 0;
        if (*p >= '0' && *p <= '9') {
            while (*p >= '0' && *p <= '9') { pad = pad * 10 + (*p - '0'); p++; }
        }
        int lc = 0;
        while (*p == 'l') { lc++; p++; }
        if (*p == 'l') { lc++; p++; }
        if (*p == '%') { kputchar('%'); continue; }
        switch (*p) {
            case 'd': {
                int64_t v;
                if (lc >= 2) v = __builtin_va_arg(ap, long long);
                else if (lc == 1) v = __builtin_va_arg(ap, long);
                else v = __builtin_va_arg(ap, int);
                kprint_int64(v, 10, pad);
                break;
            }
            case 'u': {
                uint64_t v;
                if (lc >= 2) v = __builtin_va_arg(ap, unsigned long long);
                else if (lc == 1) v = __builtin_va_arg(ap, unsigned long);
                else v = __builtin_va_arg(ap, unsigned int);
                kprint_int64((int64_t)v, 10, pad);
                break;
            }
            case 'x': {
                uint64_t v;
                if (lc >= 2) v = __builtin_va_arg(ap, unsigned long long);
                else if (lc == 1) v = __builtin_va_arg(ap, unsigned long);
                else v = __builtin_va_arg(ap, unsigned int);
                kprintf("0x");
                int shift = 60;
                while (shift > 0 && !((v >> shift) & 0xF)) shift -= 4;
                for (int i = shift; i >= 0; i -= 4)
                    kputchar(hexdigits[(v >> i) & 0xF]);
                break;
            }
            case 'p': {
                void* pv = __builtin_va_arg(ap, void*);
                kputhex((uint64_t)pv);
                break;
            }
            case 's': {
                const char* s = __builtin_va_arg(ap, const char*);
                kputs(s ? s : "(null)");
                break;
            }
            case 'c': {
                int c = __builtin_va_arg(ap, int);
                kputchar(c);
                break;
            }
            default:
                kputchar('%');
                kputchar(*p);
                break;
        }
    }
    __builtin_va_end(ap);
}

void kassert_fail(const char* expr, const char* file, int line, const char* func) {
    kprintf("\n*** ASSERTION FAILED ***\n");
    kprintf("  Expression: %s\n", expr);
    kprintf("  File:       %s\n", file);
    kprintf("  Line:       %d\n", line);
    kprintf("  Function:   %s\n", func);
    kpanic("Assertion failed");
}

void kpanic(const char* msg, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, msg);
    kprintf("\n====== KERNEL PANIC ======\n");
    kprintf("  Message: ");
    for (const char* p = msg; *p; p++) {
        if (*p != '%') { kputchar(*p); continue; }
        p++;
        switch (*p) {
            case 's': kputs(__builtin_va_arg(ap, const char*)); break;
            case 'x': kputhex(__builtin_va_arg(ap, uint64_t)); break;
            case 'd': kputdec(__builtin_va_arg(ap, uint64_t), 0); break;
            default: kputchar(*p); break;
        }
    }
    kprintf("\n==========================\n");
    __builtin_va_end(ap);
    for (;;) { asm volatile("cli; hlt"); }
}

size_t kstrlen(const char* s) {
    size_t n = 0;
    while (*s) { n++; s++; }
    return n;
}

int kstrcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int kstrncmp(const char* a, const char* b, size_t n) {
    while (n > 0 && *a && *a == *b) { a++; b++; n--; }
    if (n == 0) return 0;
    return (unsigned char)*a - (unsigned char)*b;
}

char* kstrcpy(char* d, const char* s) {
    char* r = d;
    while (*s) { *d++ = *s++; }
    *d = 0;
    return r;
}

char* kstrncpy(char* d, const char* s, size_t n) {
    char* r = d;
    while (n > 0 && *s) { *d++ = *s++; n--; }
    while (n > 0) { *d++ = 0; n--; }
    return r;
}

void* kmemset(void* d, int c, size_t n) {
    unsigned char* p = (unsigned char*)d;
    while (n--) *p++ = (unsigned char)c;
    return d;
}

void* kmemcpy(void* d, const void* s, size_t n) {
    unsigned char* dp = (unsigned char*)d;
    const unsigned char* sp = (const unsigned char*)s;
    while (n--) *dp++ = *sp++;
    return d;
}

int kmemcmp(const void* a, const void* b, size_t n) {
    const unsigned char* pa = (const unsigned char*)a;
    const unsigned char* pb = (const unsigned char*)b;
    while (n--) {
        if (*pa != *pb) return *pa - *pb;
        pa++; pb++;
    }
    return 0;
}

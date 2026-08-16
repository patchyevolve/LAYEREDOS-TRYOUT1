#include "kernel.h"
#include "sync.h"

#define UART_LSR 0x3FD
#define UART_THR 0x3F8
#define VGA_ADDR   0xB8000
#define VGA_COLS   80
#define VGA_ROWS   25

static char hexdigits[] = "0123456789ABCDEF";
#define INT64_MIN (-9223372036854775807LL - 1LL)

static volatile uint16_t* const vga_buf = (volatile uint16_t*)(KERNEL_VMA_BASE + VGA_ADDR);
static int vga_row = 0;
static int vga_col = 0;
static spinlock_t kputchar_lock = { .lock = 0, .name = "kputchar", .holder = 0 };

static inline void outb(uint16_t port, uint8_t val) {
    asm volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static void vga_scroll(void) {
    for (int i = 0; i < (VGA_ROWS - 1) * VGA_COLS; i++)
        vga_buf[i] = vga_buf[i + VGA_COLS];
    for (int i = (VGA_ROWS - 1) * VGA_COLS; i < VGA_ROWS * VGA_COLS; i++)
        vga_buf[i] = (uint16_t)' ' | (0x07 << 8);
    vga_row = VGA_ROWS - 1;
}

static void vga_update_cursor(void) {
    uint16_t pos = vga_row * VGA_COLS + vga_col;
    outb(0x3D4, 14);
    outb(0x3D5, (uint8_t)(pos >> 8));
    outb(0x3D4, 15);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
}

/* Optional kmsg buffer; linked when kmsg.c is compiled in */
extern void kmsg_putchar(char c) __attribute__((weak));

void kputchar(char c) {
    /* Buffer into kmsg ring first (no lock needed — ring buffer) */
    if (kmsg_putchar) kmsg_putchar(c);

    /* Serial output — best-effort via try_acquire to avoid deadlock if
     * kputchar is re-entered (e.g. page fault during VGA output). */
    {
        cpu_flags_t kpflags;
        if (spinlock_try_acquire(&kputchar_lock, &kpflags)) {
            if (c == '\n') {
                while (!(inb(UART_LSR) & 0x20));
                outb(UART_THR, '\r');
            }
            while (!(inb(UART_LSR) & 0x20));
            outb(UART_THR, c);
            spinlock_release(&kputchar_lock, kpflags);
        } else {
            /* Contended: fallback to raw CLI/STI serial write (no VGA).
             * This ensures panic/fault output appears even during a
             * concurrent kprintf from another CPU. */
            unsigned long _kpflags;
            asm volatile("pushfq; popq %0; cli" : "=r"(_kpflags));
            if (c == '\n') {
                while (!(inb(UART_LSR) & 0x20));
                outb(UART_THR, '\r');
            }
            while (!(inb(UART_LSR) & 0x20));
            outb(UART_THR, c);
            if (_kpflags & 0x200) asm volatile("sti");
        }
    }

    /* VGA output — only when the lock is available (no contention).
     * Contention means another CPU is currently writing to VGA, so
     * our update would be lost or corrupting anyway.  Skipping VGA
     * in that case prevents vga_row/vga_col data races that lead to
     * out-of-bounds VGA buffer writes on SMP. */
    {
        cpu_flags_t vflags;
        if (spinlock_try_acquire(&kputchar_lock, &vflags)) {
            if (c == '\n') { vga_col = 0; vga_row++; }
            else if (c == '\r') { vga_col = 0; }
            else if (c == '\b' || c == 127) { if (vga_col > 0) vga_col--; }
            else if (c >= ' ') {
                vga_buf[vga_row * VGA_COLS + vga_col] = (uint16_t)c | (0x07 << 8);
                vga_col++;
            }
            if (vga_col >= VGA_COLS) { vga_col = 0; vga_row++; }
            if (vga_row >= VGA_ROWS) vga_scroll();
            vga_update_cursor();
            spinlock_release(&kputchar_lock, vflags);
        }
    }
}

void kputs(const char* s) {
    while (*s) { kputchar(*s++); }
}

static void kprint_int64(int64_t v, int base, int pad) {
    char buf[24];
    int neg = 0;
    int pos = 0;
    if (base == 10 && v < 0) {
        if (v == INT64_MIN) {
            kputs("-9223372036854775808");
            return;
        }
        neg = 1; v = -v;
    }
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

static void kprint_uint64(uint64_t v, int pad) {
    char buf[24];
    int pos = 0;
    if (v == 0) { buf[pos++] = '0'; }
    while (v > 0) {
        buf[pos++] = '0' + (v % 10);
        v /= 10;
    }
    while (pos < pad) buf[pos++] = '0';
    for (int i = pos - 1; i >= 0; i--) kputchar(buf[i]);
}

void kputhex(uint64_t v) {
    kputs("0x");
    for (int i = 60; i >= 0; i -= 4) {
        kputchar(hexdigits[(v >> i) & 0xF]);
    }
}

static void kputhex_trim(uint64_t v) {
    kputs("0x");
    int shift = 60;
    while (shift > 0 && !((v >> shift) & 0xF)) shift -= 4;
    for (int i = shift; i >= 0; i -= 4)
        kputchar(hexdigits[(v >> i) & 0xF]);
}

void kputdec(uint64_t v, int pad) {
    kprint_uint64(v, pad);
}

int kvsnprintf(char* buf, size_t size, const char* fmt, __builtin_va_list ap) {
    if (!buf || size == 0) return 0;
    int written = 0;
    for (const char* p = fmt; *p; p++) {
        if (written >= (int)size - 1) break;
        if (*p != '%') { buf[written++] = *p; continue; }
        p++;
        int left = 0;
        if (*p == '-') { left = 1; p++; }
        int pad = 0;
        while (*p >= '0' && *p <= '9') { pad = pad * 10 + (*p - '0'); p++; }
        int lc = 0;
        while (*p == 'l') { lc++; p++; }
        char tmp[64];
        int ti = 0;
        int len;
        if (*p == '%') { buf[written++] = '%'; continue; }
        switch (*p) {
            case 'd': {
                int64_t v;
                if (lc >= 2) v = __builtin_va_arg(ap, long long);
                else if (lc == 1) v = __builtin_va_arg(ap, long);
                else v = __builtin_va_arg(ap, int);
                uint64_t uv;
                int neg = 0;
                if (v < 0) {
                    if ((uint64_t)v == 0x8000000000000000ULL) { uv = 0x8000000000000000ULL; neg = 1; }
                    else { neg = 1; uv = (uint64_t)(-v); }
                } else { uv = (uint64_t)v; }
                do { tmp[ti++] = '0' + (uv % 10); uv /= 10; } while (uv > 0);
                if (neg) tmp[ti++] = '-';
                len = ti;
                for (int i = len - 1; i >= 0 && written < (int)size - 1; i--)
                    buf[written++] = tmp[i];
                while (left && written < pad + len - (neg ? 1 : 0) && written < (int)size - 1)
                    buf[written++] = ' ';
                break;
            }
            case 'u': {
                uint64_t v;
                if (lc >= 2) v = __builtin_va_arg(ap, unsigned long long);
                else if (lc == 1) v = __builtin_va_arg(ap, unsigned long);
                else v = __builtin_va_arg(ap, unsigned int);
                do { tmp[ti++] = '0' + (v % 10); v /= 10; } while (v > 0);
                len = ti;
                for (int i = len - 1; i >= 0 && written < (int)size - 1; i--)
                    buf[written++] = tmp[i];
                while (left && written < pad + len && written < (int)size - 1)
                    buf[written++] = ' ';
                break;
            }
            case 'x': {
                uint64_t v;
                if (lc >= 2) v = __builtin_va_arg(ap, unsigned long long);
                else if (lc == 1) v = __builtin_va_arg(ap, unsigned long);
                else v = __builtin_va_arg(ap, unsigned int);
                if (written < (int)size - 1) buf[written++] = '0';
                if (written < (int)size - 1) buf[written++] = 'x';
                for (int i = 60; i >= 0 && written < (int)size - 1; i -= 4) {
                    int nib = (v >> i) & 0xF;
                    if (nib || i == 0) 
                        buf[written++] = hexdigits[nib];
                }
                break;
            }
            case 's': {
                const char* s = __builtin_va_arg(ap, const char*);
                if (!s) s = "(null)";
                len = 0;
                while (s[len]) len++;
                if (!left)
                    while (len < pad && written < (int)size - 1) { buf[written++] = ' '; pad--; }
                while (*s && written < (int)size - 1) buf[written++] = *s++;
                while (left && pad > 0 && written < (int)size - 1) { buf[written++] = ' '; pad--; }
                break;
            }
            case 'c': {
                int c = __builtin_va_arg(ap, int);
                buf[written++] = (char)c;
                break;
            }
            default:
                buf[written++] = '%';
                if (*p && written < (int)size - 1) buf[written++] = *p;
                break;
        }
    }
    buf[written] = '\0';
    return written;
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
                kprint_uint64(v, pad);
                break;
            }
            case 'x': {
                uint64_t v;
                if (lc >= 2) v = __builtin_va_arg(ap, unsigned long long);
                else if (lc == 1) v = __builtin_va_arg(ap, unsigned long);
                else v = __builtin_va_arg(ap, unsigned int);
                kputhex_trim(v);
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

static void kdump_stack(void) {
    kprintf("\n====== STACK TRACE ======\n");
    uint64_t* rbp;
    __asm__ volatile ("mov %%rbp, %0" : "=r"(rbp));
    int depth = 0;
    while (rbp && depth < 32) {
        /* Validate frame pointer before dereferencing:
         * must be in kernel virtual address space and 8-byte aligned */
        if ((uint64_t)rbp < 0xFFFFFFFFC0000000ULL ||
            ((uint64_t)rbp) & 7) {
            kprintf("  [%d] <corrupted frame pointer %p>\n", depth, (void*)rbp);
            break;
        }
        uint64_t rip = rbp[1];
        /* Filter obvious garbage: return address should be in kernel space */
        if (rip >= 0xFFFFFFFFC0000000ULL && rip < 0xFFFFFFFFE0000000ULL)
            kprintf("  [%d] %p\n", depth, (void*)rip);
        else
            kprintf("  [%d] %p (spurious)\n", depth, (void*)rip);
        rbp = (uint64_t*)rbp[0];
        depth++;
    }
    kprintf("=========================\n");
}

/* Optional panic recovery hooks */
extern void kmsg_dump(void) __attribute__((weak));
extern void emergency_sync(void) __attribute__((weak));
extern void panic_reboot(void) __attribute__((weak));

void kpanic(const char* msg, ...) {
    /* Re-entrancy guard: if we fault while panicking, just halt */
    static volatile int panicking = 0;
    if (__sync_lock_test_and_set(&panicking, 1)) {
        (void)msg;
        for (;;) { asm volatile("cli; hlt"); }
    }

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
            case 'd': kprint_int64(__builtin_va_arg(ap, int64_t), 10, 0); break;
            default: kputchar(*p); break;
        }
    }
    __builtin_va_end(ap);
    kdump_stack();
    kprintf("\n==========================\n");

    /* Panic recovery: dump recent log, sync filesystems, reboot */
    if (kmsg_dump) kmsg_dump();
    if (emergency_sync) emergency_sync();
    if (panic_reboot) panic_reboot();

    for (;;) { asm volatile("cli; hlt"); }
}

int kstrcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int kstrncmp(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (a[i] == '\0') return 0;
    }
    return 0;
}

char* kstrncat(char* d, const char* s, size_t n) {
    char* ret = d;
    while (*d) d++;
    while (n-- > 0 && *s) { *d++ = *s++; }
    *d = '\0';
    return ret;
}

const char* kstrstr(const char* haystack, const char* needle) {
    if (!*needle) return haystack;
    for (; *haystack; haystack++) {
        const char* h = haystack;
        const char* n = needle;
        while (*n && *h == *n) { h++; n++; }
        if (!*n) return haystack;
    }
    return NULL;
}

int kmemcmp(const void* a, const void* b, size_t n) {
    const unsigned char* pa = (const unsigned char*)a;
    const unsigned char* pb = (const unsigned char*)b;
    for (size_t i = 0; i < n; i++) {
        if (pa[i] != pb[i]) return (int)pa[i] - (int)pb[i];
    }
    return 0;
}

size_t kstrlen(const char* s) {
    size_t n = 0;
    while (*s++) n++;
    return n;
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



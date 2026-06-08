#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdarg.h>

int putchar(int c) {
    char ch = (char)c;
    long ret = write(STDOUT_FILENO, &ch, 1);
    return (int)ret;
}

int puts(const char* s) {
    size_t len = strlen(s);
    write(STDOUT_FILENO, s, len);
    write(STDOUT_FILENO, "\n", 1);
    return (int)(len + 1);
}

int getchar(void) {
    char c;
    long ret = read(STDIN_FILENO, &c, 1);
    if (ret <= 0) return EOF;
    return (unsigned char)c;
}

typedef struct {
    char* buf;
    size_t pos;
    size_t max;
} fmt_state_t;

static void fmt_putc(char c, fmt_state_t* st) {
    if (st->pos < st->max) st->buf[st->pos] = c;
    st->pos++;
}

static void fmt_puts(const char* s, fmt_state_t* st) {
    while (*s) fmt_putc(*s++, st);
}

static void fmt_pad(fmt_state_t* st, char pad, int n) {
    if (n <= 0) return;
    while (n-- > 0) fmt_putc(pad, st);
}

static int vsnprintf_impl(char* buf, size_t n, const char* fmt, va_list ap) {
    fmt_state_t state;
    state.buf = buf;
    state.pos = 0;
    state.max = n ? n - 1 : 0;

    while (*fmt) {
        if (*fmt != '%') {
            fmt_putc(*fmt, &state);
            fmt++;
            continue;
        }
        fmt++;

        int width = 0;
        int zero_pad = 0;
        int left_justify = 0;

        if (*fmt == '-') { left_justify = 1; fmt++; }
        if (*fmt == '0') { zero_pad = 1; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') {
            if (width > 65536) { fmt++; continue; }
            width = width * 10 + (*fmt - '0');
            fmt++;
        }
        if (width > 65536) width = 65536;

        if (*fmt == 'l') {
            fmt++;
            int is_long = 1;
            if (*fmt == 'l') { fmt++; is_long = 2; }
            if (*fmt == 'd' || *fmt == 'i') {
                unsigned char neg = 0;
                long long v;
                if (is_long == 2) v = va_arg(ap, long long);
                else v = va_arg(ap, long);
                unsigned long long uv;
                if (v < 0) { neg = 1; uv = (unsigned long long)(-(v + 1)) + 1; }
                else { uv = (unsigned long long)v; }
                char tmp[32];
                int len = 0;
                if (uv == 0) tmp[len++] = '0';
                while (uv > 0) { tmp[len++] = (char)('0' + uv % 10); uv /= 10; }
                int num_len = len + (int)neg;
                char pad_char = zero_pad ? '0' : ' ';
                if (!left_justify) fmt_pad(&state, pad_char, width > num_len ? width - num_len : 0);
                if (neg) fmt_putc('-', &state);
                while (len > 0) fmt_putc(tmp[--len], &state);
                if (left_justify) fmt_pad(&state, ' ', width > num_len ? width - num_len : 0);
                fmt++;
            } else if (*fmt == 'u') {
                unsigned long long v;
                if (is_long == 2) v = va_arg(ap, unsigned long long);
                else v = va_arg(ap, unsigned long);
                char tmp[32];
                int len = 0;
                if (v == 0) tmp[len++] = '0';
                while (v > 0) { tmp[len++] = (char)('0' + v % 10); v /= 10; }
                char pad_char = zero_pad ? '0' : ' ';
                if (!left_justify) fmt_pad(&state, pad_char, width > len ? width - len : 0);
                while (len > 0) fmt_putc(tmp[--len], &state);
                if (left_justify) fmt_pad(&state, ' ', width > len ? width - len : 0);
                fmt++;
            } else if (*fmt == 'x' || *fmt == 'X') {
                unsigned long long v;
                if (is_long == 2) v = va_arg(ap, unsigned long long);
                else v = va_arg(ap, unsigned long);
                char tmp[32];
                int len = 0;
                if (v == 0) tmp[len++] = '0';
                while (v > 0) {
                    int d = (int)(v % 16);
                    tmp[len++] = (char)(d < 10 ? '0' + d : (*fmt == 'X' ? 'A' : 'a') + d - 10);
                    v /= 16;
                }
                char pad_char = zero_pad ? '0' : ' ';
                if (!left_justify) fmt_pad(&state, pad_char, width > len ? width - len : 0);
                while (len > 0) fmt_putc(tmp[--len], &state);
                if (left_justify) fmt_pad(&state, ' ', width > len ? width - len : 0);
                fmt++;
            } else {
                fmt_putc('%', &state);
                if (is_long) fmt_putc('l', &state);
                if (is_long == 2) fmt_putc('l', &state);
                fmt_putc(*fmt, &state);
                fmt++;
            }
        } else if (*fmt == 's') {
            const char* s = va_arg(ap, const char*);
            if (!s) s = "(null)";
            size_t slen = strlen(s);
            if (!left_justify) fmt_pad(&state, ' ', (size_t)width > slen ? (size_t)width - slen : 0);
            fmt_puts(s, &state);
            if (left_justify) fmt_pad(&state, ' ', (size_t)width > slen ? (size_t)width - slen : 0);
            fmt++;
        } else if (*fmt == 'd' || *fmt == 'i') {
            int v = va_arg(ap, int);
            unsigned int uv;
            char tmp[32];
            int neg = 0;
            if (v < 0) { neg = 1; uv = (unsigned int)(-(v + 1)) + 1; }
            else { uv = (unsigned int)v; }
            int len = 0;
            if (uv == 0) tmp[len++] = '0';
            while (uv > 0) { tmp[len++] = (char)('0' + uv % 10); uv /= 10; }
            int num_len = len + neg;
            char pad_char = zero_pad ? '0' : ' ';
            if (!left_justify) fmt_pad(&state, pad_char, width > num_len ? width - num_len : 0);
            if (neg) fmt_putc('-', &state);
            while (len > 0) fmt_putc(tmp[--len], &state);
            if (left_justify) fmt_pad(&state, ' ', width > num_len ? width - num_len : 0);
            fmt++;
        } else if (*fmt == 'u') {
            unsigned int v = va_arg(ap, unsigned int);
            char tmp[32];
            int len = 0;
            if (v == 0) tmp[len++] = '0';
            while (v > 0) { tmp[len++] = (char)('0' + v % 10); v /= 10; }
            char pad_char = zero_pad ? '0' : ' ';
            if (!left_justify) fmt_pad(&state, pad_char, width > len ? width - len : 0);
            while (len > 0) fmt_putc(tmp[--len], &state);
            if (left_justify) fmt_pad(&state, ' ', width > len ? width - len : 0);
            fmt++;
        } else if (*fmt == 'x' || *fmt == 'X') {
            unsigned int v = va_arg(ap, unsigned int);
            char tmp[32];
            int len = 0;
            if (v == 0) tmp[len++] = '0';
            while (v > 0) {
                int d = v % 16;
                tmp[len++] = (char)(d < 10 ? '0' + d : (*fmt == 'X' ? 'A' : 'a') + d - 10);
                v /= 16;
            }
            char pad_char = zero_pad ? '0' : ' ';
            if (!left_justify) fmt_pad(&state, pad_char, width > len ? width - len : 0);
            while (len > 0) fmt_putc(tmp[--len], &state);
            if (left_justify) fmt_pad(&state, ' ', width > len ? width - len : 0);
            fmt++;
        } else if (*fmt == 'p') {
            void* p = va_arg(ap, void*);
            unsigned long v = (unsigned long)p;
            char tmp[32];
            int len = 0;
            if (v == 0) {
                fmt_puts("(nil)", &state);
                fmt++;
                continue;
            }
            while (v > 0) {
                int d = v % 16;
                tmp[len++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
                v /= 16;
            }
            int total = len + 2;
            if (!left_justify) fmt_pad(&state, ' ', width > total ? width - total : 0);
            fmt_putc('0', &state);
            fmt_putc('x', &state);
            while (len > 0) fmt_putc(tmp[--len], &state);
            if (left_justify) fmt_pad(&state, ' ', width > total ? width - total : 0);
            fmt++;
        } else if (*fmt == 'c') {
            char c = (char)va_arg(ap, int);
            if (!left_justify) fmt_pad(&state, ' ', width > 1 ? width - 1 : 0);
            fmt_putc(c, &state);
            if (left_justify) fmt_pad(&state, ' ', width > 1 ? width - 1 : 0);
            fmt++;
        } else if (*fmt == '%') {
            fmt_putc('%', &state);
            fmt++;
        } else {
            fmt_putc('%', &state);
            fmt_putc(*fmt, &state);
            fmt++;
        }
    }
    if (n > 0) state.buf[state.pos <= state.max ? state.pos : state.max] = '\0';
    return (int)state.pos;
}

int snprintf(char* buf, size_t n, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int ret = vsnprintf_impl(buf, n, fmt, ap);
    va_end(ap);
    return ret;
}

int printf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf_impl(NULL, 0, fmt, ap);
    va_end(ap);
    if (len <= 0 || (size_t)len > 0x7FFFFFFF) return len;

    char* buf = malloc((size_t)len + 1);
    if (!buf) return -1;

    va_start(ap, fmt);
    vsnprintf_impl(buf, (size_t)len + 1, fmt, ap);
    va_end(ap);

    write(STDOUT_FILENO, buf, (size_t)len);
    free(buf);
    return len;
}

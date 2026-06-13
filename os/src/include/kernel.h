#ifndef KERNEL_H
#define KERNEL_H

#include "types.h"
#include "errno.h"

extern uint64_t _text_start, _text_end, _rodata_start, _rodata_end;
extern uint64_t _data_start, _data_end, _bss_start, _bss_end, _kernel_end;

void kputchar(char c);
void kputs(const char* s);
void kprintf(const char* fmt, ...);
#ifdef NDEBUG
#define KDEBUG(...) ((void)0)
#else
#define KDEBUG(...) kprintf(__VA_ARGS__)
#endif
void kputhex(uint64_t v);
void kputdec(uint64_t v, int pad);

void kpanic(const char* msg, ...) __attribute__((noreturn));

int kstrcmp(const char* a, const char* b);
int kstrncmp(const char* a, const char* b, size_t n);
size_t kstrlen(const char* s);
char* kstrncpy(char* d, const char* s, size_t n);
char* kstrncat(char* d, const char* s, size_t n);
const char* kstrstr(const char* haystack, const char* needle);
int kmemcmp(const void* a, const void* b, size_t n);
void* kmemset(void* d, int c, size_t n);
void* kmemcpy(void* d, const void* s, size_t n);
int kvsnprintf(char* buf, size_t size, const char* fmt, __builtin_va_list ap);

int copy_from_user(void* dst, const void* src, size_t n);
int copy_to_user(void* dst, const void* src, size_t n);
int strncpy_from_user(void* dst, const void* src, size_t max);

#endif

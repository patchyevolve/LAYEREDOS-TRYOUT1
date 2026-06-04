#ifndef KERNEL_H
#define KERNEL_H

#include "types.h"
#include "errno.h"

extern uint64_t _text_start, _text_end, _rodata_start, _rodata_end;
extern uint64_t _data_start, _data_end, _bss_start, _bss_end, _kernel_end;

void kputchar(char c);
void kputs(const char* s);
void kprintf(const char* fmt, ...);
void kputhex(uint64_t v);
void kputdec(uint64_t v, int pad);

void kassert_fail(const char* expr, const char* file, int line, const char* func);
#define KASSERT(e) do { if (!(e)) kassert_fail(#e, __FILE__, __LINE__, __FUNCTION__); } while(0)
#define KLIKELY(x)   __builtin_expect(!!(x), 1)
#define KUNLIKELY(x) __builtin_expect(!!(x), 0)

void kpanic(const char* msg, ...) __attribute__((noreturn));

size_t kstrlen(const char* s);
int kstrcmp(const char* a, const char* b);
int kstrncmp(const char* a, const char* b, size_t n);
char* kstrcpy(char* d, const char* s);
char* kstrncpy(char* d, const char* s, size_t n);
void* kmemset(void* d, int c, size_t n);
void* kmemcpy(void* d, const void* s, size_t n);
int kmemcmp(const void* a, const void* b, size_t n);

#endif

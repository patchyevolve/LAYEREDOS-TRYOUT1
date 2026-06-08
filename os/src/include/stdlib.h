#ifndef _STDLIB_H
#define _STDLIB_H

#include <sys/types.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define NULL ((void*)0)

extern int atoi(const char* s);
extern long atol(const char* s);
extern int abs(int n);
extern int atexit(void (*func)(void));
extern void exit(int status);
extern void* malloc(size_t size);
extern void free(void* ptr);
extern void* calloc(size_t nmemb, size_t size);
extern void* realloc(void* ptr, size_t size);
extern void __libc_init(void);

#endif
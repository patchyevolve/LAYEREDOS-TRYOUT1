#ifndef _STRING_H
#define _STRING_H

#include <sys/types.h>
#include <stddef.h>

extern void* memset(void* s, int c, size_t n);
extern void* memcpy(void* dst, const void* src, size_t n);
extern void* memmove(void* dst, const void* src, size_t n);
extern int memcmp(const void* s1, const void* s2, size_t n);
extern size_t strlen(const char* s);
extern char* strcpy(char* dst, const char* src);
extern char* strncpy(char* dst, const char* src, size_t n);
extern int strcmp(const char* s1, const char* s2);
extern int strncmp(const char* s1, const char* s2, size_t n);
extern char* strcat(char* dst, const char* src);
extern char* strchr(const char* s, int c);
extern char* strstr(const char* haystack, const char* needle);
extern size_t strcspn(const char* s, const char* reject);
extern size_t strspn(const char* s, const char* accept);

#endif
#ifndef _STDIO_H
#define _STDIO_H

#include <sys/types.h>

#define EOF (-1)
#define BUFSIZ 8192
#define FILENAME_MAX 256

extern int printf(const char* fmt, ...);
extern int putchar(int c);
extern int puts(const char* s);
extern int getchar(void);
extern int snprintf(char* buf, size_t n, const char* fmt, ...);

#endif
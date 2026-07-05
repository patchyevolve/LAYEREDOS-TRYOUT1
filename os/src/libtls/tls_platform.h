#ifndef TLS_PLATFORM_H
#define TLS_PLATFORM_H

#include <sys/types.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#define TLS_MALLOC(sz)   kmalloc(sz)
#define TLS_FREE(p)      kfree(p)
#define TLS_MEMCPY       kmemcpy
#define TLS_MEMSET       kmemset
#define TLS_MEMMOVE      kmemmove
#define TLS_MEMCMP       kmemcmp

/* Use kernel mem* functions since libtls is compiled into the kernel's
 * user-program toolchain.  The kernel C library (libuser) does not have
 * its own memcpy/memset — it relies on the kernel's via builtins.
 */
static inline void* tls_malloc(size_t sz) { return kmalloc(sz); }
static inline void tls_free(void* p) { kfree(p); }

#endif /* TLS_PLATFORM_H */

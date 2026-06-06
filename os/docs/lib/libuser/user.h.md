# user.h — User-space Library Header

**Path:** `os/src/lib/libuser/user.h`  
**Layer:** User space — libc equivalent header

---

## Purpose

Declares all types, syscall helper macros, and function prototypes for
user-space programs.  This is the only header a user program needs to
include to access the full libuser API (other than `syscall_defs.h` for
raw syscall numbers in assembly files).

---

## Type aliases

Because user-space code cannot include the kernel's `types.h`, the common
fixed-width types are re-declared here using `unsigned long long` etc.:

```c
typedef unsigned long       size_t;
typedef signed long         ssize_t;
typedef unsigned char       uint8_t;
typedef unsigned short      uint16_t;
typedef unsigned int        uint32_t;
typedef unsigned long long  uint64_t;
```

---

## Syscall inline helpers

```c
static inline long syscall0(long n)
static inline long syscall1(long n, long a1)
static inline long syscall2(long n, long a1, long a2)
static inline long syscall3(long n, long a1, long a2, long a3)
```

Each inlines directly to an `int $0x80` instruction with the appropriate
register constraints.  The `"memory"` clobber prevents the compiler from
reordering memory operations across the syscall.

These are `static inline` so they are compiled into every translation unit
that includes `user.h` without requiring a library object.

**ABI:**
- `%rax` = syscall number (input) / return value (output)
- `%rdi` = arg1
- `%rsi` = arg2
- `%rdx` = arg3

---

## C++ compatibility

```c
#ifdef __cplusplus
extern "C" {
#endif
// ... all declarations ...
#ifdef __cplusplus
}
#endif
```

Allows linking libuser into C++ user programs.

---

## All declared functions

See `user.c.md` for implementations.  Every function declared here has a
corresponding implementation in `user.c`:

```c
void   _exit(int code);
int    putchar(int c);
int    puts(const char* s);
long   write(int fd, const void* buf, unsigned long count);
long   read(int fd, void* buf, unsigned long count);
long   open(const char* path, int flags);
long   close(int fd);
long   readfile(int fd, void* buf, unsigned long count);
long   writefile(int fd, const void* buf, unsigned long count);
long   execve(const char* path);
long   fork(void);
long   waitpid(long pid, int* status);
long   getpid(void);
long   getppid(void);
long   sbrk(long increment);
void   yield(void);
void   sleep_ms(unsigned long ms);
unsigned long uptime_ms(void);
void   reboot(void);
void   poweroff(void);
```

---

## Usage example

A minimal user program using libuser:

```c
#include "user.h"

int main(int argc, char** argv) {
    puts("Hello from C user space!");
    return 0;
}
```

Compile and link:
```bash
gcc -ffreestanding -fno-stack-protector -nostdlib -m64 -mno-red-zone \
    -c myprogram.c -o myprogram.o
gcc -ffreestanding -fno-stack-protector -nostdlib -m64 -mno-red-zone \
    -c src/lib/libuser/user.c -o user.o
gcc -ffreestanding -fno-stack-protector -nostdlib -m64 -mno-red-zone \
    -c src/lib/libuser/crt0.S -o crt0.o
ld -nostdlib -Ttext=0x40000000 crt0.o user.o myprogram.o -o myprogram.elf
objcopy -O binary myprogram.elf myprogram.bin
```

#ifndef SYSCALL_DEFS_H
#define SYSCALL_DEFS_H

#ifdef __ASSEMBLER__
#define SYSCALL(n) n
#else
#define SYSCALL(n) n
#endif

#define SYS_EXIT       0
#define SYS_WRITE      1
#define SYS_READ       2
#define SYS_GETPID     3
#define SYS_SBRK       4
#define SYS_OPEN       5
#define SYS_CLOSE      6
#define SYS_READFILE   7
#define SYS_WRITEFILE  8
#define SYS_EXECVE     9
#define SYS_FORK      10
#define SYS_WAITPID   11
#define SYS_GETPPID   12
#define SYS_YIELD     13
#define SYS_SLEEP     14
#define SYS_UPTIME    15
#define SYS_REBOOT    16
#define SYS_PWRDOWN   17
#define SYS_CREATE    18
#define SYS_MKDIR     19
#define SYS_UNLINK    20
#define SYS_LSEEK     21
#define SYS_STAT      22
#define SYS_PIPE      23
#define SYS_KILL      24
#define SYS_SIGACTION 25
#define SYS_CLONE     26

#define SYSCALL_COUNT 27

#endif

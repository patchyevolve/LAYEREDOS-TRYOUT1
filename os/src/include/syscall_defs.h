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
#define SYS_SIGRETURN 27
#define SYS_GETCWD   28
#define SYS_CHDIR    29
#define SYS_DUP2     30
#define SYS_MMAP     31
#define SYS_MUNMAP   32
#define SYS_MPROTECT 33
#define SYS_IOCTL    34
#define SYS_SETPGID  35
#define SYS_GETPGID  36
#define SYS_PTY_PAIR 37
#define SYS_SOCKET   38
#define SYS_BIND     39
#define SYS_CONNECT  40
#define SYS_LISTEN   41
#define SYS_ACCEPT   42
#define SYS_SEND     43
#define SYS_RECV     44
#define SYS_SENDTO      45
#define SYS_RECVFROM    46
#define SYS_SETSOCKOPT  47
#define SYS_GETSOCKOPT  48
#define SYS_CLOCK_GETTIME 49
#define SYS_GETSOCKNAME   50
#define SYS_GETPEERNAME   51
#define SYS_POLL          52

#define SYS_FCNTL         53

#define SYSCALL_COUNT 54

#endif

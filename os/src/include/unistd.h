#ifndef _UNISTD_H
#define _UNISTD_H

#include <sys/types.h>

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

extern ssize_t read(int fd, void* buf, size_t count);
extern ssize_t write(int fd, const void* buf, size_t count);
extern int open(const char* path, int flags, ...);
extern int close(int fd);
extern pid_t fork(void);
extern pid_t getpid(void);
extern pid_t getppid(void);
extern int execve(const char* path, char* const argv[], char* const envp[]);
extern pid_t waitpid(pid_t pid, int* status, int options);
extern void _exit(int status);
extern int kill(pid_t pid, int sig);
extern void* sbrk(intptr_t increment);
extern off_t lseek(int fd, off_t offset, int whence);
extern int pipe(int fds[2]);
extern int dup2(int oldfd, int newfd);
extern int getcwd(char* buf, size_t size);
extern int chdir(const char* path);
extern unsigned int sleep(unsigned int seconds);

#endif
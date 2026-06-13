#ifndef _UNISTD_H
#define _UNISTD_H

#include <sys/types.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <signal.h>

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
extern int ioctl(int fd, unsigned long request, void* argp);
extern pid_t getpgid(pid_t pid);
extern int setpgid(pid_t pid, pid_t pgid);
extern int pty_pair(int fds[2]);

/* Socket API wrappers */
extern int socket(int domain, int type, int protocol);
extern int bind(int sockfd, const struct sockaddr* addr, socklen_t addrlen);
extern int connect(int sockfd, const struct sockaddr* addr, socklen_t addrlen);
extern int listen(int sockfd, int backlog);
extern int accept(int sockfd, struct sockaddr* addr, socklen_t* addrlen);
extern ssize_t send(int sockfd, const void* buf, size_t len, int flags);
extern ssize_t recv(int sockfd, void* buf, size_t len, int flags);
extern ssize_t sendto(int sockfd, const void* buf, size_t len, int flags,
                       const struct sockaddr* dest_addr, socklen_t addrlen);
extern ssize_t recvfrom(int sockfd, void* buf, size_t len, int flags,
                         struct sockaddr* src_addr, socklen_t* addrlen);
extern int setsockopt(int sockfd, int level, int optname,
                       const void* optval, socklen_t optlen);
extern int getsockopt(int sockfd, int level, int optname,
                       void* optval, socklen_t* optlen);

extern int clock_gettime(int clk_id, struct timespec* tp);
extern int getsockname(int sockfd, struct sockaddr* addr, socklen_t* addrlen);
extern int getpeername(int sockfd, struct sockaddr* addr, socklen_t* addrlen);

struct pollfd {
    int fd;
    short events;
    short revents;
};
extern int poll(struct pollfd* fds, int nfds, int timeout_ms);

/* Memory mapping */
extern void* mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset);
extern int munmap(void* addr, size_t length);
extern int mprotect(void* addr, size_t len, int prot);

/* Control operations on a file descriptor */
extern int fcntl(int fd, int cmd, long arg);

/* Signal handling */
extern int sigaction(int sig, const struct sigaction* act, struct sigaction* oldact);
extern void (*signal(int sig, void (*handler)(int)))(int);

/* Threading */
extern pid_t clone(int (*fn)(void*), void* stack, int flags, void* arg);

#endif
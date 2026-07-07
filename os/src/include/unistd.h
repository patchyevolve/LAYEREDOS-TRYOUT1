#ifndef _UNISTD_H
#define _UNISTD_H

#include <sys/types.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <signal.h>
#include <ip.h>

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

/* ---- Security / Capabilities ---- */

#define AUDIT_DATA_LEN 48

typedef struct {
    unsigned long timestamp_ms;
    int           event_type;
    unsigned long pid;
    char          data[AUDIT_DATA_LEN];
} audit_entry_t;

extern int capget(unsigned long pid, unsigned long* caps);
extern int capset(unsigned long caps);
extern int audit_read(audit_entry_t* buf, unsigned long count);

/* ---- UID/GID ---- */
extern uid_t getuid(void);
extern uid_t geteuid(void);
extern gid_t getgid(void);
extern gid_t getegid(void);
extern int setuid(uid_t uid);
extern int setgid(gid_t gid);

/* ---- Random ---- */
extern ssize_t getrandom(void* buf, size_t count, unsigned int flags);

/* ---- Syscall filter ---- */
extern int set_syscall_filter(const unsigned long long* masks, size_t count);

/* ---- Socketpair (AF_UNIX only) ---- */
extern int socketpair(int domain, int type, int protocol, int sv[2]);

/* ── ucred structure for SO_PEERCRED ── */
struct ucred {
    uid_t uid;
    gid_t gid;
    pid_t pid;
};

/* ---- prctl ---- */
#define PR_SET_NO_NEW_PRIVS 0
#define PR_GET_NO_NEW_PRIVS 1
extern int prctl(int option, unsigned long arg2, unsigned long arg3,
                 unsigned long arg4, unsigned long arg5);

/* ---- netconfig (IP/route/ARP configuration syscall) ---- */
#define NETCONFIG_SET_IPV4       1
#define NETCONFIG_ADD_ROUTE_V4   2
#define NETCONFIG_DEL_ROUTE_V4   3
#define NETCONFIG_SET_ARP        4
#define NETCONFIG_DEL_ARP        5
#define NETCONFIG_SET_IPV6       6
#define NETCONFIG_ADD_ROUTE_V6   7
#define NETCONFIG_DEL_ROUTE_V6   8
#define NETCONFIG_SET_NDP        9
#define NETCONFIG_DEL_NDP       10
#define NETCONFIG_GET_IPV4      11

typedef struct {
    int         op;
    ipv4_addr_t addr4;
    int         prefix_len;
    ipv4_addr_t gw4;
    uint8_t     mac[6];
    uint8_t     addr6[16];
    uint8_t     gw6[16];
} netconfig_req_t;

extern int netconfig(netconfig_req_t* req);
extern int veth_move(int pair_idx, int end_sel);

/* ---- Network namespace ---- */
#define CLONE_NEWNET  0x40000000
extern int unshare(int flags);
extern int veth_pair(netconfig_req_t* req);

/* ---- Secure boot ---- */
#define SECURE_BOOT_ENABLE  0  /* cmd for secure_boot(): enable/disable enforcement */
#define SECURE_BOOT_QUERY   1  /* cmd for secure_boot(): query current state */
extern int secure_boot(int cmd, unsigned long arg);

/* ---- Extra POSIX syscalls ---- */
extern int chmod(const char* path, unsigned int mode);
extern int link(const char* target, const char* linkpath);
extern int symlink(const char* target, const char* linkpath);
extern int readlink(const char* path, char* buf, size_t size);
extern int rmdir(const char* path);
extern int ftruncate(int fd, uint64_t size);
extern int dup(int oldfd);
extern int access(const char* path, int mode);
extern int nanosleep(const struct timespec* req, struct timespec* rem);

/* utsname for uname */
struct utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
};
extern int uname(struct utsname* buf);
extern void sync(void);

#endif
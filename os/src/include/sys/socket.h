#ifndef _SYS_SOCKET_H
#define _SYS_SOCKET_H

/* Standard POSIX values for user-space */
#define AF_UNSPEC   0
#define AF_UNIX     1
#define AF_INET     2
#define AF_INET6    10

#define SOCK_STREAM 1
#define SOCK_DGRAM  2

#define SOL_SOCKET   1
#define SOL_IP       0
#define SOL_IPV6     41
#define SOL_TCP      6
#define SO_REUSEADDR 2
#define SO_KEEPALIVE 9
#define SO_RCVTIMEO 20
#define SO_SNDTIMEO 21
#define IPV6_V6ONLY  26
#define IPV6_JOIN_GROUP  20
#define IPV6_LEAVE_GROUP 21
#define IP_ADD_MEMBERSHIP   12
#define IP_DROP_MEMBERSHIP  13
#define TCP_NODELAY  1
#define IPPROTO_TCP  6

/* poll/select (not in POSIX socket.h but useful) */
#define POLLIN      0x001
#define POLLOUT     0x004
#define POLLERR     0x008
#define POLLHUP     0x010
#define POLLNVAL    0x020

struct ipv6_mreq {
    unsigned char ipv6mr_multiaddr[16];
    int           ipv6mr_interface;
};

typedef unsigned int socklen_t;

struct timeval {
    long tv_sec;
    long tv_usec;
};

struct sockaddr_un {
    unsigned short sun_family;
    char           sun_path[108];
};

struct sockaddr {
    unsigned short sa_family;
    char sa_data[14];
};

struct sockaddr_in {
    unsigned short sin_family;
    unsigned short sin_port;
    unsigned int   sin_addr;
    char           sin_zero[8];
};

struct sockaddr_in6 {
    unsigned short sin6_family;
    unsigned short sin6_port;
    unsigned int   sin6_flowinfo;
    unsigned char  sin6_addr[16];
    unsigned int   sin6_scope_id;
};

/* Syscall numbers for socket operations (kernel-internal mapping) */
#define SYS_SOCKET   38
#define SYS_BIND     39
#define SYS_CONNECT  40
#define SYS_LISTEN   41
#define SYS_ACCEPT   42
#define SYS_SEND     43
#define SYS_RECV     44
#define SYS_SENDTO   45
#define SYS_RECVFROM 46
#define SYS_SETSOCKOPT 47
#define SYS_GETSOCKOPT 48

#endif

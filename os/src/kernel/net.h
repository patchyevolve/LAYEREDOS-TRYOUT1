#ifndef NET_H
#define NET_H

#include "types.h"
#include "ip.h"
#include "route.h"

/* BSD-compatible socket type constants */
#define SOCK_STREAM 1
#define SOCK_DGRAM  2
#define AF_UNIX     1
#define AF_UNSPEC   0

/* Socket options */
#define SOL_SOCKET   1
#define SOL_IP       0
#define SOL_IPV6     41
#define SOL_TCP      6
#define SO_REUSEADDR 2
#define SO_PEERCRED  17
#define SO_KEEPALIVE 9
#define SO_RCVTIMEO  20
#define SO_SNDTIMEO  21
#define IPV6_V6ONLY  26
#define IPV6_JOIN_GROUP  20
#define IPV6_LEAVE_GROUP 21
#define IP_ADD_MEMBERSHIP   12
#define IP_DROP_MEMBERSHIP  13
#define TCP_NODELAY  1
#define IPPROTO_TCP  6

/* poll/select events (BSD/POSIX compatible) */
#define POLLIN      0x001
#define POLLOUT     0x004
#define POLLERR     0x008
#define POLLHUP     0x010
#define POLLNVAL    0x020

/* ioctl requests for sockets */
#define FIONBIO      0x5421

/* Multicast group membership request */
typedef struct {
    uint8_t  ipv6mr_multiaddr[16];
    int      ipv6mr_interface;
} ipv6_mreq_t;

/* IPv4 group membership */
typedef struct {
    uint32_t imr_multiaddr;
    uint32_t imr_interface;
} ip_mreq_t;

/* Socket table size per namespace */
#define NET_MAX_SOCKETS 32

/* sockaddr_storage must be large enough for sockaddr_in6 */
#define SOCKADDR_MAX sizeof(sockaddr_in6_t)
typedef uint32_t socklen_t;

struct timeval {
    long tv_sec;
    long tv_usec;
};

/* Socket states */
#define SS_UNBOUND    0
#define SS_BOUND      1
#define SS_LISTENING  2
#define SS_CONNECTING 3
#define SS_CONNECTED  4
#define SS_CLOSED     5

/* Socket address structures (packed, same layout as POSIX) */
typedef struct __attribute__((packed)) {
    uint16_t sa_family;  /* AF_* */
    uint8_t  sa_data[14];
} sockaddr_t;

typedef struct __attribute__((packed)) {
    uint16_t sun_family;
    char     sun_path[108];
} sockaddr_un_t;

typedef struct __attribute__((packed)) {
    uint16_t sin_family;  /* AF_INET */
    uint16_t sin_port;    /* network byte order */
    uint8_t  sin_addr[4]; /* IPv4 address */
    uint8_t  sin_zero[8];
} sockaddr_in_t;

typedef struct __attribute__((packed)) {
    uint16_t sin6_family; /* AF_INET6 */
    uint16_t sin6_port;   /* network byte order */
    uint32_t sin6_flowinfo;
    uint8_t  sin6_addr[16];
    uint32_t sin6_scope_id;
} sockaddr_in6_t;

/* Forward declarations */
struct socket;

typedef int (*sock_op_bind_t)(struct socket* s, const sockaddr_t* addr, socklen_t len);
typedef int (*sock_op_connect_t)(struct socket* s, const sockaddr_t* addr, socklen_t len);
typedef int (*sock_op_listen_t)(struct socket* s, int backlog);
typedef struct socket* (*sock_op_accept_t)(struct socket* s, sockaddr_t* addr, socklen_t* len);
typedef int (*sock_op_send_t)(struct socket* s, const uint8_t* buf, uint32_t len);
typedef int (*sock_op_recv_t)(struct socket* s, uint8_t* buf, uint32_t size);
typedef int (*sock_op_close_t)(struct socket* s);
typedef int (*sock_op_sendto_t)(struct socket* s, const uint8_t* buf, uint32_t len,
                                 const sockaddr_t* dst_addr, socklen_t addrlen);
typedef int (*sock_op_recvfrom_t)(struct socket* s, uint8_t* buf, uint32_t size,
                                  sockaddr_t* src_addr, socklen_t* addrlen);
typedef int (*sock_op_setsockopt_t)(struct socket* s, int level, int optname,
                                     const void* optval, socklen_t optlen);
typedef int (*sock_op_getsockopt_t)(struct socket* s, int level, int optname,
                                     void* optval, socklen_t* optlen);

typedef int (*sock_op_getsockname_t)(struct socket* s, sockaddr_t* addr, socklen_t* len);
typedef int (*sock_op_getpeername_t)(struct socket* s, sockaddr_t* addr, socklen_t* len);
typedef int (*sock_op_ioctl_t)(struct socket* s, uint64_t request, void* argp);
typedef int (*sock_op_poll_t)(struct socket* s, int events, int* revents);

typedef struct sock_ops {
    sock_op_bind_t        bind;
    sock_op_connect_t     connect;
    sock_op_listen_t      listen;
    sock_op_accept_t      accept;
    sock_op_send_t        send;
    sock_op_recv_t        recv;
    sock_op_close_t       close;
    sock_op_sendto_t      sendto;
    sock_op_recvfrom_t    recvfrom;
    sock_op_setsockopt_t  setsockopt;
    sock_op_getsockopt_t  getsockopt;
    sock_op_getsockname_t getsockname;
    sock_op_getpeername_t getpeername;
    sock_op_ioctl_t       ioctl;
    sock_op_poll_t        poll;
} sock_ops_t;

typedef struct socket {
    int          refcount;
    int          family;    /* AF_INET or AF_INET6 */
    int          type;      /* SOCK_STREAM or SOCK_DGRAM */
    int          state;     /* SS_* */
    sock_ops_t*  ops;       /* protocol-specific operations */
    void*        proto;     /* points to tcp_conn_t or udp_endpoint_t */
    void*        owner;     /* process that owns this socket */
    int          recv_timeout; /* receive timeout (milliseconds) */
    int          send_timeout; /* send timeout (milliseconds) */
    int          ipv6only;  /* IPV6_V6ONLY flag (default 0 = dual-stack) */
    int          nonblock;  /* non-blocking I/O (O_NONBLOCK) */
    /* UDP connected address (for connect() + send/recv on DGRAM sockets) */
    uint8_t      udp_conn_addr[SOCKADDR_MAX];
    socklen_t    udp_conn_addrlen;
} socket_t;

/* ---- Socket API (called by syscalls, wraps transport layer) ---- */
socket_t* socket_alloc(int family, int type, int protocol);
void      socket_retain(socket_t* s);
void      socket_release(socket_t* s);

int  sock_bind(socket_t* s, const sockaddr_t* addr, socklen_t len);
int  sock_connect(socket_t* s, const sockaddr_t* addr, socklen_t len);
int  sock_listen(socket_t* s, int backlog);
socket_t* sock_accept(socket_t* s, sockaddr_t* addr, socklen_t* len);
int  sock_send(socket_t* s, const uint8_t* buf, uint32_t len);
int  sock_recv(socket_t* s, uint8_t* buf, uint32_t size);
int  sock_close(socket_t* s);

/* UDP-specific (connectionless) operations */
int  sock_sendto(socket_t* s, const uint8_t* buf, uint32_t len,
                 const sockaddr_t* dst_addr, socklen_t addrlen);
int  sock_recvfrom(socket_t* s, uint8_t* buf, uint32_t size,
                   sockaddr_t* src_addr, socklen_t* addrlen);

/* Internal: register/unregister socket in process table (returns fd) */
int  sock_register(socket_t* s);
socket_t* sock_lookup(int fd);
int  sock_unregister(int fd);

int  sock_setsockopt(socket_t* s, int level, int optname, const void* optval, socklen_t optlen);
int  sock_getsockopt(socket_t* s, int level, int optname, void* optval, socklen_t* optlen);
int  sock_getsockname(socket_t* s, sockaddr_t* addr, socklen_t* len);
int  sock_getpeername(socket_t* s, sockaddr_t* addr, socklen_t* len);
int  sock_ioctl(socket_t* s, uint64_t request, void* argp);
int  sock_poll(socket_t* s, int events, int* revents);

/* Init */
void net_init(void);

#endif

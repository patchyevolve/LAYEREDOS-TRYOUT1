#include "kernel.h"
#include "net.h"
#include "tcp.h"
#include "udp.h"
#include "ipv4.h"
#include "ipv6.h"
#include "igmp.h"
#include "unix.h"
#include "process.h"
#include "sched.h"
#include "kmalloc.h"
#include "net_ns.h"

#ifndef SOCKADDR_MAX
#define SOCKADDR_MAX sizeof(sockaddr_in6_t)
#endif

#define net_initialized (get_current_ns()->net_initialized)
#define net_sockets (get_current_ns()->net_sockets)
#define sockets_lock   (get_current_ns()->sockets_lock)

/* Translate kernel-internal AF_* to POSIX userspace AF_* values */
static int af_to_user(int af_kernel) {
    if (af_kernel == AF_INET) return 2;   /* POSIX AF_INET */
    if (af_kernel == AF_INET6) return 10; /* POSIX AF_INET6 */
    return af_kernel;                      /* AF_UNIX (1) is same */
}

/* ---- TCP socket operations ---- */

static int tcp_sock_bind(socket_t* s, const sockaddr_t* addr, socklen_t len) {
    tcp_conn_t* c = (tcp_conn_t*)s->proto;
    uint16_t port;

    if (s->family == AF_INET) {
        if (len < (socklen_t)sizeof(sockaddr_in_t)) return ERR_INVAL;
        const sockaddr_in_t* in = (const sockaddr_in_t*)addr;
        port = __builtin_bswap16(in->sin_port);
    } else {
        if (len < (socklen_t)sizeof(sockaddr_in6_t)) return ERR_INVAL;
        const sockaddr_in6_t* in6 = (const sockaddr_in6_t*)addr;
        port = __builtin_bswap16(in6->sin6_port);
    }

    int e = tcp_conn_bind(c, port, s->ipv6only);
    if (e == ERR_OK) s->state = SS_BOUND;
    return e;
}

static int tcp_sock_connect(socket_t* s, const sockaddr_t* addr, socklen_t len) {
    tcp_conn_t* c = (tcp_conn_t*)s->proto;
    if (!c) return ERR_INVAL;
    void* dst_ip;
    uint16_t port;

    if (s->family == AF_INET) {
        if (len < (socklen_t)sizeof(sockaddr_in_t)) return ERR_INVAL;
        const sockaddr_in_t* in = (const sockaddr_in_t*)addr;
        dst_ip = (void*)in->sin_addr;
        port = __builtin_bswap16(in->sin_port);
    } else {
        if (len < (socklen_t)sizeof(sockaddr_in6_t)) return ERR_INVAL;
        const sockaddr_in6_t* in6 = (const sockaddr_in6_t*)addr;
        dst_ip = (void*)in6->sin6_addr;
        port = __builtin_bswap16(in6->sin6_port);
    }

    c->send_timeout = s->send_timeout;
    if (s->nonblock) {
        if (s->state == SS_CONNECTING) {
            int st = tcp_conn_get_state(c);
            if (st == TCP_ESTABLISHED) { s->state = SS_CONNECTED; return ERR_OK; }
            if (st == TCP_CLOSED) { s->state = SS_UNBOUND; return ERR_CONNREFUSED; }
            return ERR_AGAIN;
        }
        s->state = SS_CONNECTING;
        int e = tcp_conn_connect(c, dst_ip, port, 0);
        if (e == ERR_OK) { s->state = SS_CONNECTED; return e; }
        return ERR_AGAIN;
    }
    s->state = SS_CONNECTING;
    int timeout = s->recv_timeout > 0 ? s->recv_timeout : (s->family == AF_INET6 ? 30000 : 5000);
    int e = tcp_conn_connect(c, dst_ip, port, timeout);
    if (e == ERR_OK) s->state = SS_CONNECTED;
    return e;
}

static int tcp_sock_listen(socket_t* s, int backlog) {
    (void)backlog;
    tcp_conn_t* c = (tcp_conn_t*)s->proto;
    int e = tcp_conn_listen(c, c->local_port);
    if (e == ERR_OK) s->state = SS_LISTENING;
    return e;
}

static socket_t* tcp_sock_accept(socket_t* s, sockaddr_t* addr, socklen_t* len) {
    tcp_conn_t* c = (tcp_conn_t*)s->proto;
    int timeout = s->nonblock ? 0 : 5000;
    tcp_conn_t* child = tcp_conn_accept(c, timeout);
    if (!child) return NULL;

    /* Create a socket wrapper for the child */
    socket_t* child_sock = socket_alloc(child->af, SOCK_STREAM, 0);
    if (!child_sock) { tcp_conn_destroy(child); return NULL; }

    /* Free the auto-allocated proto and replace with accepted child */
    tcp_conn_destroy((tcp_conn_t*)child_sock->proto);
    child_sock->proto = child;
    child_sock->state = SS_CONNECTED;

    /* Fill in peer address if caller wants it */
    if (addr && len) {
        if (child->af == AF_INET) {
            sockaddr_in_t* out = (sockaddr_in_t*)addr;
            socklen_t need = sizeof(sockaddr_in_t);
            if (*len < need) { tcp_conn_destroy(child); socket_release(child_sock); return NULL; }
            kmemset(out, 0, need);
            out->sin_family = af_to_user(AF_INET);
            out->sin_port = __builtin_bswap16(child->remote_port);
            kmemcpy(out->sin_addr, child->remote_ip.v4.bytes, 4);
            *len = need;
        } else {
            sockaddr_in6_t* out = (sockaddr_in6_t*)addr;
            socklen_t need = sizeof(sockaddr_in6_t);
            if (*len < need) { tcp_conn_destroy(child); socket_release(child_sock); return NULL; }
            kmemset(out, 0, need);
            out->sin6_family = af_to_user(AF_INET6);
            out->sin6_port = __builtin_bswap16(child->remote_port);
            kmemcpy(out->sin6_addr, child->remote_ip.v6, 16);
            *len = need;
        }
    }

    return child_sock;
}

static int tcp_sock_send(socket_t* s, const uint8_t* buf, uint32_t len) {
    tcp_conn_t* c = (tcp_conn_t*)s->proto;
    if (!c) return ERR_INVAL;
    c->send_timeout = s->send_timeout;
    return tcp_conn_send(c, buf, len);
}

static int tcp_sock_sendto(socket_t* s, const uint8_t* buf, uint32_t len,
                             const sockaddr_t* dst_addr, socklen_t addrlen) {
    (void)dst_addr; (void)addrlen;
    /* On connected sockets, sendto ignores dest_addr per POSIX */
    return tcp_sock_send(s, buf, len);
}

static int tcp_sock_recv(socket_t* s, uint8_t* buf, uint32_t size) {
    tcp_conn_t* c = (tcp_conn_t*)s->proto;
    if (!c) return ERR_INVAL;
    int timeout = s->recv_timeout;
    if (c->recv_timeout > 0) timeout = c->recv_timeout;
    if (timeout <= 0) timeout = s->nonblock ? 0 : 5000;
    return tcp_conn_recv(c, buf, size, timeout);
}

static int tcp_sock_recvfrom(socket_t* s, uint8_t* buf, uint32_t size,
                               sockaddr_t* src_addr, socklen_t* addrlen) {
    int ret = tcp_sock_recv(s, buf, size);
    if (ret < 0) return ret;
    if (src_addr && addrlen) {
        tcp_conn_t* c = (tcp_conn_t*)s->proto;
        if (c) {
            socklen_t need = (s->family == AF_INET) ? sizeof(sockaddr_in_t) : sizeof(sockaddr_in6_t);
            socklen_t copy_len = *addrlen < need ? *addrlen : need;
            kmemset(src_addr, 0, copy_len);
            if (s->family == AF_INET) {
                sockaddr_in_t* out = (sockaddr_in_t*)src_addr;
                out->sin_family = af_to_user(AF_INET);
                out->sin_port = __builtin_bswap16(c->remote_port);
                kmemcpy(out->sin_addr, c->remote_ip.v4.bytes, 4);
            } else {
                sockaddr_in6_t* out = (sockaddr_in6_t*)src_addr;
                out->sin6_family = af_to_user(AF_INET6);
                out->sin6_port = __builtin_bswap16(c->remote_port);
                if (c->af == AF_INET && !s->ipv6only) {
                    out->sin6_addr[10] = 0xFF;
                    out->sin6_addr[11] = 0xFF;
                    kmemcpy(out->sin6_addr + 12, c->remote_ip.v4.bytes, 4);
                } else {
                    kmemcpy(out->sin6_addr, c->remote_ip.v6, 16);
                }
            }
            *addrlen = need;
        }
    }
    return ret;
}

static int tcp_sock_close(socket_t* s) {
    tcp_conn_t* c = (tcp_conn_t*)s->proto;
    if (c) {
        tcp_conn_close(c);
    }
    s->proto = NULL;
    s->state = SS_CLOSED;
    return ERR_OK;
}

static int tcp_sock_setsockopt(socket_t* s, int level, int optname,
                                 const void* optval, socklen_t optlen) {
    if (level == SOL_IPV6 && optname == IPV6_V6ONLY) {
        if (optlen < sizeof(int)) return ERR_INVAL;
        s->ipv6only = *(const int*)optval ? 1 : 0;
        return ERR_OK;
    }
    if (level == SOL_IPV6 && (optname == IPV6_JOIN_GROUP || optname == IPV6_LEAVE_GROUP)) {
        if (optlen < sizeof(ipv6_mreq_t)) return ERR_INVAL;
        const ipv6_mreq_t* mreq = (const ipv6_mreq_t*)optval;
        if (optname == IPV6_JOIN_GROUP)
            return ipv6_mcast_join(mreq->ipv6mr_multiaddr);
        else
            return ipv6_mcast_leave(mreq->ipv6mr_multiaddr);
    }
    if (level == SOL_SOCKET) {
        if (optname == SO_RCVTIMEO || optname == SO_SNDTIMEO) {
            if (optlen < sizeof(struct timeval)) return ERR_INVAL;
            const struct timeval* tv = (const struct timeval*)optval;
            int timeout_ms = (int)(tv->tv_sec * 1000 + tv->tv_usec / 1000);
            if (timeout_ms < 0) timeout_ms = 0;
            if (optname == SO_RCVTIMEO)
                s->recv_timeout = timeout_ms;
            else
                s->send_timeout = timeout_ms;
            /* Propagate to transport */
            tcp_conn_t* c = (tcp_conn_t*)s->proto;
            if (c) {
                if (optname == SO_RCVTIMEO) c->recv_timeout = timeout_ms;
                else c->send_timeout = timeout_ms;
            }
            return ERR_OK;
        }
        if (optname == SO_KEEPALIVE) {
            return ERR_OK;
        }
    }
    if (level == SOL_TCP || level == IPPROTO_TCP) {
        if (optname == TCP_NODELAY) {
            if (optlen < sizeof(int)) return ERR_INVAL;
            tcp_conn_t* c = (tcp_conn_t*)s->proto;
            if (c) c->nodelay = *(const int*)optval ? 1 : 0;
            return ERR_OK;
        }
    }
    if (level == SOL_IP) {
        if (optname == IP_ADD_MEMBERSHIP) {
            if (optlen < sizeof(ip_mreq_t)) return ERR_INVAL;
            const ip_mreq_t* mreq = (const ip_mreq_t*)optval;
            return igmp_mcast_join(__builtin_bswap32(mreq->imr_multiaddr));
        }
        if (optname == IP_DROP_MEMBERSHIP) {
            if (optlen < sizeof(ip_mreq_t)) return ERR_INVAL;
            const ip_mreq_t* mreq = (const ip_mreq_t*)optval;
            return igmp_mcast_leave(__builtin_bswap32(mreq->imr_multiaddr));
        }
    }
    (void)s; (void)level; (void)optname; (void)optval; (void)optlen;
    return ERR_NOSYS;
}

static int tcp_sock_getsockopt(socket_t* s, int level, int optname,
                                 void* optval, socklen_t* optlen) {
    if (level == SOL_TCP || level == IPPROTO_TCP) {
        if (optname == TCP_NODELAY) {
            if (!optval || !optlen) return ERR_INVAL;
            if (*optlen < sizeof(int)) return ERR_INVAL;
            tcp_conn_t* c = (tcp_conn_t*)s->proto;
            *(int*)optval = (c && c->nodelay) ? 1 : 0;
            *optlen = sizeof(int);
            return ERR_OK;
        }
    }
    if (level == SOL_SOCKET) {
        if (optname == SO_RCVTIMEO || optname == SO_SNDTIMEO) {
            if (!optval || !optlen) return ERR_INVAL;
            if (*optlen < sizeof(struct timeval)) return ERR_INVAL;
            struct timeval* tv = (struct timeval*)optval;
            tcp_conn_t* c = (tcp_conn_t*)s->proto;
            int timeout_ms = 5000;
            if (c) timeout_ms = (optname == SO_RCVTIMEO)
                ? c->recv_timeout : c->send_timeout;
            tv->tv_sec = timeout_ms / 1000;
            tv->tv_usec = (timeout_ms % 1000) * 1000;
            *optlen = sizeof(struct timeval);
            return ERR_OK;
        }
    }
    (void)s; (void)level; (void)optname; (void)optval; (void)optlen;
    return ERR_NOSYS;
}

static int tcp_sock_getsockname(socket_t* s, sockaddr_t* addr, socklen_t* len) {
    tcp_conn_t* c = (tcp_conn_t*)s->proto;
    if (!c) return ERR_INVAL;
    socklen_t need = (s->family == AF_INET) ? sizeof(sockaddr_in_t) : sizeof(sockaddr_in6_t);
    if (*len < need) return ERR_INVAL;
    kmemset(addr, 0, need);
    if (s->family == AF_INET) {
        sockaddr_in_t* out = (sockaddr_in_t*)addr;
        out->sin_family = af_to_user(AF_INET);
        out->sin_port = __builtin_bswap16(c->local_port);
        ipv4_addr_t _local = ipv4_get_addr();
        kmemcpy(out->sin_addr, _local.bytes, 4);
    } else {
        sockaddr_in6_t* out = (sockaddr_in6_t*)addr;
        out->sin6_family = af_to_user(AF_INET6);
        out->sin6_port = __builtin_bswap16(c->local_port);
        /* If connection is IPv4 over dual-stack, present as ::ffff:x.x.x.x */
        if (c->af == AF_INET && !s->ipv6only) {
            ipv4_addr_t _local = ipv4_get_addr();
            out->sin6_addr[10] = 0xFF;
            out->sin6_addr[11] = 0xFF;
            kmemcpy(out->sin6_addr + 12, _local.bytes, 4);
        }
    }
    *len = need;
    return ERR_OK;
}

static int tcp_sock_getpeername(socket_t* s, sockaddr_t* addr, socklen_t* len) {
    tcp_conn_t* c = (tcp_conn_t*)s->proto;
    if (!c) return ERR_INVAL;
    if (c->state != TCP_ESTABLISHED && c->state != TCP_CLOSE_WAIT)
        return ERR_NOTCONN;
    socklen_t need = (s->family == AF_INET) ? sizeof(sockaddr_in_t) : sizeof(sockaddr_in6_t);
    if (*len < need) return ERR_INVAL;
    kmemset(addr, 0, need);
    if (s->family == AF_INET) {
        sockaddr_in_t* out = (sockaddr_in_t*)addr;
        out->sin_family = af_to_user(AF_INET);
        out->sin_port = __builtin_bswap16(c->remote_port);
        kmemcpy(out->sin_addr, c->remote_ip.v4.bytes, 4);
    } else {
        sockaddr_in6_t* out = (sockaddr_in6_t*)addr;
        out->sin6_family = af_to_user(AF_INET6);
        out->sin6_port = __builtin_bswap16(c->remote_port);
        /* If connection is IPv4 over dual-stack, present as ::ffff:x.x.x.x */
        if (c->af == AF_INET && !s->ipv6only) {
            out->sin6_addr[10] = 0xFF;
            out->sin6_addr[11] = 0xFF;
            kmemcpy(out->sin6_addr + 12, c->remote_ip.v4.bytes, 4);
        } else {
            kmemcpy(out->sin6_addr, c->remote_ip.v6, 16);
        }
    }
    *len = need;
    return ERR_OK;
}

static int tcp_sock_ioctl(socket_t* s, uint64_t request, void* argp) {
    if (request == FIONBIO) {
        int val;
        if (copy_from_user(&val, argp, sizeof(val)) != 0)
            return ERR_FAULT;
        s->nonblock = (val != 0) ? 1 : 0;
        return ERR_OK;
    }
    return ERR_NOSYS;
}

static int tcp_sock_poll(socket_t* s, int events, int* revents) {
    *revents = 0;
    tcp_conn_t* c = (tcp_conn_t*)s->proto;
    if (!c) { *revents = POLLNVAL; return ERR_OK; }
    int st = c->state;
    /* POLLIN: data available or connection ready to accept or FIN received */
    if (events & POLLIN) {
        if (st == TCP_LISTEN) {
            if (c->accept_count > 0) *revents |= POLLIN;
        } else if (st == TCP_ESTABLISHED || st == TCP_CLOSE_WAIT) {
            if (c->recv_done || c->closed) *revents |= POLLIN;
        } else if (st == TCP_CLOSED) {
            *revents |= POLLIN | POLLHUP;
        }
    }
    /* POLLOUT: can send data */
    if (events & POLLOUT) {
        if (st == TCP_ESTABLISHED) *revents |= POLLOUT;
        if (st == TCP_CLOSE_WAIT) *revents |= POLLOUT;
        if (st == TCP_CLOSED) *revents |= POLLOUT | POLLHUP;
    }
    /* POLLERR / POLLHUP */
    if (st == TCP_CLOSED) {
        *revents |= POLLHUP;
        if (c->closed) *revents |= POLLERR;
    }
    return ERR_OK;
}

static int udp_sock_poll(socket_t* s, int events, int* revents) {
    *revents = 0;
    udp_endpoint_t* ep = (udp_endpoint_t*)s->proto;
    if (!ep) {
        if (events & POLLOUT) *revents |= POLLOUT;
        return ERR_OK;
    }
    if (events & POLLIN) {
        if (ep->q_count > 0) *revents |= POLLIN;
    }
    if (events & POLLOUT) {
        *revents |= POLLOUT;
    }
    return ERR_OK;
}

static sock_ops_t tcp_ops = {
    .bind     = tcp_sock_bind,
    .connect  = tcp_sock_connect,
    .listen   = tcp_sock_listen,
    .accept   = tcp_sock_accept,
    .send     = tcp_sock_send,
    .recv     = tcp_sock_recv,
    .close    = tcp_sock_close,
    .sendto   = tcp_sock_sendto,
    .recvfrom = tcp_sock_recvfrom,
    .setsockopt = tcp_sock_setsockopt,
    .getsockopt = tcp_sock_getsockopt,
    .getsockname = tcp_sock_getsockname,
    .getpeername = tcp_sock_getpeername,
    .ioctl      = tcp_sock_ioctl,
    .poll       = tcp_sock_poll,
};

/* ---- UDP socket operations ---- */

static int udp_sock_bind(socket_t* s, const sockaddr_t* addr, socklen_t len) {
    uint16_t port;
    uint8_t local_addr[16];

    kmemset(local_addr, 0, 16);
    if (s->family == AF_INET) {
        if (len < (socklen_t)sizeof(sockaddr_in_t)) return ERR_INVAL;
        const sockaddr_in_t* in = (const sockaddr_in_t*)addr;
        port = __builtin_bswap16(in->sin_port);
        kmemcpy(local_addr, in->sin_addr, 4);
    } else {
        if (len < (socklen_t)sizeof(sockaddr_in6_t)) return ERR_INVAL;
        const sockaddr_in6_t* in6 = (const sockaddr_in6_t*)addr;
        port = __builtin_bswap16(in6->sin6_port);
        kmemcpy(local_addr, in6->sin6_addr, 16);
    }

    int e = udp_bind_endpoint(s->family, local_addr, port,
                               s->recv_timeout > 0 ? s->recv_timeout : 5000,
                               s->send_timeout > 0 ? s->send_timeout : 5000,
                               s->ipv6only);
    if (e != ERR_OK) return e;

    /* Find the internal copy and use its address */
    s->proto = udp_find_endpoint(s->family, port);
    s->state = SS_BOUND;
    return ERR_OK;
}

static uint16_t udp_auto_port = 49152;

static int udp_sock_autobind(socket_t* s) {
    uint16_t port = udp_auto_port++;
    int e = udp_bind_endpoint(s->family, NULL, port,
                               s->recv_timeout > 0 ? s->recv_timeout : 5000,
                               s->send_timeout > 0 ? s->send_timeout : 5000,
                               s->ipv6only);
    if (e != ERR_OK) return e;
    s->proto = udp_find_endpoint(s->family, port);
    s->state = SS_BOUND;
    return ERR_OK;
}

static int udp_sock_connect(socket_t* s, const sockaddr_t* addr, socklen_t len) {
    if (!s || !addr) return ERR_INVAL;
    if (s->type != 2) return ERR_INVAL; /* SOCK_DGRAM */
    udp_endpoint_t* ep = (udp_endpoint_t*)s->proto;
    if (!ep) {
        int e = udp_sock_autobind(s);
        if (e != ERR_OK) return e;
        ep = (udp_endpoint_t*)s->proto;
    }
    if (!ep) return ERR_INVAL;
    socklen_t copy = (len < SOCKADDR_MAX) ? len : SOCKADDR_MAX;
    kmemcpy(s->udp_conn_addr, addr, copy);
    s->udp_conn_addrlen = copy;
    s->state = SS_CONNECTED;
    return ERR_OK;
}

static int udp_sock_listen(socket_t* s, int backlog) {
    (void)s; (void)backlog;
    return ERR_NOSYS;
}

static socket_t* udp_sock_accept(socket_t* s, sockaddr_t* addr, socklen_t* len) {
    (void)s; (void)addr; (void)len;
    return NULL;
}

static int udp_sock_sendto(socket_t* s, const uint8_t* buf, uint32_t len,
                             const sockaddr_t* dst_addr, socklen_t addrlen) {
    udp_endpoint_t* ep = (udp_endpoint_t*)s->proto;
    if (!ep) {
        int e = udp_sock_autobind(s);
        if (e != ERR_OK) return e;
        ep = (udp_endpoint_t*)s->proto;
    }
    if (!ep) return ERR_INVAL;

    void* dst_ip;
    uint16_t dst_port;

    if (s->family == AF_INET) {
        if (addrlen < (socklen_t)sizeof(sockaddr_in_t)) return ERR_INVAL;
        const sockaddr_in_t* in = (const sockaddr_in_t*)dst_addr;
        dst_ip = (void*)in->sin_addr;
        dst_port = __builtin_bswap16(in->sin_port);
    } else {
        if (addrlen < (socklen_t)sizeof(sockaddr_in6_t)) return ERR_INVAL;
        const sockaddr_in6_t* in6 = (const sockaddr_in6_t*)dst_addr;
        dst_ip = (void*)in6->sin6_addr;
        dst_port = __builtin_bswap16(in6->sin6_port);
    }

    int e = udp_sendto(s->family, dst_ip, dst_port, ep->port, buf, len);
    if (e < 0) return e;
    return (int)len;
}

static int udp_sock_recvfrom(socket_t* s, uint8_t* buf, uint32_t size,
                               sockaddr_t* src_addr, socklen_t* addrlen) {
    udp_endpoint_t* ep = (udp_endpoint_t*)s->proto;
    if (!ep) return ERR_INVAL;

    int timeout = ep->recv_timeout > 0 ? ep->recv_timeout : s->recv_timeout;
    if (timeout <= 0) timeout = 5000;

    uint8_t src_ip[16];
    uint16_t src_port;
    int recv_af = AF_INET;
    int ret = udp_endpoint_dequeue(ep, buf, size, &recv_af,
                                       src_ip, &src_port, timeout);
    if (ret < 0) return ret;

    if (src_addr && addrlen) {
        if (recv_af == AF_INET) {
            sockaddr_in_t* out = (sockaddr_in_t*)src_addr;
            kmemset(out, 0, sizeof(sockaddr_in_t));
            out->sin_family = af_to_user(AF_INET);
            out->sin_port = __builtin_bswap16(src_port);
            kmemcpy(out->sin_addr, src_ip, 4);
            *addrlen = sizeof(sockaddr_in_t);
        } else {
            sockaddr_in6_t* out = (sockaddr_in6_t*)src_addr;
            kmemset(out, 0, sizeof(sockaddr_in6_t));
            out->sin6_family = af_to_user(AF_INET6);
            out->sin6_port = __builtin_bswap16(src_port);
            kmemcpy(out->sin6_addr, src_ip, 16);
            *addrlen = sizeof(sockaddr_in6_t);
        }
    }

    return ret;
}

static int udp_sock_send(socket_t* s, const uint8_t* buf, uint32_t len) {
    if (!s || s->state != SS_CONNECTED || s->udp_conn_addrlen == 0)
        return ERR_NOTCONN;
    return udp_sock_sendto(s, buf, len,
                           (const sockaddr_t*)s->udp_conn_addr,
                           s->udp_conn_addrlen);
}

static int udp_sock_recv(socket_t* s, uint8_t* buf, uint32_t size) {
    return udp_sock_recvfrom(s, buf, size, NULL, NULL);
}

static int udp_sock_close(socket_t* s) {
    udp_endpoint_t* ep = (udp_endpoint_t*)s->proto;
    if (ep) {
        udp_unbind_endpoint(ep);
    }
    s->proto = NULL;
    s->state = SS_CLOSED;
    return ERR_OK;
}

static int udp_sock_setsockopt(socket_t* s, int level, int optname,
                                 const void* optval, socklen_t optlen) {
    if (!s) return ERR_INVAL;
    if (level == SOL_IPV6 && optname == IPV6_V6ONLY) {
        if (optlen < sizeof(int)) return ERR_INVAL;
        s->ipv6only = *(const int*)optval ? 1 : 0;
        return ERR_OK;
    }
    if (level == SOL_IPV6 && (optname == IPV6_JOIN_GROUP || optname == IPV6_LEAVE_GROUP)) {
        if (optlen < sizeof(ipv6_mreq_t)) return ERR_INVAL;
        const ipv6_mreq_t* mreq = (const ipv6_mreq_t*)optval;
        if (optname == IPV6_JOIN_GROUP)
            return ipv6_mcast_join(mreq->ipv6mr_multiaddr);
        else
            return ipv6_mcast_leave(mreq->ipv6mr_multiaddr);
    }
    if (level == SOL_SOCKET) {
        if (optname == SO_RCVTIMEO) {
            if (optlen < sizeof(struct timeval)) return ERR_INVAL;
            const struct timeval* tv = (const struct timeval*)optval;
            int timeout_ms = (int)(tv->tv_sec * 1000 + tv->tv_usec / 1000);
            if (timeout_ms <= 0) timeout_ms = 5000;
            s->recv_timeout = timeout_ms;
            udp_endpoint_t* ep = (udp_endpoint_t*)s->proto;
            if (ep) ep->recv_timeout = timeout_ms;
            return ERR_OK;
        }
        if (optname == SO_SNDTIMEO) {
            if (optlen < sizeof(struct timeval)) return ERR_INVAL;
            const struct timeval* tv = (const struct timeval*)optval;
            int timeout_ms = (int)(tv->tv_sec * 1000 + tv->tv_usec / 1000);
            if (timeout_ms < 0) timeout_ms = 0;
            s->send_timeout = timeout_ms;
            udp_endpoint_t* ep = (udp_endpoint_t*)s->proto;
            if (ep) ep->send_timeout = timeout_ms;
            return ERR_OK;
        }
        if (optname == SO_KEEPALIVE) {
            return ERR_OK;
        }
    }
    if (level == SOL_IP) {
        if (optname == IP_ADD_MEMBERSHIP) {
            if (optlen < sizeof(ip_mreq_t)) return ERR_INVAL;
            const ip_mreq_t* mreq = (const ip_mreq_t*)optval;
            return igmp_mcast_join(__builtin_bswap32(mreq->imr_multiaddr));
        }
        if (optname == IP_DROP_MEMBERSHIP) {
            if (optlen < sizeof(ip_mreq_t)) return ERR_INVAL;
            const ip_mreq_t* mreq = (const ip_mreq_t*)optval;
            return igmp_mcast_leave(__builtin_bswap32(mreq->imr_multiaddr));
        }
    }
    return ERR_NOSYS;
}

static int udp_sock_getsockopt(socket_t* s, int level, int optname,
                                 void* optval, socklen_t* optlen) {
    if (level == SOL_SOCKET) {
        if (optname == SO_RCVTIMEO || optname == SO_SNDTIMEO) {
            if (!optval || !optlen) return ERR_INVAL;
            if (*optlen < sizeof(struct timeval)) return ERR_INVAL;
            struct timeval* tv = (struct timeval*)optval;
            int timeout_ms = (optname == SO_RCVTIMEO)
                ? s->recv_timeout : s->send_timeout;
            tv->tv_sec = timeout_ms / 1000;
            tv->tv_usec = (timeout_ms % 1000) * 1000;
            *optlen = sizeof(struct timeval);
            return ERR_OK;
        }
    }
    if (level == SOL_IPV6 && optname == IPV6_V6ONLY) {
        if (!optval || !optlen) return ERR_INVAL;
        if (*optlen < sizeof(int)) return ERR_INVAL;
        *(int*)optval = s->ipv6only;
        *optlen = sizeof(int);
        return ERR_OK;
    }
    (void)optname; (void)optlen;
    return ERR_NOSYS;
}

static int udp_sock_getsockname(socket_t* s, sockaddr_t* addr, socklen_t* len) {
    udp_endpoint_t* ep = (udp_endpoint_t*)s->proto;
    if (!ep) return ERR_INVAL;
    socklen_t need = (s->family == AF_INET) ? sizeof(sockaddr_in_t) : sizeof(sockaddr_in6_t);
    if (*len < need) return ERR_INVAL;
    kmemset(addr, 0, need);
    if (s->family == AF_INET) {
        sockaddr_in_t* out = (sockaddr_in_t*)addr;
        out->sin_family = af_to_user(AF_INET);
        out->sin_port = __builtin_bswap16(ep->port);
    } else {
        sockaddr_in6_t* out = (sockaddr_in6_t*)addr;
        out->sin6_family = af_to_user(AF_INET6);
        out->sin6_port = __builtin_bswap16(ep->port);
    }
    *len = need;
    return ERR_OK;
}

static int udp_sock_getpeername(socket_t* s, sockaddr_t* addr, socklen_t* len) {
    (void)s; (void)addr; (void)len;
    return ERR_NOTCONN;
}

static int udp_sock_ioctl(socket_t* s, uint64_t request, void* argp) {
    if (request == FIONBIO) {
        int val;
        if (copy_from_user(&val, argp, sizeof(val)) != 0)
            return ERR_FAULT;
        s->nonblock = (val != 0) ? 1 : 0;
        return ERR_OK;
    }
    return ERR_NOSYS;
}

static sock_ops_t udp_ops = {
    .bind     = udp_sock_bind,
    .connect  = udp_sock_connect,
    .listen   = udp_sock_listen,
    .accept   = udp_sock_accept,
    .send     = udp_sock_send,
    .recv     = udp_sock_recv,
    .close    = udp_sock_close,
    .sendto   = udp_sock_sendto,
    .recvfrom = udp_sock_recvfrom,
    .setsockopt = udp_sock_setsockopt,
    .getsockopt = udp_sock_getsockopt,
    .getsockname = udp_sock_getsockname,
    .getpeername = udp_sock_getpeername,
    .ioctl      = udp_sock_ioctl,
    .poll       = udp_sock_poll,
};

/* ---- Socket alloc/retain/release ---- */

extern sock_ops_t unix_ops;

socket_t* socket_alloc(int family, int type, int protocol) {
    (void)protocol;
    if (!net_initialized) return NULL;

    socket_t* s = (socket_t*)kmalloc(sizeof(socket_t));
    if (!s) return NULL;
    kmemset(s, 0, sizeof(socket_t));

    s->refcount = 1;
    s->fd = -1;
    s->family = family;
    s->type = type;
    s->state = SS_UNBOUND;
    s->recv_timeout = 5000; /* default 5 second timeout */
    s->send_timeout = 5000; /* default 5 second send timeout */
    s->ipv6only = 0;       /* default: dual-stack */

    if (family == AF_UNIX) {
        s->ops = &unix_ops;
        return s;
    }

    if (type == SOCK_STREAM) {
        tcp_conn_t* c = tcp_conn_create(family);
        if (!c) { kfree(s); return NULL; }
        s->proto = c;
        s->ops = &tcp_ops;
    } else {
        s->ops = &udp_ops;
    }

    return s;
}

void socket_retain(socket_t* s) {
    if (s) s->refcount++;
}

void socket_release(socket_t* s) {
    if (!s) return;
    if (--s->refcount <= 0) {
        if (s->ops && s->ops->close) s->ops->close(s);
        kfree(s);
    }
}

/* ---- Socket operations (called by syscalls) ---- */

int sock_bind(socket_t* s, const sockaddr_t* addr, socklen_t len) {
    if (!s || !addr || !s->ops || !s->ops->bind) return ERR_INVAL;
    return s->ops->bind(s, addr, len);
}

int sock_connect(socket_t* s, const sockaddr_t* addr, socklen_t len) {
    if (!s || !addr || !s->ops || !s->ops->connect) return ERR_INVAL;
    return s->ops->connect(s, addr, len);
}

int sock_listen(socket_t* s, int backlog) {
    if (!s || !s->ops || !s->ops->listen) return ERR_INVAL;
    return s->ops->listen(s, backlog);
}

socket_t* sock_accept(socket_t* s, sockaddr_t* addr, socklen_t* len) {
    if (!s || !s->ops || !s->ops->accept) return NULL;
    return s->ops->accept(s, addr, len);
}

int sock_send(socket_t* s, const uint8_t* buf, uint32_t len) {
    if (!s || !s->ops || !s->ops->send) return ERR_INVAL;
    return s->ops->send(s, buf, len);
}

int sock_recv(socket_t* s, uint8_t* buf, uint32_t size) {
    if (!s || !s->ops || !s->ops->recv) return ERR_INVAL;
    return s->ops->recv(s, buf, size);
}

int sock_sendto(socket_t* s, const uint8_t* buf, uint32_t len,
                const sockaddr_t* dst_addr, socklen_t addrlen) {
    if (!s || !s->ops || !s->ops->sendto) return ERR_INVAL;
    return s->ops->sendto(s, buf, len, dst_addr, addrlen);
}

int sock_recvfrom(socket_t* s, uint8_t* buf, uint32_t size,
                  sockaddr_t* src_addr, socklen_t* addrlen) {
    if (!s || !s->ops || !s->ops->recvfrom) return ERR_INVAL;
    return s->ops->recvfrom(s, buf, size, src_addr, addrlen);
}

int sock_close(socket_t* s) {
    if (!s) return ERR_INVAL;
    int ret = ERR_OK;
    if (s->ops && s->ops->close) ret = s->ops->close(s);
    sock_unregister(s->fd);
    socket_release(s);
    return ret;
}

int sock_setsockopt(socket_t* s, int level, int optname,
                     const void* optval, socklen_t optlen) {
    if (!s || !s->ops || !s->ops->setsockopt) return ERR_INVAL;
    return s->ops->setsockopt(s, level, optname, optval, optlen);
}

int sock_getsockopt(socket_t* s, int level, int optname,
                     void* optval, socklen_t* optlen) {
    if (!s || !s->ops || !s->ops->getsockopt) return ERR_INVAL;
    return s->ops->getsockopt(s, level, optname, optval, optlen);
}

int sock_getsockname(socket_t* s, sockaddr_t* addr, socklen_t* len) {
    if (!s || !s->ops || !s->ops->getsockname) return ERR_INVAL;
    return s->ops->getsockname(s, addr, len);
}

int sock_getpeername(socket_t* s, sockaddr_t* addr, socklen_t* len) {
    if (!s || !s->ops || !s->ops->getpeername) return ERR_INVAL;
    return s->ops->getpeername(s, addr, len);
}

int sock_ioctl(socket_t* s, uint64_t request, void* argp) {
    if (!s || !s->ops || !s->ops->ioctl) return ERR_NOSYS;
    return s->ops->ioctl(s, request, argp);
}

int sock_poll(socket_t* s, int events, int* revents) {
    if (!s || !s->ops || !s->ops->poll) { *revents = POLLNVAL; return ERR_OK; }
    return s->ops->poll(s, events, revents);
}

/* ---- Socket registration (process fd table) ---- */

int sock_register(socket_t* s) {
    if (!s) return ERR_INVAL;
    cpu_flags_t flags;
    spinlock_acquire(&sockets_lock, &flags);
    for (int i = 0; i < NET_MAX_SOCKETS; i++) {
        if (!net_sockets[i].used) {
            net_sockets[i].used = 1;
            net_sockets[i].sock = s;
            s->fd = i;
            spinlock_release(&sockets_lock, flags);
            return i;
        }
    }
    spinlock_release(&sockets_lock, flags);
    return ERR_NOSPACE;
}

socket_t* sock_lookup(int fd) {
    if (fd < 0 || fd >= NET_MAX_SOCKETS) return NULL;
    cpu_flags_t flags;
    spinlock_acquire(&sockets_lock, &flags);
    socket_t* s = NULL;
    if (net_sockets[fd].used) s = net_sockets[fd].sock;
    spinlock_release(&sockets_lock, flags);
    return s;
}

int sock_unregister(int fd) {
    if (fd < 0 || fd >= NET_MAX_SOCKETS) return ERR_INVAL;
    cpu_flags_t flags;
    spinlock_acquire(&sockets_lock, &flags);
    if (!net_sockets[fd].used) { spinlock_release(&sockets_lock, flags); return ERR_INVAL; }
    net_sockets[fd].used = 0;
    net_sockets[fd].sock = NULL;
    spinlock_release(&sockets_lock, flags);
    return ERR_OK;
}

/* ---- Net subsystem init ---- */

void net_init(void) {
    if (net_initialized) return;
    net_initialized = 1;
    unix_init();
    kprintf("[NET] Socket layer initialized\n");
}
#include "kernel.h"
#include "hal.h"
#include "unix.h"
#include "kmalloc.h"
#include "sched.h"
#include "process.h"
#include "net.h"

static int unix_initialized = 0;
sock_ops_t unix_ops;

/* Named socket registry: path → listening socket */
static unix_named_t unix_named_table[UNIX_MAX_NAMED];
static spinlock_t unix_named_lock;

void unix_init(void) {
    spinlock_init(&unix_named_lock, "unix_named_lock");
    kmemset(unix_named_table, 0, sizeof(unix_named_table));
    unix_initialized = 1;
}

int unix_buf_write(unix_buf_t* b, const uint8_t* data, uint32_t len) {
    uint32_t space = UNIX_BUF_SIZE - ((b->wr - b->rd) % (UNIX_BUF_SIZE + 1));
    if (space == 0) return 0;
    if (len > space) len = space;
    for (uint32_t i = 0; i < len; i++) {
        b->buf[b->wr] = data[i];
        b->wr = (b->wr + 1) % UNIX_BUF_SIZE;
    }
    return (int)len;
}

int unix_buf_read(unix_buf_t* b, uint8_t* data, uint32_t size) {
    uint32_t avail = (b->wr - b->rd) % (UNIX_BUF_SIZE + 1);
    if (avail == 0) return 0;
    if (size > avail) size = avail;
    for (uint32_t i = 0; i < size; i++) {
        data[i] = b->buf[b->rd];
        b->rd = (b->rd + 1) % UNIX_BUF_SIZE;
    }
    return (int)size;
}

static int path_equal(const char* a, const char* b) {
    for (int i = 0; i < UNIX_PATH_MAX; i++) {
        if (a[i] != b[i]) return 0;
        if (a[i] == 0) return 1;
    }
    return 1;
}

static void path_copy(char* dst, const char* src) {
    for (int i = 0; i < UNIX_PATH_MAX; i++) {
        dst[i] = src[i];
        if (src[i] == 0) break;
    }
}

/* ---- Named socket table operations ---- */

static int unix_named_register(const char* path, socket_t* s) {
    int slot = -1;
    for (int i = 0; i < UNIX_MAX_NAMED; i++) {
        if (!unix_named_table[i].used && slot < 0) slot = i;
        if (unix_named_table[i].used && path_equal(unix_named_table[i].path, path))
            return ERR_EXIST;
    }
    if (slot < 0) return ERR_NOSPACE;
    path_copy(unix_named_table[slot].path, path);
    unix_named_table[slot].sock = s;
    unix_named_table[slot].used = 1;
    return ERR_OK;
}

static socket_t* unix_named_find(const char* path) {
    for (int i = 0; i < UNIX_MAX_NAMED; i++) {
        if (unix_named_table[i].used && path_equal(unix_named_table[i].path, path))
            return unix_named_table[i].sock;
    }
    return NULL;
}

/* ---- Helper: create a socket pair for a new connection ---- */
static int unix_create_connected_pair(socket_t** out_a, socket_t** out_b) {
    process_t* proc = current_thread ? current_thread->proc : NULL;
    uid_t uid = proc ? proc->euid : 0;
    gid_t gid = proc ? proc->egid : 0;
    uint64_t pid = proc ? proc->pid : 0;

    unix_pair_t* pair = (unix_pair_t*)kmalloc(sizeof(unix_pair_t));
    if (!pair) return ERR_NOMEM;
    kmemset(pair, 0, sizeof(unix_pair_t));
    spinlock_init(&pair->lock, "unix_pair_lock");
    pair->refcount = 2;
    pair->cred[0].uid = uid;
    pair->cred[0].gid = gid;
    pair->cred[0].pid = pid;
    pair->cred[1].uid = uid;
    pair->cred[1].gid = gid;
    pair->cred[1].pid = pid;

    socket_t* sa = (socket_t*)kmalloc(sizeof(socket_t));
    socket_t* sb = (socket_t*)kmalloc(sizeof(socket_t));
    if (!sa || !sb) {
        kfree(pair);
        kfree(sa); kfree(sb);
        return ERR_NOMEM;
    }
    kmemset(sa, 0, sizeof(socket_t));
    kmemset(sb, 0, sizeof(socket_t));

    unix_endpoint_t* epa = (unix_endpoint_t*)kmalloc(sizeof(unix_endpoint_t));
    unix_endpoint_t* epb = (unix_endpoint_t*)kmalloc(sizeof(unix_endpoint_t));
    if (!epa || !epb) {
        kfree(pair); kfree(sa); kfree(sb);
        kfree(epa); kfree(epb);
        return ERR_NOMEM;
    }
    epa->pair = pair; epa->side = 0;
    epb->pair = pair; epb->side = 1;

    sa->refcount = 1; sa->family = AF_UNIX; sa->type = SOCK_STREAM;
    sa->state = SS_CONNECTED; sa->ops = NULL; sa->proto = epa;
    sa->recv_timeout = 5000; sa->send_timeout = 5000;

    sb->refcount = 1; sb->family = AF_UNIX; sb->type = SOCK_STREAM;
    sb->state = SS_CONNECTED; sb->ops = NULL; sb->proto = epb;
    sb->recv_timeout = 5000; sb->send_timeout = 5000;

    *out_a = sa;
    *out_b = sb;
    return 0;
}

/* ---- Helpers ---- */

static unix_endpoint_t* unix_get_ep(socket_t* s) {
    return (unix_endpoint_t*)s->proto;
}

#define OTHER(side) ((side) == 0 ? 1 : 0)

/* ---- Named socket operations ---- */

int unix_sock_bind(socket_t* s, const sockaddr_t* addr, socklen_t len) {
    if (!s || !addr) return ERR_INVAL;
    if (s->type != SOCK_STREAM) return ERR_INVAL;
    if (len < 2) return ERR_INVAL;
    const char* path = ((const sockaddr_un_t*)addr)->sun_path;
    int pathlen = 0;
    while (pathlen < UNIX_PATH_MAX && path[pathlen]) pathlen++;
    if (pathlen == 0 || pathlen >= UNIX_PATH_MAX) return ERR_INVAL;

    cpu_flags_t _sflags; spinlock_acquire(&unix_named_lock, &_sflags);
    int e = unix_named_register(path, s);
    spinlock_release(&unix_named_lock, _sflags);
    if (e != ERR_OK) return e;

    s->state = SS_BOUND;
    return ERR_OK;
}

int unix_sock_connect(socket_t* s, const sockaddr_t* addr, socklen_t len) {
    if (!s || !addr) return ERR_INVAL;
    if (s->type != SOCK_STREAM) return ERR_INVAL;
    if (len < 2) return ERR_INVAL;
    const char* path = ((const sockaddr_un_t*)addr)->sun_path;
    int pathlen = 0;
    while (pathlen < UNIX_PATH_MAX && path[pathlen]) pathlen++;
    if (pathlen == 0 || pathlen >= UNIX_PATH_MAX) return ERR_INVAL;

    cpu_flags_t _sflags; spinlock_acquire(&unix_named_lock, &_sflags);
    socket_t* target = unix_named_find(path);
    spinlock_release(&unix_named_lock, _sflags);

    if (!target) return ERR_NOSYS; /* No such socket */
    if (target->state != SS_LISTENING) return ERR_CONNREFUSED;

    unix_listener_t* lst = (unix_listener_t*)target->proto;
    if (!lst) return ERR_CONNREFUSED;

    /* Create a pair: one side for us, one for the listener */
    socket_t* our_side = NULL;
    socket_t* their_side = NULL;
    int e = unix_create_connected_pair(&our_side, &their_side);
    if (e != 0) return e;

    our_side->ops = &unix_ops;
    their_side->ops = &unix_ops;

    cpu_flags_t _lflags; spinlock_acquire(&lst->lock, &_lflags);
    if (lst->q_count >= lst->backlog) {
        spinlock_release(&lst->lock, _lflags);
        socket_release(our_side);
        socket_release(their_side);
        return ERR_NOSPACE;
    }
    lst->pending[lst->q_count++] = their_side;
    /* Transfer the listener's ref to the pending queue */
    socket_retain(their_side);
    spinlock_release(&lst->lock, _lflags);

    /* Wake any accept() waiter */
    if (lst->accept_wait.waiters)
        sched_wake(&lst->accept_wait);

    /* Replace our socket proto with the new endpoint */
    s->state = SS_CONNECTED;
    s->proto = our_side->proto;
    our_side->proto = NULL;
    socket_release(our_side);

    return ERR_OK;
}

int unix_sock_listen(socket_t* s, int backlog) {
    if (!s) return ERR_INVAL;
    if (s->state != SS_BOUND) return ERR_INVAL;
    if (backlog <= 0) backlog = 1;
    if (backlog > UNIX_BACKLOG_DEFAULT) backlog = UNIX_BACKLOG_DEFAULT;

    /* Find the path we're bound to */
    const char* bound_path = NULL;
    cpu_flags_t _sflags; spinlock_acquire(&unix_named_lock, &_sflags);
    for (int i = 0; i < UNIX_MAX_NAMED; i++) {
        if (unix_named_table[i].used && unix_named_table[i].sock == s) {
            bound_path = unix_named_table[i].path;
            break;
        }
    }
    spinlock_release(&unix_named_lock, _sflags);
    if (!bound_path) return ERR_INVAL;

    unix_listener_t* lst = (unix_listener_t*)kmalloc(sizeof(unix_listener_t));
    if (!lst) return ERR_NOMEM;
    kmemset(lst, 0, sizeof(unix_listener_t));
    spinlock_init(&lst->lock, "unix_listener_lock");
    path_copy(lst->path, bound_path);
    lst->backlog = backlog;
    lst->q_count = 0;
    wait_queue_init(&lst->accept_wait);

    s->proto = lst;
    s->state = SS_LISTENING;
    return ERR_OK;
}

socket_t* unix_sock_accept(socket_t* s, sockaddr_t* addr, socklen_t* len) {
    if (!s) return NULL;
    if (s->state != SS_LISTENING) return NULL;
    unix_listener_t* lst = (unix_listener_t*)s->proto;
    if (!lst) return NULL;

    int timeout = s->recv_timeout > 0 ? s->recv_timeout : 5000;
    uint64_t deadline_ns = hal_timer_get_ns() + (uint64_t)timeout * 1000000ULL;

    while (1) {
        cpu_flags_t _lflags; spinlock_acquire(&lst->lock, &_lflags);
        if (lst->q_count > 0) {
            socket_t* client = lst->pending[0];
            for (int i = 1; i < lst->q_count; i++)
                lst->pending[i - 1] = lst->pending[i];
            lst->q_count--;
            spinlock_release(&lst->lock, _lflags);

            /* Fill in peer address */
            if (addr && len) {
                if (*len >= sizeof(sockaddr_t)) {
                    kmemset(addr, 0, sizeof(sockaddr_t));
                    addr->sa_family = AF_UNIX;
                    *len = sizeof(sockaddr_t);
                }
            }

            /* Register the client socket in the caller's fd table */
            int fd = sock_register(client);
            if (fd < 0) {
                socket_release(client);
                return NULL;
            }

            /* Return a new socket_t* for the wrapper; the fd lookup sees client */
            return client;
        }
        if (s->nonblock) { spinlock_release(&lst->lock, _lflags); return NULL; }
        if (hal_timer_get_ns() >= deadline_ns) { spinlock_release(&lst->lock, _lflags); return NULL; }
        /* Wait for incoming connection */
        spinlock_release(&lst->lock, _lflags);
        thread_sleep(1);
    }
}

static void unix_cleanup_named(socket_t* s) {
    cpu_flags_t _sflags; spinlock_acquire(&unix_named_lock, &_sflags);
    for (int i = 0; i < UNIX_MAX_NAMED; i++) {
        if (unix_named_table[i].used && unix_named_table[i].sock == s) {
            unix_named_table[i].used = 0;
            break;
        }
    }
    spinlock_release(&unix_named_lock, _sflags);
}

/* ---- Existing operations (socketpair) ---- */

static int unix_sock_send(socket_t* s, const uint8_t* buf, uint32_t len) {
    unix_endpoint_t* ep = unix_get_ep(s);
    if (!ep || !ep->pair) return ERR_INVAL;
    unix_pair_t* p = ep->pair;
    int my_side = ep->side;
    int other = OTHER(my_side);
    unix_buf_t* dst = (my_side == 0) ? &p->a_to_b : &p->b_to_a;

    uint32_t total = 0;
    while (total < len) {
        cpu_flags_t _sflags; spinlock_acquire(&p->lock, &_sflags);
        if (p->closed[other]) { spinlock_release(&p->lock, _sflags); return ERR_IO; }
        uint32_t chunk = len - total;
        if (chunk > UNIX_BUF_SIZE) chunk = UNIX_BUF_SIZE;
        int n = unix_buf_write(dst, buf + total, chunk);
        if (n > 0) {
            total += (uint32_t)n;
            if (p->readers[other].waiters) sched_wake(&p->readers[other]);
            spinlock_release(&p->lock, _sflags);
        } else {
            spinlock_release(&p->lock, _sflags);
            thread_sleep(1);
        }
    }
    return (int)total;
}

static int unix_sock_recv(socket_t* s, uint8_t* buf, uint32_t size) {
    unix_endpoint_t* ep = unix_get_ep(s);
    if (!ep || !ep->pair) return ERR_INVAL;
    unix_pair_t* p = ep->pair;
    int my_side = ep->side;
    int other = OTHER(my_side);
    unix_buf_t* src = (my_side == 0) ? &p->b_to_a : &p->a_to_b;

    int timeout = s->recv_timeout > 0 ? s->recv_timeout : 5000;
    uint64_t deadline_ns = hal_timer_get_ns() + (uint64_t)timeout * 1000000ULL;

    while (1) {
        cpu_flags_t _sflags; spinlock_acquire(&p->lock, &_sflags);
        int n = unix_buf_read(src, buf, size);
        if (n > 0) {
            if (p->writers[other].waiters) sched_wake(&p->writers[other]);
            spinlock_release(&p->lock, _sflags);
            return n;
        }
        if (p->closed[other]) { spinlock_release(&p->lock, _sflags); return 0; }
        if (s->nonblock) { spinlock_release(&p->lock, _sflags); return ERR_AGAIN; }
        if (hal_timer_get_ns() >= deadline_ns) { spinlock_release(&p->lock, _sflags); return ERR_TIMEOUT; }
        spinlock_release(&p->lock, _sflags);
        thread_sleep(1);
    }
}

static int unix_sock_close(socket_t* s) {
    /* If this is a listening socket, clean up listener state */
    if (s->state == SS_LISTENING) {
        unix_cleanup_named(s);
        unix_listener_t* lst = (unix_listener_t*)s->proto;
        if (lst) {
            /* Reject any pending connections */
            for (int i = 0; i < lst->q_count; i++) {
                socket_release(lst->pending[i]);
            }
            kfree(lst);
        }
        s->proto = NULL;
        s->state = SS_CLOSED;
        return ERR_OK;
    }

    /* If this is a bound (non-listening) socket, clean up */
    if (s->state == SS_BOUND) {
        unix_cleanup_named(s);
        s->state = SS_CLOSED;
        return ERR_OK;
    }

    /* Connected socket: close the pair endpoint */
    unix_endpoint_t* ep = unix_get_ep(s);
    if (!ep) return ERR_OK;
    unix_pair_t* p = ep->pair;
    if (!p) return ERR_OK;

    cpu_flags_t _sflags; spinlock_acquire(&p->lock, &_sflags);
    p->closed[ep->side] = 1;
    int other = OTHER(ep->side);
    if (p->readers[other].waiters) sched_wake(&p->readers[other]);
    if (p->writers[other].waiters) sched_wake(&p->writers[other]);
    int do_free = (__sync_fetch_and_sub(&p->refcount, 1) == 1);
    spinlock_release(&p->lock, _sflags);

    kfree(ep);
    s->proto = NULL;
    s->state = SS_CLOSED;

    if (do_free) kfree(p);
    return ERR_OK;
}

static int unix_sock_sendto(socket_t* s, const uint8_t* buf, uint32_t len,
                             const sockaddr_t* dst_addr, socklen_t addrlen) {
    (void)dst_addr; (void)addrlen;
    return unix_sock_send(s, buf, len);
}

static int unix_sock_recvfrom(socket_t* s, uint8_t* buf, uint32_t size,
                               sockaddr_t* src_addr, socklen_t* addrlen) {
    (void)src_addr; (void)addrlen;
    return unix_sock_recv(s, buf, size);
}

static int unix_sock_setsockopt(socket_t* s, int level, int optname,
                                 const void* optval, socklen_t optlen) {
    (void)s; (void)level; (void)optname; (void)optval; (void)optlen;
    return ERR_NOSYS;
}

static int unix_sock_getsockopt(socket_t* s, int level, int optname,
                                 void* optval, socklen_t* optlen) {
    if (level == SOL_SOCKET && optname == SO_PEERCRED) {
        if (!optval || !optlen) return ERR_INVAL;
        if (*optlen < sizeof(ucred_t)) return ERR_INVAL;
        unix_endpoint_t* ep = unix_get_ep(s);
        if (!ep || !ep->pair) return ERR_INVAL;
        unix_pair_t* p = ep->pair;
        ucred_t* cred = (ucred_t*)optval;
        int other = OTHER(ep->side);
        cred->uid = p->cred[other].uid;
        cred->gid = p->cred[other].gid;
        cred->pid = p->cred[other].pid;
        *optlen = sizeof(ucred_t);
        return ERR_OK;
    }
    return ERR_NOSYS;
}

static int unix_sock_getsockname(socket_t* s, sockaddr_t* addr, socklen_t* len) {
    if (!s || !addr || !len) return ERR_INVAL;
    if (*len < 2) return ERR_INVAL;

    if (s->state == SS_LISTENING) {
        unix_listener_t* lst = (unix_listener_t*)s->proto;
        if (lst) {
            int slen = 0;
            while (slen < UNIX_PATH_MAX && lst->path[slen]) slen++;
            if (*len < (socklen_t)(2 + slen + 1)) return ERR_INVAL;
            addr->sa_family = AF_UNIX;
            char* dst = ((sockaddr_un_t*)addr)->sun_path;
            for (int i = 0; i <= slen; i++) dst[i] = lst->path[i];
            *len = 2 + slen + 1;
            return ERR_OK;
        }
    }

    kmemset(addr, 0, sizeof(sockaddr_t));
    addr->sa_family = AF_UNIX;
    *len = sizeof(sockaddr_t);
    return ERR_OK;
}

static int unix_sock_getpeername(socket_t* s, sockaddr_t* addr, socklen_t* len) {
    (void)s;
    if (!addr || !len) return ERR_INVAL;
    if (*len < sizeof(sockaddr_t)) return ERR_INVAL;
    kmemset(addr, 0, sizeof(sockaddr_t));
    addr->sa_family = AF_UNIX;
    *len = sizeof(sockaddr_t);
    return ERR_OK;
}

static int unix_sock_ioctl(socket_t* s, uint64_t request, void* argp) {
    if (request == FIONBIO) {
        int val;
        if (copy_from_user(&val, argp, sizeof(val)) != 0)
            return ERR_FAULT;
        s->nonblock = (val != 0) ? 1 : 0;
        return ERR_OK;
    }
    return ERR_NOSYS;
}

static int unix_sock_poll(socket_t* s, int events, int* revents) {
    int r = 0;

    /* Listening socket: check pending queue */
    if (s->state == SS_LISTENING) {
        unix_listener_t* lst = (unix_listener_t*)s->proto;
        if (lst) {
            cpu_flags_t _lflags; spinlock_acquire(&lst->lock, &_lflags);
            if ((events & POLLIN) && lst->q_count > 0) r |= POLLIN;
            spinlock_release(&lst->lock, _lflags);
        } else {
            if (events & (POLLIN | POLLOUT)) r |= (POLLIN | POLLOUT | POLLHUP);
        }
        if (revents) *revents = r;
        return 0;
    }

    /* Connected socket (pair endpoint) */
    unix_endpoint_t* ep = unix_get_ep(s);
    if (!ep || !ep->pair) {
        if (events & (POLLIN | POLLOUT)) r |= (POLLIN | POLLOUT | POLLHUP);
        if (revents) *revents = r;
        return 0;
    }
    unix_pair_t* p = ep->pair;
    int my_side = ep->side;
    int other = OTHER(my_side);
    unix_buf_t* src = (my_side == 0) ? &p->b_to_a : &p->a_to_b;
    unix_buf_t* dst = (my_side == 0) ? &p->a_to_b : &p->b_to_a;

    cpu_flags_t _sflags; spinlock_acquire(&p->lock, &_sflags);
    uint32_t avail = (src->wr - src->rd) % (UNIX_BUF_SIZE + 1);
    uint32_t space = UNIX_BUF_SIZE - ((dst->wr - dst->rd) % (UNIX_BUF_SIZE + 1));
    if (events & POLLIN) {
        if (avail > 0) r |= POLLIN;
        if (p->closed[other]) r |= POLLIN | POLLHUP;
    }
    if (events & POLLOUT) {
        if (space > 0) r |= POLLOUT;
        if (p->closed[other]) r |= POLLOUT | POLLHUP;
    }
    spinlock_release(&p->lock, _sflags);

    if (revents) *revents = r;
    return 0;
}

sock_ops_t unix_ops = {
    .bind        = unix_sock_bind,
    .connect     = unix_sock_connect,
    .listen      = unix_sock_listen,
    .accept      = unix_sock_accept,
    .send        = unix_sock_send,
    .recv        = unix_sock_recv,
    .close       = unix_sock_close,
    .sendto      = unix_sock_sendto,
    .recvfrom    = unix_sock_recvfrom,
    .setsockopt  = unix_sock_setsockopt,
    .getsockopt  = unix_sock_getsockopt,
    .getsockname = unix_sock_getsockname,
    .getpeername = unix_sock_getpeername,
    .ioctl       = unix_sock_ioctl,
    .poll        = unix_sock_poll,
};

int unix_socketpair(int sv[2]) {
    if (!unix_initialized) return ERR_NOSYS;

    process_t* proc = current_thread ? current_thread->proc : NULL;
    uid_t uid = proc ? proc->euid : 0;
    gid_t gid = proc ? proc->egid : 0;
    uint64_t pid = proc ? proc->pid : 0;

    unix_pair_t* pair = (unix_pair_t*)kmalloc(sizeof(unix_pair_t));
    if (!pair) return ERR_NOMEM;
    kmemset(pair, 0, sizeof(unix_pair_t));
    pair->refcount = 2;
    pair->cred[0].uid = uid;
    pair->cred[0].gid = gid;
    pair->cred[0].pid = pid;
    pair->cred[1].uid = uid;
    pair->cred[1].gid = gid;
    pair->cred[1].pid = pid;

    socket_t* s0 = (socket_t*)kmalloc(sizeof(socket_t));
    socket_t* s1 = (socket_t*)kmalloc(sizeof(socket_t));
    if (!s0 || !s1) {
        kfree(pair);
        if (s0) kfree(s0);
        if (s1) kfree(s1);
        return ERR_NOMEM;
    }
    kmemset(s0, 0, sizeof(socket_t));
    kmemset(s1, 0, sizeof(socket_t));

    unix_endpoint_t* ep0 = (unix_endpoint_t*)kmalloc(sizeof(unix_endpoint_t));
    unix_endpoint_t* ep1 = (unix_endpoint_t*)kmalloc(sizeof(unix_endpoint_t));
    if (!ep0 || !ep1) {
        kfree(pair); kfree(s0); kfree(s1);
        if (ep0) kfree(ep0);
        if (ep1) kfree(ep1);
        return ERR_NOMEM;
    }
    ep0->pair = pair; ep0->side = 0;
    ep1->pair = pair; ep1->side = 1;

    s0->refcount = 1; s0->family = AF_UNIX; s0->type = SOCK_STREAM;
    s0->state = SS_CONNECTED; s0->ops = &unix_ops; s0->proto = ep0;
    s0->recv_timeout = 5000; s0->send_timeout = 5000;

    s1->refcount = 1; s1->family = AF_UNIX; s1->type = SOCK_STREAM;
    s1->state = SS_CONNECTED; s1->ops = &unix_ops; s1->proto = ep1;
    s1->recv_timeout = 5000; s1->send_timeout = 5000;

    int fd0 = sock_register(s0);
    if (fd0 < 0) { socket_release(s0); socket_release(s1); return ERR_NOSPACE; }
    int fd1 = sock_register(s1);
    if (fd1 < 0) { sock_unregister(fd0); socket_release(s0); socket_release(s1); return ERR_NOSPACE; }

    sv[0] = fd0;
    sv[1] = fd1;
    return 0;
}

#ifndef UNIX_H
#define UNIX_H

#include "types.h"
#include "net.h"
#include "sync.h"
#include "sched.h" /* wait_queue_t, spinlock_t */

#define UNIX_BUF_SIZE 4096
#define UNIX_PATH_MAX 108
#define UNIX_MAX_NAMED 16
#define UNIX_BACKLOG_DEFAULT 8

/* Credentials for SO_PEERCRED */
typedef struct {
    uid_t    uid;
    gid_t    gid;
    uint64_t pid; /* kernel pid_t = uint64_t */
} ucred_t;

/* One direction's ring buffer */
typedef struct {
    uint8_t  buf[UNIX_BUF_SIZE];
    uint32_t wr;
    uint32_t rd;
} unix_buf_t;

/* Shared pair state */
typedef struct {
    unix_buf_t a_to_b;   /* socket A writes → socket B reads */
    unix_buf_t b_to_a;   /* socket B writes → socket A reads */
    int        refcount;
    int        closed[2];   /* per-side close flag */
    ucred_t    cred[2];
    spinlock_t lock;
    wait_queue_t readers[2], writers[2];
} unix_pair_t;

/* Per-socket endpoint */
typedef struct {
    unix_pair_t* pair;
    int          side;  /* 0 or 1 */
} unix_endpoint_t;

/* Named socket registration entry */
typedef struct {
    char       path[UNIX_PATH_MAX];
    socket_t*  sock;
    int        used;
} unix_named_t;

/* Listener state for named AF_UNIX sockets */
typedef struct {
    char       path[UNIX_PATH_MAX];
    int        backlog;
    int        q_count;
    socket_t*  pending[UNIX_BACKLOG_DEFAULT];
    spinlock_t lock;
    wait_queue_t accept_wait;
} unix_listener_t;

/* Create a pair of connected AF_UNIX stream sockets.
 * Returns number of fds (2) on success, or < 0 on error.
 * Fds are written to sv[0] and sv[1]. */
int  unix_socketpair(int sv[2]);

/* Named AF_UNIX socket operations (used by sock_ops_t) */
int        unix_sock_bind(socket_t* s, const sockaddr_t* addr, socklen_t len);
int        unix_sock_connect(socket_t* s, const sockaddr_t* addr, socklen_t len);
int        unix_sock_listen(socket_t* s, int backlog);
socket_t*  unix_sock_accept(socket_t* s, sockaddr_t* addr, socklen_t* len);

void unix_init(void);

/* Exposed for test/debug validation of ring buffer logic */
int  unix_buf_write(unix_buf_t* b, const uint8_t* data, uint32_t len);
int  unix_buf_read(unix_buf_t* b, uint8_t* data, uint32_t size);

#endif

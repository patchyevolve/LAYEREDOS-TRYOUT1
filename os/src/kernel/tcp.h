#ifndef TCP_H
#define TCP_H

#include "types.h"
#include "ip.h"

#define TCP_HDR_LEN  20
#define TCP_MSS      1460
#define TCP_MAX_CONN 16
#define TCP_WINDOW   65535

/* TCP queue lengths for accept */
#define TCP_BACKLOG  8

typedef struct __attribute__((packed)) {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t  offset;
    uint8_t  flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent;
} tcp_hdr_t;

#define TCP_FIN  1
#define TCP_SYN  2
#define TCP_RST  4
#define TCP_PSH  8
#define TCP_ACK  16
#define TCP_URG  32

typedef enum {
    TCP_CLOSED,
    TCP_LISTEN,
    TCP_SYN_SENT,
    TCP_SYN_RECEIVED,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT1,
    TCP_FIN_WAIT2,
    TCP_CLOSE_WAIT,
    TCP_CLOSING,
    TCP_LAST_ACK,
    TCP_TIME_WAIT
} tcp_state_t;

typedef struct tcp_conn {
    int         used;
    int         af;
    tcp_state_t state;
    uint16_t    local_port;
    union { ipv4_addr_t v4; uint8_t v6[16]; } remote_ip;
    uint16_t    remote_port;
    uint32_t    snd_nxt;
    uint32_t    rcv_nxt;
    uint32_t    snd_una;
    uint32_t    snd_wnd;    /* peer's advertised receive window */
    uint32_t    iss;
    uint32_t    irs;
    int         closed;

    /* Callbacks (used by socket layer) */
    void (*on_recv)(struct tcp_conn* conn, const uint8_t* data, uint32_t len);
    void (*on_close)(struct tcp_conn* conn);
    void (*on_connect)(struct tcp_conn* conn);

    /* Blocking recv support (recv_buf holds a copy to avoid use-after-return) */
    volatile int recv_done;
    uint8_t recv_buf[TCP_MSS];
    uint32_t recv_len;

    /* Accept queue for listening sockets */
    struct tcp_conn* accept_queue[TCP_BACKLOG];
    int accept_head;
    int accept_tail;
    int accept_count;

    /* IPV6_V6ONLY flag (default 1 for transport API, 0 for socket dual-stack) */
    int ipv6only;

    /* TCP_NODELAY flag: disable Nagle's algorithm */
    int nodelay;

    /* Timeouts */
    int recv_timeout;
    int send_timeout;

    /* TIME_WAIT 2MSL timer (ms remaining) — 0 means no timer active */
    uint32_t timewait_ms;

    /* Retransmission buffer and RTO tracking */
    uint8_t  retrans_buf[TCP_MSS];
    uint32_t retrans_seq;
    uint32_t retrans_len;
    uint32_t rto_ms;
    uint32_t rto_remaining;
    uint32_t fin_rto_remaining;
} tcp_conn_t;

/* ---- Stable transport API (sockets will wrap this) ---- */
void tcp_init(void);

/* Create/destroy a transport connection */
tcp_conn_t* tcp_conn_create(int af);
void        tcp_conn_destroy(tcp_conn_t* c);

/* Bind to a local port (optional; auto-assigned if not called) */
int tcp_conn_bind(tcp_conn_t* c, uint16_t port, int ipv6only);

/* Active open: connect to a remote host */
int tcp_conn_connect(tcp_conn_t* c, const void* dst_ip,
                     uint16_t dst_port, int timeout_ms);

/* Passive open: listen for incoming connections */
int tcp_conn_listen(tcp_conn_t* c, uint16_t port);

/* Blocking accept: returns a new connected connection */
tcp_conn_t* tcp_conn_accept(tcp_conn_t* c, int timeout_ms);

/* Send data on an established connection */
int tcp_conn_send(tcp_conn_t* c, const uint8_t* data, uint32_t len);

/* Blocking recv: copies up to `size` bytes into `buf` */
int tcp_conn_recv(tcp_conn_t* c, uint8_t* buf, uint32_t size, int timeout_ms);

/* Graceful close */
int tcp_conn_close(tcp_conn_t* c);

/* ---- Internal (used by net.c and packet handler) ---- */
tcp_conn_t* tcp_find_conn(int af, const void* src_ip,
                          uint16_t src_port, uint16_t dst_port);

/* ---- Legacy API (kept for backward compat during migration) ---- */
tcp_conn_t* tcp_listen(int af, uint16_t port,
                        void (*on_connect)(tcp_conn_t* conn));
tcp_conn_t* tcp_connect(int af, const void* dst_ip, uint16_t dst_port,
                         uint16_t src_port, int timeout_ms);
int tcp_send(tcp_conn_t* conn, const uint8_t* data, uint32_t len);
int tcp_close(tcp_conn_t* conn);

/* Get connection state (thread-safe) */
int tcp_conn_get_state(tcp_conn_t* c);

/* Periodic tick (called from NIC poll thread, ~10ms interval) */
void tcp_tick(void);

#ifdef NET_SELF_TEST
tcp_conn_t* tcp_test_add_conn(int af, const void* remote_ip,
                              uint16_t remote_port, uint16_t local_port);
#endif

#endif

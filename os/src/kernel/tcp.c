#include "kernel.h"
#include "tcp.h"
#include "ipv4.h"
#include "ipv6.h"
#include "route.h"
#include "nic.h"
#include "ndp.h"
#include "eth.h"
#include "hal.h"
#include "sched.h"
#include "sync.h"
#include "net_ns.h"

#define tcp_initialized (get_current_ns()->tcp_initialized)
#define tcp_conns (get_current_ns()->tcp_conns)
#define tcp_ephemeral_port (get_current_ns()->tcp_ephemeral_port)
#define tcp_lock (get_current_ns()->tcp_lock)

static uint16_t tcp_htons(uint16_t v) { return __builtin_bswap16(v); }
static uint32_t tcp_htonl(uint32_t v) { return __builtin_bswap32(v); }

static uint32_t tcp_checksum_pseudo(const void* tcp_pkt, uint32_t tcp_len,
                                     const void* src, const void* dst,
                                     int af) {
    uint32_t sum = 0;
    const uint16_t* p;
    if (af == AF_INET) {
        p = (const uint16_t*)src;
        for (int i = 0; i < 2; i++) sum += tcp_htons(p[i]);
        p = (const uint16_t*)dst;
        for (int i = 0; i < 2; i++) sum += tcp_htons(p[i]);
        sum += (uint16_t)IPV4_PROTO_TCP;
        sum += (uint16_t)tcp_len;
    } else {
        p = (const uint16_t*)src;
        for (int i = 0; i < 8; i++) sum += tcp_htons(p[i]);
        p = (const uint16_t*)dst;
        for (int i = 0; i < 8; i++) sum += tcp_htons(p[i]);
        sum += tcp_len;
        sum += (uint16_t)IPV4_PROTO_TCP;
    }
    p = (const uint16_t*)tcp_pkt;
    for (uint32_t i = 0; i < tcp_len / 2; i++) sum += tcp_htons(p[i]);
    if (tcp_len & 1)
        sum += (uint16_t)((const uint8_t*)tcp_pkt)[tcp_len - 1] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return tcp_htons((~sum) & 0xFFFF);
}

tcp_conn_t* tcp_find_conn(int af, const void* src_ip,
                          uint16_t src_port, uint16_t dst_port) {
    for (int i = 0; i < TCP_MAX_CONN; i++) {
        if (!tcp_conns[i].used) continue;
        if (tcp_conns[i].local_port != dst_port) continue;
        if (tcp_conns[i].state == TCP_LISTEN) continue;
        if (tcp_conns[i].remote_port != src_port) continue;
        if (af == AF_INET) {
            if (tcp_conns[i].af == AF_INET) {
                if (!ipv4_addr_equal(tcp_conns[i].remote_ip.v4,
                                     *(const ipv4_addr_t*)src_ip)) continue;
            } else if (tcp_conns[i].af == AF_INET6 && !tcp_conns[i].ipv6only) {
                /* Dual-stack: check ::ffff:a.b.c.d mapping */
                uint8_t* v6 = tcp_conns[i].remote_ip.v6;
                if (v6[10] != 0xFF || v6[11] != 0xFF) continue;
                if (v6[12] != ((const uint8_t*)src_ip)[0] ||
                    v6[13] != ((const uint8_t*)src_ip)[1] ||
                    v6[14] != ((const uint8_t*)src_ip)[2] ||
                    v6[15] != ((const uint8_t*)src_ip)[3]) continue;
            } else {
                continue;
            }
        } else {
            if (tcp_conns[i].af != AF_INET6) continue;
            if (kmemcmp(tcp_conns[i].remote_ip.v6, src_ip, 16) != 0) continue;
        }
        return &tcp_conns[i];
    }
    return NULL;
}

static tcp_conn_t* tcp_find_listener(uint16_t port, int af) {
    for (int i = 0; i < TCP_MAX_CONN; i++) {
        if (!tcp_conns[i].used) continue;
        if (tcp_conns[i].state != TCP_LISTEN) continue;
        if (tcp_conns[i].local_port != port) continue;
        /* Address family matching:
         * - AF_INET6 listener with ipv6only=0 matches both families (dual-stack)
         * - AF_INET6 listener with ipv6only=1 matches only AF_INET6
         * - AF_INET listener matches only AF_INET */
        if (tcp_conns[i].af == AF_INET6 && tcp_conns[i].ipv6only && af == AF_INET)
            continue;
        if (tcp_conns[i].af == AF_INET && af == AF_INET6)
            continue;
        return &tcp_conns[i];
    }
    return NULL;
}

static int tcp_send_pkt(tcp_conn_t* conn, uint8_t flags,
                         const uint8_t* data, uint32_t len) {
    uint32_t total = TCP_HDR_LEN + len;
    uint8_t buf[total];
    tcp_hdr_t* hdr = (tcp_hdr_t*)buf;

    hdr->src_port = tcp_htons(conn->local_port);
    hdr->dst_port = tcp_htons(conn->remote_port);
    hdr->seq = tcp_htonl(conn->snd_nxt);
    hdr->ack = tcp_htonl(conn->rcv_nxt);
    hdr->offset = 0x50;
    hdr->flags = flags;
    hdr->window = tcp_htons(TCP_WINDOW);
    hdr->checksum = 0;
    hdr->urgent = 0;

    if (len > 0) kmemcpy(buf + TCP_HDR_LEN, data, len);

    if (conn->af == AF_INET) {
        hdr->checksum = tcp_checksum_pseudo(buf, total, &conn->local_ip.v4,
                                              &conn->remote_ip.v4, AF_INET);
        KDEBUG("[TCP] TX flags=0x%02x seq=%u ack=%u csum=0x%04x data=%u\n",
                flags, conn->snd_nxt, conn->rcv_nxt, hdr->checksum, len);
        return ipv4_send(conn->remote_ip.v4, IPV4_PROTO_TCP, buf, total);
    } else {
        hdr->checksum = tcp_checksum_pseudo(buf, total, conn->local_ip.v6,
                                              conn->remote_ip.v6, AF_INET6);
        return ipv6_send(conn->remote_ip.v6, IPV6_NEXT_TCP, buf, total);
    }
}

static void tcp_handle_common(int af, const void* src_ip, const void* dst_ip,
                               const uint8_t* data, uint32_t len) {
    cpu_flags_t flags;
    spinlock_acquire(&tcp_lock, &flags);

    if (len < TCP_HDR_LEN) { spinlock_release(&tcp_lock, flags); return; }
    const tcp_hdr_t* hdr = (const tcp_hdr_t*)data;
    uint32_t hdr_len = (hdr->offset >> 4) * 4;
    if (hdr_len < TCP_HDR_LEN || hdr_len > len) { spinlock_release(&tcp_lock, flags); return; }
    uint16_t dst_port = tcp_htons(hdr->dst_port);
    uint16_t src_port = tcp_htons(hdr->src_port);
    uint32_t seq = tcp_htonl(hdr->seq);
    uint32_t ack = tcp_htonl(hdr->ack);
    uint8_t tcp_flags = hdr->flags;
    uint32_t payload_len = len - hdr_len;
    const uint8_t* payload = data + hdr_len;
    uint16_t window = tcp_htons(hdr->window);

    KDEBUG("[TCP RX] flags=0x%02x sport=%u dport=%u seq=%u ack=%u len=%u\n",
            tcp_flags, src_port, dst_port, seq, ack, payload_len);

    tcp_conn_t* conn = tcp_find_conn(af, src_ip, src_port, dst_port);
    if (!conn) {
        /* Check for a LISTEN connection */
        tcp_conn_t* listener = tcp_find_listener(dst_port, af);
        if (listener && (tcp_flags & TCP_SYN)) {
            /* Accept new connection */
            tcp_conn_t* child = NULL;
            for (int i = 0; i < TCP_MAX_CONN; i++) {
                if (!tcp_conns[i].used) { child = &tcp_conns[i]; break; }
            }
            if (!child) {
                kprintf("[TCP] No connection slots for incoming\n");
                spinlock_release(&tcp_lock, flags);
                return;
            }
            child->used = 1;
            child->af = af;
            child->state = TCP_SYN_RECEIVED;
            child->local_port = dst_port;
            child->remote_port = src_port;
            child->snd_una = 0;
            child->snd_wnd = TCP_WINDOW;
            child->rcv_nxt = seq + 1;
            child->iss = 0;
            child->irs = seq;
            child->closed = 0;
            if (af == AF_INET) {
                child->remote_ip.v4 = *(const ipv4_addr_t*)src_ip;
                child->local_ip.v4 = *(const ipv4_addr_t*)dst_ip;
            } else {
                kmemcpy(child->remote_ip.v6, src_ip, 16);
                kmemcpy(child->local_ip.v6, dst_ip, 16);
            }
            child->on_recv = NULL;
            child->on_close = NULL;
            child->on_connect = listener->on_connect;
            child->recv_done = 0;
            child->recv_len = 0;
            child->timewait_ms = 0;
            child->retrans_len = 0;
            child->rto_remaining = 0;
            child->rto_ms = 1000;
            child->fin_rto_remaining = 0;

            uint32_t iss;
            __asm__ volatile("rdtsc" : "=a"(iss) : : "edx");
            child->snd_nxt = iss;
            child->iss = iss;

            kprintf("[TCP] SYN+ACK: rcv_nxt=%u snd_nxt=%u\n",
                    child->rcv_nxt, child->snd_nxt);
            tcp_send_pkt(child, TCP_SYN | TCP_ACK, NULL, 0);
            child->snd_nxt++;
            kprintf("[TCP] SYN+ACK sent for incoming connection to port %u\n",
                    dst_port);

            /* Enqueue on listener's accept queue */
            if (listener->accept_count < TCP_BACKLOG) {
                listener->accept_queue[listener->accept_tail] = child;
                listener->accept_tail = (listener->accept_tail + 1) % TCP_BACKLOG;
                listener->accept_count++;
            }
            spinlock_release(&tcp_lock, flags);
            return;
        }

        if ((tcp_flags & TCP_SYN) && dst_port < 49152) {
            kprintf("[TCP] Connection refused to port %u\n", dst_port);
            uint8_t rst_buf[TCP_HDR_LEN];
            tcp_hdr_t* rst = (tcp_hdr_t*)rst_buf;
            rst->src_port = tcp_htons(dst_port);
            rst->dst_port = tcp_htons(src_port);
            rst->seq = 0;
            rst->ack = tcp_htonl(seq + 1);
            rst->offset = 0x50;
            rst->flags = TCP_RST | TCP_ACK;
            rst->window = 0;
            rst->checksum = 0;
            rst->urgent = 0;
            if (af == AF_INET) {
                ipv4_addr_t my_ip4 = ipv4_get_addr();
                rst->checksum = tcp_checksum_pseudo(rst_buf, TCP_HDR_LEN,
                                                     &my_ip4, src_ip, AF_INET);
                ipv4_send(*(const ipv4_addr_t*)src_ip, IPV4_PROTO_TCP,
                           rst_buf, TCP_HDR_LEN);
            } else {
                uint8_t my_ip6[16];
                ipv6_get_lladdr(my_ip6);
                rst->checksum = tcp_checksum_pseudo(rst_buf, TCP_HDR_LEN,
                                                     my_ip6, src_ip, AF_INET6);
                ipv6_send(src_ip, IPV6_NEXT_TCP, rst_buf, TCP_HDR_LEN);
            }
        }
        spinlock_release(&tcp_lock, flags);
        return;
    }

    /* Save callbacks before potentially releasing the lock */
    void (*on_connect_cb)(tcp_conn_t*) = NULL;
    void (*on_recv_cb)(tcp_conn_t*, const uint8_t*, uint32_t) = NULL;
    uint32_t recv_data_len = 0;
    uint8_t recv_copy[TCP_MSS];
    void (*on_close_cb)(tcp_conn_t*) = NULL;

    /* Generic RST handling for all states (except LISTEN).
       We keep used=1 so tcp_find_conn still finds this connection
       (needed for tcp_conn_connect's poll loop to detect the RST). */
    if (tcp_flags & TCP_RST) {
        if (conn->state != TCP_LISTEN) {
            on_close_cb = conn->on_close;
            conn->state = TCP_CLOSED;
            conn->closed = 1;
        }
        spinlock_release(&tcp_lock, flags);
        if (on_close_cb) on_close_cb(conn);
        return;
    }

    /* Flag for SYN_RECEIVED data that must be processed after on_connect */
    int syn_recv_deferred = 0;

    switch (conn->state) {
    case TCP_SYN_SENT:
        if ((tcp_flags & TCP_SYN) && (tcp_flags & TCP_ACK)) {
            kprintf("[TCP] SYN+ACK check: ack=%u snd_nxt=%u seq=%u rcv_nxt=%u\n",
                    ack, conn->snd_nxt, seq, conn->rcv_nxt);
            if (ack == conn->snd_nxt) {
                conn->state = TCP_ESTABLISHED;
                conn->rcv_nxt = seq + 1;
                conn->irs = seq;
                tcp_send_pkt(conn, TCP_ACK, NULL, 0);
                kprintf("[TCP] Connection established (%s)",
                        af == AF_INET ? "IPv4" : "IPv6");
                on_connect_cb = conn->on_connect;
            }
        }
        break;

    case TCP_SYN_RECEIVED:
        if (tcp_flags & TCP_ACK) {
            conn->state = TCP_ESTABLISHED;
            kprintf("[TCP] Incoming connection established (%s)\n",
                    af == AF_INET ? "IPv4" : "IPv6");
            on_connect_cb = conn->on_connect;
            /* Don't goto process_established_data here — on_connect must
               be called first to set on_recv. Save data for later. */
            if (payload_len > 0) {
                uint32_t copy = payload_len < TCP_MSS ? payload_len : TCP_MSS;
                kmemcpy(recv_copy, payload, copy);
                recv_data_len = copy;
                conn->rcv_nxt = seq + payload_len;
                tcp_send_pkt(conn, TCP_ACK, NULL, 0);
                kmemcpy(conn->recv_buf, recv_copy, copy);
                conn->recv_len = copy;
                conn->recv_done = 1;
                syn_recv_deferred = 1;
            }
        }
        break;

    case TCP_ESTABLISHED:
        if (tcp_flags & TCP_FIN) {
            conn->rcv_nxt = seq + 1;
            conn->state = TCP_CLOSE_WAIT;
            tcp_send_pkt(conn, TCP_ACK, NULL, 0);
            kprintf("[TCP] FIN received, entering CLOSE_WAIT\n");
            on_close_cb = conn->on_close;
            break;
        }
        if (payload_len > 0) {
            if (seq == conn->rcv_nxt && !conn->recv_done) {
                conn->rcv_nxt = seq + payload_len;
                tcp_send_pkt(conn, TCP_ACK, NULL, 0);
                on_recv_cb = conn->on_recv;
                recv_data_len = payload_len;
                uint32_t copy = payload_len < TCP_MSS ? payload_len : TCP_MSS;
                kmemcpy(recv_copy, payload, copy);
                kmemcpy(conn->recv_buf, recv_copy, copy);
                conn->recv_len = copy;
                conn->recv_done = 1;
            } else if (seq != conn->rcv_nxt) {
                tcp_send_pkt(conn, TCP_ACK, NULL, 0);
            }
        }
        /* ACK processing: track peer's advertised window, update snd_una,
           and clear retransmission buffer when data is acknowledged */
        if (tcp_flags & TCP_ACK) {
            conn->snd_wnd = window;
            if (ack > conn->snd_una) {
                conn->snd_una = ack;
            }
            if (conn->retrans_len > 0 &&
                ack >= conn->retrans_seq + conn->retrans_len) {
                conn->retrans_len = 0;
                conn->rto_remaining = 0;
                conn->rto_ms = 1000;
            }
        }
        break;

    case TCP_FIN_WAIT1:
        if ((tcp_flags & TCP_FIN) && (tcp_flags & TCP_ACK)) {
            conn->rcv_nxt = seq + 1;
            conn->state = TCP_TIME_WAIT;
            conn->timewait_ms = 60000;
            tcp_send_pkt(conn, TCP_ACK, NULL, 0);
            conn->fin_rto_remaining = 0;
        } else if (tcp_flags & TCP_FIN) {
            conn->rcv_nxt = seq + 1;
            conn->state = TCP_CLOSING;
            tcp_send_pkt(conn, TCP_ACK, NULL, 0);
        } else if (tcp_flags & TCP_ACK) {
            conn->state = TCP_FIN_WAIT2;
            conn->fin_rto_remaining = 0;
        }
        break;

    case TCP_FIN_WAIT2:
        if (tcp_flags & TCP_FIN) {
            conn->rcv_nxt = seq + 1;
            conn->state = TCP_TIME_WAIT;
            conn->timewait_ms = 60000;
            tcp_send_pkt(conn, TCP_ACK, NULL, 0);
        }
        break;

    case TCP_CLOSE_WAIT:
        break;

    case TCP_CLOSING:
        if (tcp_flags & TCP_ACK) conn->state = TCP_TIME_WAIT;
        break;

    case TCP_LAST_ACK:
        if (tcp_flags & TCP_ACK) {
            conn->state = TCP_CLOSED;
            conn->used = 0;
            conn->fin_rto_remaining = 0;
            kprintf("[TCP] Connection closed\n");
        }
        break;

    case TCP_TIME_WAIT:
        /* Start 2MSL timer (60s). tcp_tick() frees when it expires. */
        conn->timewait_ms = 60000;
        break;

    default:
        break;
    }

    spinlock_release(&tcp_lock, flags);

    /* Callbacks without the lock */
    if (on_connect_cb) on_connect_cb(conn);
    /* If SYN_RECEIVED carried data, process it now — on_connect has set on_recv */
    if (syn_recv_deferred) {
        void (*recv_cb)(tcp_conn_t*, const uint8_t*, uint32_t) = conn->on_recv;
        if (recv_cb) recv_cb(conn, recv_copy, recv_data_len);
    }
    if (on_close_cb) on_close_cb(conn);
    if (on_recv_cb) on_recv_cb(conn, recv_copy, recv_data_len);
}

static void tcp_ipv4_handler(ipv4_addr_t src, ipv4_addr_t dst,
                               uint8_t protocol,
                               const uint8_t* data, uint32_t len) {
    (void)protocol;
    tcp_handle_common(AF_INET, &src, &dst, data, len);
}

static void tcp_ipv6_handler(const uint8_t* src, const uint8_t* dst,
                               uint8_t next_header,
                               const uint8_t* data, uint32_t len) {
    (void)next_header;
    tcp_handle_common(AF_INET6, src, dst, data, len);
}

tcp_conn_t* tcp_listen(int af, uint16_t port,
                        void (*on_connect)(tcp_conn_t* conn)) {
    if (!tcp_initialized) return NULL;

    cpu_flags_t flags;
    spinlock_acquire(&tcp_lock, &flags);

    tcp_conn_t* conn = NULL;
    for (int i = 0; i < TCP_MAX_CONN; i++) {
        if (!tcp_conns[i].used) { conn = &tcp_conns[i]; break; }
    }
    if (!conn) { spinlock_release(&tcp_lock, flags); return NULL; }

    conn->used = 1;
    conn->af = af;
    conn->state = TCP_LISTEN;
    conn->local_port = port;
    conn->on_connect = on_connect;
    conn->on_recv = NULL;
    conn->on_close = NULL;

    spinlock_release(&tcp_lock, flags);
    kprintf("[TCP] Listening on port %u\n", port);
    return conn;
}

tcp_conn_t* tcp_connect(int af, const void* dst_ip, uint16_t dst_port,
                         uint16_t src_port, int timeout_ms) {
    if (!tcp_initialized) return NULL;

    cpu_flags_t flags;
    spinlock_acquire(&tcp_lock, &flags);

    tcp_conn_t* conn = NULL;
    for (int i = 0; i < TCP_MAX_CONN; i++) {
        if (!tcp_conns[i].used) { conn = &tcp_conns[i]; break; }
    }
    if (!conn) { spinlock_release(&tcp_lock, flags); return NULL; }

    if (src_port == 0) src_port = tcp_ephemeral_port++;

    conn->used = 1;
    conn->af = af;
    conn->state = TCP_SYN_SENT;
    conn->local_port = src_port;
    conn->remote_port = dst_port;
    conn->snd_una = 0;
    conn->rcv_nxt = 0;
    conn->closed = 0;
    conn->on_recv = NULL;
    conn->on_close = NULL;
    conn->on_connect = NULL;

    uint32_t iss;
    __asm__ volatile("rdtsc" : "=a"(iss) : : "edx");
    conn->iss = iss;
    conn->snd_nxt = iss;

    if (af == AF_INET)
        conn->remote_ip.v4 = *(const ipv4_addr_t*)dst_ip;
    else
        kmemcpy(conn->remote_ip.v6, dst_ip, 16);

    int e = tcp_send_pkt(conn, TCP_SYN, NULL, 0);
    if (e != ERR_OK) { conn->used = 0; spinlock_release(&tcp_lock, flags); kprintf("[TCP] SYN send failed: %d\n", e); return NULL; }
    conn->snd_nxt++;

    spinlock_release(&tcp_lock, flags);
    kprintf("[TCP] SYN sent (ISS=%u), connection pending...\n", iss);

    if (timeout_ms <= 5000) timeout_ms = 30000;

    int step = 50;
    int steps = timeout_ms / step;
    for (int i = 0; i < steps &&
         conn->state != TCP_ESTABLISHED &&
         conn->state != TCP_CLOSED; i++) {
        eth_rx_poll();
        thread_sleep((uint64_t)step);
    }

    if (conn->state == TCP_ESTABLISHED) return conn;

    if (conn->state == TCP_CLOSED)
        kprintf("[TCP] Connection refused\n");
    else
        kprintf("[TCP] Connection timeout\n");
    conn->used = 0;
    return NULL;

    return conn;
}

int tcp_send(tcp_conn_t* conn, const uint8_t* data, uint32_t len) {
    if (!conn || !conn->used) return ERR_INVAL;
    if (conn->state != TCP_ESTABLISHED) return ERR_AGAIN;

    int timeout_ms = conn->send_timeout > 0 ? conn->send_timeout : 5000;

    cpu_flags_t flags;
    spinlock_acquire(&tcp_lock, &flags);

    /* If retransmit buffer is still busy (previous data un-ACKed), wait
     * with SO_SNDTIMEO. This prevents overwriting un-ACKed data and makes
     * SO_SNDTIMEO meaningful for TCP. */
    int step = 50;
    int steps = timeout_ms / step;
    int waited = 0;
    while (conn->retrans_len > 0 && conn->state == TCP_ESTABLISHED) {
        if (waited >= steps) {
            spinlock_release(&tcp_lock, flags);
            return ERR_TIMEOUT;
        }
        spinlock_release(&tcp_lock, flags);
        eth_rx_poll();
        thread_sleep((uint64_t)step);
        spinlock_acquire(&tcp_lock, &flags);
        waited++;
    }

    if (conn->state != TCP_ESTABLISHED) {
        spinlock_release(&tcp_lock, flags);
        return ERR_AGAIN;
    }

    uint32_t remaining = len;
    uint32_t offset = 0;

    while (remaining > 0) {
        uint32_t chunk = remaining > TCP_MSS ? TCP_MSS : remaining;
        uint8_t tcp_flags = TCP_ACK;
        if (chunk == remaining) tcp_flags |= TCP_PSH;

        /* Release lock around tcp_send_pkt to prevent deadlock when
         * ARP/NDP resolution triggers eth_rx_poll() which may re-enter TCP. */
        spinlock_release(&tcp_lock, flags);
        int e = tcp_send_pkt(conn, tcp_flags, data + offset, chunk);
        spinlock_acquire(&tcp_lock, &flags);

        /* Re-check connection after lock re-acquire */
        if (e != ERR_OK || !conn->used ||
            conn->state != TCP_ESTABLISHED) {
            spinlock_release(&tcp_lock, flags);
            return (e != ERR_OK) ? e : ERR_AGAIN;
        }

        conn->snd_nxt += chunk;

        /* Buffer the most recent chunk for retransmission */
        kmemcpy(conn->retrans_buf, data + offset, chunk);
        conn->retrans_seq = conn->snd_nxt - chunk;
        conn->retrans_len = chunk;
        conn->rto_ms = 1000;
        conn->rto_remaining = 1000;

        offset += chunk;
        remaining -= chunk;
    }

    spinlock_release(&tcp_lock, flags);
    return (int)len;
}

int tcp_close(tcp_conn_t* conn) {
    if (!conn || !conn->used) return ERR_INVAL;

    cpu_flags_t flags;
    spinlock_acquire(&tcp_lock, &flags);

    if (conn->state == TCP_ESTABLISHED || conn->state == TCP_CLOSE_WAIT) {
        conn->state = conn->state == TCP_ESTABLISHED ?
                      TCP_FIN_WAIT1 : TCP_LAST_ACK;
        conn->snd_nxt++;
        conn->fin_rto_remaining = 1000;
        spinlock_release(&tcp_lock, flags);
        tcp_send_pkt(conn, TCP_FIN | TCP_ACK, NULL, 0);
        return ERR_OK;
    }

    conn->state = TCP_CLOSED;
    conn->used = 0;
    spinlock_release(&tcp_lock, flags);
    return ERR_OK;
}

void tcp_tick(void) {
    cpu_flags_t flags;
    spinlock_acquire(&tcp_lock, &flags);
    for (int i = 0; i < TCP_MAX_CONN; i++) {
        tcp_conn_t* c = &tcp_conns[i];
        if (!c->used) continue;

        /* TIME_WAIT 2MSL timer */
        if (c->state == TCP_TIME_WAIT) {
            if (c->timewait_ms <= 10) {
                c->state = TCP_CLOSED;
                c->used = 0;
            } else {
                c->timewait_ms -= 10;
            }
            continue;
        }

        /* Data retransmission RTO */
        if (c->retrans_len > 0 && c->state == TCP_ESTABLISHED) {
            if (c->rto_remaining <= 10) {
                uint8_t rbuf[TCP_MSS];
                uint32_t rlen = c->retrans_len;
                uint32_t rseq = c->retrans_seq;
                kmemcpy(rbuf, c->retrans_buf, rlen);
                c->rto_ms = (c->rto_ms * 2 > 60000) ? 60000 : c->rto_ms * 2;
                c->rto_remaining = c->rto_ms;

                spinlock_release(&tcp_lock, flags);
                tcp_send_pkt(c, TCP_ACK | TCP_PSH, rbuf, rlen);
                spinlock_acquire(&tcp_lock, &flags);

                /* Re-check connection after lock re-acquire */
                if (!c->used || c->state != TCP_ESTABLISHED || c->retrans_len == 0)
                    continue;
                c->snd_nxt = rseq + rlen;
            } else {
                c->rto_remaining -= 10;
            }
        }

        /* FIN retransmission */
        if ((c->state == TCP_FIN_WAIT1 || c->state == TCP_LAST_ACK) &&
            c->fin_rto_remaining > 0) {
            if (c->fin_rto_remaining <= 10) {
                c->fin_rto_ms = (c->fin_rto_ms * 2 > 60000) ? 60000 : (c->fin_rto_ms ? c->fin_rto_ms * 2 : 2000);
                c->fin_rto_remaining = c->fin_rto_ms;

                spinlock_release(&tcp_lock, flags);
                tcp_send_pkt(c, TCP_FIN | TCP_ACK, NULL, 0);
                spinlock_acquire(&tcp_lock, &flags);

                /* Re-check connection after lock re-acquire */
                if (!c->used ||
                    (c->state != TCP_FIN_WAIT1 && c->state != TCP_LAST_ACK))
                    continue;
                c->snd_nxt++;
            } else {
                c->fin_rto_remaining -= 10;
            }
        }
    }
    spinlock_release(&tcp_lock, flags);
}

void tcp_init(void) {
    if (tcp_initialized) return;

    spinlock_init(&tcp_lock, "tcp");
    tcp_ephemeral_port = 49152;
    ipv4_register_handler(IPV4_PROTO_TCP, tcp_ipv4_handler);
    ipv6_register_handler(IPV6_NEXT_TCP, tcp_ipv6_handler);
    kprintf("[TCP] Initialized\n");

    tcp_initialized = 1;
}

/* ---- Stable transport API ---- */

tcp_conn_t* tcp_conn_create(int af) {
    if (!tcp_initialized) return NULL;
    cpu_flags_t flags;
    spinlock_acquire(&tcp_lock, &flags);
    for (int i = 0; i < TCP_MAX_CONN; i++) {
        if (!tcp_conns[i].used) {
            kmemset(&tcp_conns[i], 0, sizeof(tcp_conn_t));
            tcp_conns[i].used = 1;
            tcp_conns[i].af = af;
            tcp_conns[i].state = TCP_CLOSED;
            tcp_conns[i].recv_timeout = 5000;
            tcp_conns[i].send_timeout = 5000;
            spinlock_release(&tcp_lock, flags);
            return &tcp_conns[i];
        }
    }
    spinlock_release(&tcp_lock, flags);
    return NULL;
}

void tcp_conn_destroy(tcp_conn_t* c) {
    if (!c) return;
    cpu_flags_t flags;
    spinlock_acquire(&tcp_lock, &flags);
    c->used = 0;
    c->state = TCP_CLOSED;
    spinlock_release(&tcp_lock, flags);
}

int tcp_conn_bind(tcp_conn_t* c, uint16_t port, int ipv6only) {
    if (!c || !c->used) return ERR_INVAL;
    cpu_flags_t flags;
    spinlock_acquire(&tcp_lock, &flags);

    /* Check same-family conflict */
    for (int i = 0; i < TCP_MAX_CONN; i++) {
        if (i != (int)(c - tcp_conns) && tcp_conns[i].used &&
            tcp_conns[i].local_port == port &&
            tcp_conns[i].state != TCP_CLOSED &&
            tcp_conns[i].af == c->af) {
            spinlock_release(&tcp_lock, flags);
            return ERR_BUSY;
        }
    }

    /* Cross-family conflict: dual-stack IPv6 conflicts with IPv4 on same port */
    if (c->af == AF_INET6 && !ipv6only) {
        for (int i = 0; i < TCP_MAX_CONN; i++) {
            if (tcp_conns[i].used &&
                tcp_conns[i].af == AF_INET &&
                tcp_conns[i].local_port == port &&
                tcp_conns[i].state != TCP_CLOSED) {
                spinlock_release(&tcp_lock, flags);
                return ERR_BUSY;
            }
        }
    }
    if (c->af == AF_INET) {
        for (int i = 0; i < TCP_MAX_CONN; i++) {
            if (tcp_conns[i].used &&
                tcp_conns[i].af == AF_INET6 &&
                tcp_conns[i].local_port == port &&
                tcp_conns[i].state != TCP_CLOSED &&
                !tcp_conns[i].ipv6only) {
                spinlock_release(&tcp_lock, flags);
                return ERR_BUSY;
            }
        }
    }

    c->local_port = port;
    c->ipv6only = ipv6only;
    spinlock_release(&tcp_lock, flags);
    return ERR_OK;
}

int tcp_conn_connect(tcp_conn_t* c, const void* dst_ip,
                     uint16_t dst_port, int timeout_ms) {
    if (!c || !c->used) return ERR_INVAL;

    cpu_flags_t flags;
    spinlock_acquire(&tcp_lock, &flags);

    if (c->local_port == 0)
        c->local_port = tcp_ephemeral_port++;
    c->state = TCP_SYN_SENT;
    c->remote_port = dst_port;
    if (c->af == AF_INET) {
        c->remote_ip.v4 = *(const ipv4_addr_t*)dst_ip;
        c->local_ip.v4 = ipv4_get_addr();
    } else {
        kmemcpy(c->remote_ip.v6, dst_ip, 16);
        ipv6_get_lladdr(c->local_ip.v6);
    }

    uint32_t iss;
    __asm__ volatile("rdtsc" : "=a"(iss) : : "edx");
    c->iss = iss;
    c->snd_nxt = iss;

    spinlock_release(&tcp_lock, flags);
    int e = tcp_send_pkt(c, TCP_SYN, NULL, 0);
    spinlock_acquire(&tcp_lock, &flags);
    if (e != ERR_OK || !c->used) { c->used = 0; spinlock_release(&tcp_lock, flags); return e; }
    c->snd_nxt++;

    spinlock_release(&tcp_lock, flags);
    kprintf("[TCP] SYN sent (ISS=%u), connection pending...\n", iss);

    if (timeout_ms < 0) timeout_ms = 0;
    if (timeout_ms == 0) {
        /* Non-blocking: just poll once */
        eth_rx_poll();
        spinlock_acquire(&tcp_lock, &flags);
        int connected = (c->state == TCP_ESTABLISHED);
        if (c->state == TCP_CLOSED) { c->used = 0; }
        spinlock_release(&tcp_lock, flags);
        return connected ? ERR_OK : ERR_AGAIN;
    }
    if (c->af == AF_INET6 && timeout_ms < 30000) timeout_ms = 30000;

    int step = 50;
    int steps = timeout_ms / step;
    int retry_interval = 5000 / step;
    for (int i = 0; i < steps; i++) {
        eth_rx_poll();

        spinlock_acquire(&tcp_lock, &flags);
        if (c->state == TCP_ESTABLISHED) {
            spinlock_release(&tcp_lock, flags);
            return ERR_OK;
        }
        if (c->state == TCP_SYN_SENT && i > 0 && (i % retry_interval) == 0) {
            kprintf("[TCP] SYN retry #%d\n", i / retry_interval);
            c->snd_nxt = c->iss;
            spinlock_release(&tcp_lock, flags);
            tcp_send_pkt(c, TCP_SYN, NULL, 0);
            spinlock_acquire(&tcp_lock, &flags);
            if (!c->used) { spinlock_release(&tcp_lock, flags); return ERR_AGAIN; }
            c->snd_nxt = c->iss + 1;
        }
        if (!c->used) { spinlock_release(&tcp_lock, flags); return ERR_AGAIN; }
        if (c->state == TCP_CLOSED) {
            c->used = 0;
            spinlock_release(&tcp_lock, flags);
            return ERR_AGAIN;
        }
        spinlock_release(&tcp_lock, flags);

        thread_sleep((uint64_t)step);
    }

    spinlock_acquire(&tcp_lock, &flags);
    int connected = (c->state == TCP_ESTABLISHED);
    spinlock_release(&tcp_lock, flags);
    return connected ? ERR_OK : ERR_TIMEOUT;
}

int tcp_conn_listen(tcp_conn_t* c, uint16_t port) {
    if (!c || !c->used) return ERR_INVAL;
    cpu_flags_t flags;
    spinlock_acquire(&tcp_lock, &flags);
    c->state = TCP_LISTEN;
    c->local_port = port;
    c->accept_head = 0;
    c->accept_tail = 0;
    c->accept_count = 0;
    spinlock_release(&tcp_lock, flags);
    kprintf("[TCP] Listening on port %u\n", port);
    return ERR_OK;
}

tcp_conn_t* tcp_conn_accept(tcp_conn_t* c, int timeout_ms) {
    if (!c || !c->used || c->state != TCP_LISTEN) return NULL;

    int step = 50;
    int steps = timeout_ms / step;
    for (int i = 0; i < steps; i++) {
        eth_rx_poll();
        cpu_flags_t flags;
        spinlock_acquire(&tcp_lock, &flags);
        if (c->accept_count > 0) {
            tcp_conn_t* child = c->accept_queue[c->accept_head];
            c->accept_head = (c->accept_head + 1) % TCP_BACKLOG;
            c->accept_count--;
            spinlock_release(&tcp_lock, flags);
            return child;
        }
        spinlock_release(&tcp_lock, flags);
        thread_sleep((uint64_t)step);
    }
    return NULL;
}

int tcp_conn_send(tcp_conn_t* c, const uint8_t* data, uint32_t len) {
    return tcp_send(c, data, len);
}

int tcp_conn_recv(tcp_conn_t* c, uint8_t* buf, uint32_t size, int timeout_ms) {
    if (!c || !c->used) return ERR_INVAL;
    if (c->state != TCP_ESTABLISHED && c->state != TCP_CLOSE_WAIT)
        return ERR_AGAIN;

    int step = 50;
    int steps = timeout_ms / step;
    for (int i = 0; i < steps; i++) {
        eth_rx_poll();
        cpu_flags_t flags;
        spinlock_acquire(&tcp_lock, &flags);
        if (c->recv_done) {
            uint32_t copy = c->recv_len < size ? c->recv_len : size;
            kmemcpy(buf, c->recv_buf, copy);
            c->recv_done = 0;
            c->recv_len = 0;
            spinlock_release(&tcp_lock, flags);
            return (int)copy;
        }
        if (c->state == TCP_CLOSED) { spinlock_release(&tcp_lock, flags); return 0; }
        spinlock_release(&tcp_lock, flags);
        thread_sleep((uint64_t)step);
    }
    return ERR_TIMEOUT;
}

int tcp_conn_get_state(tcp_conn_t* c) {
    if (!c || !c->used) return TCP_CLOSED;
    cpu_flags_t flags;
    spinlock_acquire(&tcp_lock, &flags);
    int st = c->state;
    spinlock_release(&tcp_lock, flags);
    return st;
}

int tcp_conn_close(tcp_conn_t* c) {
    return tcp_close(c);
}

#ifdef NET_SELF_TEST
tcp_conn_t* tcp_test_add_conn(int af, const void* remote_ip,
                              uint16_t remote_port, uint16_t local_port) {
    cpu_flags_t flags;
    spinlock_acquire(&tcp_lock, &flags);
    for (int i = 0; i < TCP_MAX_CONN; i++) {
        if (!tcp_conns[i].used) {
            tcp_conns[i].used = 1;
            tcp_conns[i].af = af;
            tcp_conns[i].state = TCP_SYN_SENT;
            tcp_conns[i].local_port = local_port;
            tcp_conns[i].remote_port = remote_port;
            if (af == AF_INET)
                tcp_conns[i].remote_ip.v4 = *(const ipv4_addr_t*)remote_ip;
            else
                kmemcpy(tcp_conns[i].remote_ip.v6, remote_ip, 16);
            spinlock_release(&tcp_lock, flags);
            return &tcp_conns[i];
        }
    }
    spinlock_release(&tcp_lock, flags);
    return NULL;
}
#endif

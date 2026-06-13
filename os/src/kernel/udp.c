#include "kernel.h"
#include "udp.h"
#include "ipv4.h"
#include "ipv6.h"
#include "route.h"
#include "nic.h"
#include "ndp.h"
#include "sched.h"
#include "eth.h"
#include "sync.h"

static int udp_initialized = 0;
static udp_endpoint_t udp_endpoints[UDP_MAX_ENDPOINTS];
static spinlock_t udp_lock;

static uint16_t udp_htons(uint16_t v) { return __builtin_bswap16(v); }

static uint16_t udp_checksum_v4(const void* udp_pkt, uint32_t udp_len,
                                 ipv4_addr_t src, ipv4_addr_t dst) {
    uint32_t sum = 0;
    const uint16_t* p;

    p = (const uint16_t*)src.bytes;
    for (int i = 0; i < 2; i++) sum += __builtin_bswap16(p[i]);
    p = (const uint16_t*)dst.bytes;
    for (int i = 0; i < 2; i++) sum += __builtin_bswap16(p[i]);
    sum += (uint16_t)17;
    sum += udp_len;

    p = (const uint16_t*)udp_pkt;
    for (uint32_t i = 0; i < udp_len / 2; i++) sum += __builtin_bswap16(p[i]);
    if (udp_len & 1)
        sum += (uint16_t)((const uint8_t*)udp_pkt)[udp_len - 1] << 8;

    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return __builtin_bswap16(~sum & 0xFFFF);
}

static uint16_t udp_checksum_v6(const void* udp_pkt, uint32_t udp_len,
                                 const uint8_t* src, const uint8_t* dst) {
    uint32_t sum = 0;
    const uint16_t* p;

    p = (const uint16_t*)src;
    for (int i = 0; i < 8; i++) sum += __builtin_bswap16(p[i]);
    p = (const uint16_t*)dst;
    for (int i = 0; i < 8; i++) sum += __builtin_bswap16(p[i]);
    sum += udp_len;
    sum += (uint16_t)17;

    p = (const uint16_t*)udp_pkt;
    for (uint32_t i = 0; i < udp_len / 2; i++) sum += __builtin_bswap16(p[i]);
    if (udp_len & 1)
        sum += (uint16_t)((const uint8_t*)udp_pkt)[udp_len - 1] << 8;

    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return __builtin_bswap16(~sum & 0xFFFF);
}

int udp_sendto(int af, const void* dst_ip, uint16_t dst_port,
               uint16_t src_port,
               const uint8_t* data, uint32_t len) {
    if (!udp_initialized) return ERR_NOSYS;

    uint32_t total = UDP_HDR_LEN + len;
    uint8_t buf[total];
    udp_hdr_t* hdr = (udp_hdr_t*)buf;

    hdr->src_port = udp_htons(src_port);
    hdr->dst_port = udp_htons(dst_port);
    hdr->length = udp_htons(total);
    hdr->checksum = 0;
    kmemcpy(buf + UDP_HDR_LEN, data, len);

    int e;
    if (af == AF_INET) {
        ipv4_addr_t dst = *(const ipv4_addr_t*)dst_ip;
        ipv4_addr_t my_ip = ipv4_get_addr();
        hdr->checksum = udp_checksum_v4(buf, total, my_ip, dst);
        e = ipv4_send(dst, 17, buf, total);
    } else {
        uint8_t my_ip[16];
        ndp_make_lladdr(nic.mac, my_ip);
        hdr->checksum = udp_checksum_v6(buf, total, my_ip,
                                         (const uint8_t*)dst_ip);
        e = ipv6_send((const uint8_t*)dst_ip, 17, buf, total);
    }

    if (e != ERR_OK) return e;
    return (int)len;
}

/* ---- Endpoint API ---- */

int udp_bind_endpoint(int af, const uint8_t* addr, uint16_t port,
                       int recv_timeout, int send_timeout, int ipv6only) {
    if (!udp_initialized) return ERR_NOSYS;
    cpu_flags_t flags;
    spinlock_acquire(&udp_lock, &flags);

    /* Check same-family conflict */
    for (int i = 0; i < UDP_MAX_ENDPOINTS; i++) {
        if (udp_endpoints[i].used &&
            udp_endpoints[i].af == af &&
            udp_endpoints[i].port == port) {
            spinlock_release(&udp_lock, flags);
            return ERR_BUSY;
        }
    }

    /* Cross-family conflict: IPv6 dual-stack (ipv6only=0) conflicts with
     * IPv4 on the same port, and vice-versa. */
    if (af == AF_INET6 && !ipv6only) {
        for (int i = 0; i < UDP_MAX_ENDPOINTS; i++) {
            if (udp_endpoints[i].used &&
                udp_endpoints[i].af == AF_INET &&
                udp_endpoints[i].port == port) {
                spinlock_release(&udp_lock, flags);
                return ERR_BUSY;
            }
        }
    }
    if (af == AF_INET) {
        for (int i = 0; i < UDP_MAX_ENDPOINTS; i++) {
            if (udp_endpoints[i].used &&
                udp_endpoints[i].af == AF_INET6 &&
                udp_endpoints[i].port == port &&
                !udp_endpoints[i].ipv6only) {
                spinlock_release(&udp_lock, flags);
                return ERR_BUSY;
            }
        }
    }

    for (int i = 0; i < UDP_MAX_ENDPOINTS; i++) {
        if (!udp_endpoints[i].used) {
            kmemset(&udp_endpoints[i], 0, sizeof(udp_endpoint_t));
            udp_endpoints[i].af = af;
            udp_endpoints[i].port = port;
            if (addr) kmemcpy(udp_endpoints[i].addr, addr, (af == AF_INET) ? 4 : 16);
            udp_endpoints[i].recv_timeout = recv_timeout;
            udp_endpoints[i].send_timeout = send_timeout;
            udp_endpoints[i].ipv6only = ipv6only;
            udp_endpoints[i].used = 1;
            spinlock_release(&udp_lock, flags);
            return ERR_OK;
        }
    }
    spinlock_release(&udp_lock, flags);
    return ERR_NOSPACE;
}

udp_endpoint_t* udp_find_endpoint(int af, uint16_t port) {
    cpu_flags_t flags;
    spinlock_acquire(&udp_lock, &flags);
    for (int i = 0; i < UDP_MAX_ENDPOINTS; i++) {
        if (udp_endpoints[i].used &&
            udp_endpoints[i].af == af &&
            udp_endpoints[i].port == port) {
            spinlock_release(&udp_lock, flags);
            return &udp_endpoints[i];
        }
    }
    spinlock_release(&udp_lock, flags);
    return NULL;
}

void udp_unbind_endpoint(udp_endpoint_t* ep) {
    if (!ep) return;
    cpu_flags_t flags;
    spinlock_acquire(&udp_lock, &flags);
    for (int i = 0; i < UDP_MAX_ENDPOINTS; i++) {
        if (&udp_endpoints[i] == ep) {
            udp_endpoints[i].used = 0;
            spinlock_release(&udp_lock, flags);
            return;
        }
    }
    spinlock_release(&udp_lock, flags);
}

void udp_endpoint_enqueue(udp_endpoint_t* ep, int af, const void* src_ip,
                           uint16_t src_port,
                           const uint8_t* data, uint32_t len) {
    if (ep->q_count >= UDP_DGRAM_QUEUE_SIZE) return;

    udp_dgram_t* d = &ep->queue[ep->q_tail];
    d->af = af;
    kmemcpy(d->src_addr, src_ip, (af == AF_INET) ? 4 : 16);
    d->src_port = src_port;
    d->len = len < UDP_DGRAM_MAX_SIZE ? len : UDP_DGRAM_MAX_SIZE;
    kmemcpy(d->data, data, d->len);

    ep->q_tail = (ep->q_tail + 1) % UDP_DGRAM_QUEUE_SIZE;
    ep->q_count++;
}

int udp_endpoint_dequeue(udp_endpoint_t* ep, uint8_t* buf, uint32_t size,
                          int* out_af, void* out_src_addr,
                          uint16_t* out_src_port, int timeout_ms) {
    int step = 50;
    int steps = timeout_ms / step;
    for (int i = 0; i < steps; i++) {
        eth_rx_poll();
        cpu_flags_t flags;
        spinlock_acquire(&udp_lock, &flags);
        if (ep->q_count > 0) {
            udp_dgram_t* d = &ep->queue[ep->q_head];
            uint32_t copy_len = size < d->len ? size : d->len;
            kmemcpy(buf, d->data, copy_len);

            if (out_af) *out_af = d->af;
            if (out_src_addr) {
                kmemcpy(out_src_addr, d->src_addr, (d->af == AF_INET) ? 4 : 16);
            }
            if (out_src_port) *out_src_port = d->src_port;

            ep->q_head = (ep->q_head + 1) % UDP_DGRAM_QUEUE_SIZE;
            ep->q_count--;
            spinlock_release(&udp_lock, flags);
            return (int)copy_len;
        }
        spinlock_release(&udp_lock, flags);
        thread_sleep((uint64_t)step);
    }
    return ERR_TIMEOUT;
}

/* ---- Packet handling ---- */

static void udp_handle_common(int af, const void* src_ip, const void* dst_ip,
                               const uint8_t* data, uint32_t len) {
    if (len < UDP_HDR_LEN) return;
    const udp_hdr_t* hdr = (const udp_hdr_t*)data;

    /* Validate checksum:
     * Standard validation: compute checksum over pseudo-header + full UDP segment
     * (including the checksum field as-is). Result should be 0 for a valid packet. */
    uint16_t wire_csum = hdr->checksum;
    if (af == AF_INET) {
        /* IPv4: checksum is optional (0 means no checksum) */
        if (wire_csum != 0) {
            /* Compute checksum over full data including wire checksum field.
             * Valid packet produces calc=0 (1's complement of 0xFFFF). */
            uint16_t calc = udp_checksum_v4(data, len,
                                             *(const ipv4_addr_t*)src_ip,
                                             *(const ipv4_addr_t*)dst_ip);
            if (calc != 0) {
                KDEBUG("[UDP] IPv4 checksum mismatch: wire=0x%04x calc=0x%04x\n",
                       wire_csum, calc);
                return;
            }
        }
    } else {
        /* IPv6: checksum is mandatory */
        if (wire_csum == 0) {
            KDEBUG("[UDP] IPv6 zero checksum, dropping\n");
            return;
        }
        uint16_t calc = udp_checksum_v6(data, len,
                                         (const uint8_t*)src_ip,
                                         (const uint8_t*)dst_ip);
        if (calc != 0) {
            KDEBUG("[UDP] IPv6 checksum mismatch: wire=0x%04x calc=0x%04x\n",
                   wire_csum, calc);
            return;
        }
    }

    uint16_t dst_port = udp_htons(hdr->dst_port);
    uint16_t src_port = udp_htons(hdr->src_port);
    uint32_t payload_len = len - UDP_HDR_LEN;
    const uint8_t* payload = data + UDP_HDR_LEN;

    for (int i = 0; i < UDP_MAX_ENDPOINTS; i++) {
        if (udp_endpoints[i].used &&
            udp_endpoints[i].af == af &&
            udp_endpoints[i].port == dst_port) {
            udp_endpoint_enqueue(&udp_endpoints[i], af, src_ip,
                                  src_port, payload, payload_len);
            return;
        }
    }
}

static void udp_ipv4_handler(ipv4_addr_t src, ipv4_addr_t dst,
                               uint8_t protocol,
                               const uint8_t* data, uint32_t len) {
    (void)protocol;
    cpu_flags_t flags;
    spinlock_acquire(&udp_lock, &flags);
    udp_handle_common(AF_INET, &src, &dst, data, len);
    spinlock_release(&udp_lock, flags);
}

static void udp_ipv6_handler(const uint8_t* src, const uint8_t* dst,
                               uint8_t next_header,
                               const uint8_t* data, uint32_t len) {
    (void)next_header;
    cpu_flags_t flags;
    spinlock_acquire(&udp_lock, &flags);
    udp_handle_common(AF_INET6, src, dst, data, len);
    spinlock_release(&udp_lock, flags);
}

void udp_init(void) {
    if (udp_initialized) return;

    spinlock_init(&udp_lock, "udp");
    kmemset(udp_endpoints, 0, sizeof(udp_endpoints));

    ipv4_register_handler(17, udp_ipv4_handler);
    ipv6_register_handler(17, udp_ipv6_handler);
    kprintf("[UDP] Initialized\n");

    udp_initialized = 1;
}

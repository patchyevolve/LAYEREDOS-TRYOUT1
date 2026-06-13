#include "kernel.h"
#include "icmpv6.h"
#include "ipv6.h"
#include "ndp.h"
#include "eth.h"
#include "nic.h"
#include "hal.h"
#include "sched.h"

#define ICMPV6_PING_DATA "opencode6"
#define ICMPV6_PING_DATALEN 9

static int icmpv6_initialized = 0;
static uint16_t icmpv6_next_id = 1;

static void (*icmpv6_ra_callback)(const uint8_t* src, const uint8_t* data, uint32_t len) = NULL;

void icmpv6_set_ra_callback(void (*cb)(const uint8_t* src, const uint8_t* data, uint32_t len)) {
    icmpv6_ra_callback = cb;
}

static struct {
    uint8_t  dst[16];
    uint16_t id;
    uint16_t seq;
    int replied;
} icmpv6_ping_wait;

/* Compute ICMPv6 checksum (includes IPv6 pseudo-header) */
static uint16_t icmpv6_checksum(const uint8_t* src_ip, const uint8_t* dst_ip,
                                 const void* icmp, uint32_t icmp_len) {
    uint32_t sum = 0;
    const uint16_t* p;

    /* Pseudo-header: source address (8 × uint16) */
    p = (const uint16_t*)src_ip;
    for (int i = 0; i < 8; i++) sum += __builtin_bswap16(p[i]);

    /* Pseudo-header: destination address */
    p = (const uint16_t*)dst_ip;
    for (int i = 0; i < 8; i++) sum += __builtin_bswap16(p[i]);

    /* Pseudo-header: payload length */
    sum += (uint16_t)icmp_len;

    /* Pseudo-header: next header */
    sum += (uint16_t)58;

    /* ICMP message itself */
    p = (const uint16_t*)icmp;
    uint32_t words = icmp_len / 2;
    for (uint32_t i = 0; i < words; i++) sum += __builtin_bswap16(p[i]);

    /* Pad byte if odd length */
    if (icmp_len & 1)
        sum += (uint16_t)((const uint8_t*)icmp)[icmp_len - 1] << 8;

    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return __builtin_bswap16(~sum & 0xFFFF);
}

static void icmpv6_handler(const uint8_t* src, const uint8_t* dst,
                             uint8_t next_header,
                             const uint8_t* data, uint32_t len) {
    (void)next_header;
    (void)dst;
    if (!icmpv6_initialized) return;
    if (len < sizeof(icmpv6_hdr_t)) return;

    const icmpv6_hdr_t* hdr = (const icmpv6_hdr_t*)data;

    switch (hdr->type) {
    case ICMPV6_ECHO_REQ: {
        uint16_t id = __builtin_bswap16(*(const uint16_t*)&hdr->data[0]);
        uint16_t seq = __builtin_bswap16(*(const uint16_t*)&hdr->data[2]);
        kprintf("[ICMPv6] Echo request id=%u seq=%u\n", id, seq);

        uint8_t reply_buf[len];
        icmpv6_hdr_t* rep = (icmpv6_hdr_t*)reply_buf;
        rep->type = ICMPV6_ECHO_REPLY;
        rep->code = 0;
        rep->checksum = 0;
        kmemcpy(rep->data, hdr->data, len - sizeof(icmpv6_hdr_t));

        uint8_t my_ip[16];
        ndp_make_lladdr(nic.mac, my_ip);
        rep->checksum = icmpv6_checksum(my_ip, src, rep, len);
        ipv6_send(src, 58, reply_buf, len);
        break;
    }

    case ICMPV6_ECHO_REPLY: {
        uint16_t id = __builtin_bswap16(*(const uint16_t*)&hdr->data[0]);

        if (icmpv6_ping_wait.replied) break;
        if (kmemcmp(src, icmpv6_ping_wait.dst, 16) != 0) break;
        if (id != icmpv6_ping_wait.id) break;

        icmpv6_ping_wait.replied = 1;
        break;
    }

    case ICMPV6_NS: {
        if (len < sizeof(icmpv6_hdr_t) + 16) break;
        const uint8_t* target = hdr->data + 4;
        uint8_t my_ip[16];
        ndp_make_lladdr(nic.mac, my_ip);
        if (kmemcmp(target, my_ip, 16) != 0) {
            kprintf("[ICMPv6] NS for other addr, ignoring\n");
            break;
        }
        kprintf("[ICMPv6] NS from %x:%x:%x:%x:%x:%x:%x:%x for our link-local, sending NA\n",
                (uint32_t)((src[0]<<8)|src[1]), (uint32_t)((src[2]<<8)|src[3]),
                (uint32_t)((src[4]<<8)|src[5]), (uint32_t)((src[6]<<8)|src[7]),
                (uint32_t)((src[8]<<8)|src[9]), (uint32_t)((src[10]<<8)|src[11]),
                (uint32_t)((src[12]<<8)|src[13]), (uint32_t)((src[14]<<8)|src[15]));

        /* NA: ICMPv6 hdr (4) + flags+reserved (4) + target (16) + TLLAO (8) */
        uint8_t na_buf[4 + 4 + 16 + 8];
        kmemset(na_buf, 0, sizeof(na_buf));
        na_buf[0] = ICMPV6_NA;         /* type=136 */
        na_buf[1] = 0;                 /* code=0 */
        /* flags at offset 4: R=0, S=1, O=1 */
        na_buf[4] = 0x60;              /* Solicited + Override */
        kmemcpy(na_buf + 8, target, 16); /* target address from NS */
        na_buf[24] = NDP_OPT_TGT_LLADDR; /* type=2 */
        na_buf[25] = 1;                  /* len=1 (8 octets) */
        kmemcpy(na_buf + 26, nic.mac, 6); /* MAC */

        uint32_t na_len = sizeof(na_buf);
        uint16_t csum = icmpv6_checksum(my_ip, src, na_buf, na_len);
        *(uint16_t*)&na_buf[2] = csum;

        int ret = ipv6_send(src, 58, na_buf, na_len);
        kprintf("[ICMPv6] NA sent ret=%d\n", ret);
        break;
    }

    case ICMPV6_NA: {
        kprintf("[ICMPv6] NA received from %x:%x:%x:%x:%x:%x:%x:%x\n",
                (uint32_t)((src[0]<<8)|src[1]), (uint32_t)((src[2]<<8)|src[3]),
                (uint32_t)((src[4]<<8)|src[5]), (uint32_t)((src[6]<<8)|src[7]),
                (uint32_t)((src[8]<<8)|src[9]), (uint32_t)((src[10]<<8)|src[11]),
                (uint32_t)((src[12]<<8)|src[13]), (uint32_t)((src[14]<<8)|src[15]));
        if (len < sizeof(icmpv6_hdr_t) + 4 + 16) break;
        const uint8_t* target = data + sizeof(icmpv6_hdr_t) + 4;
        /* Extract TLLAO if present */
        const uint8_t* tllao = NULL;
        uint32_t off = sizeof(icmpv6_hdr_t) + 4 + 16;
        while (off + 2 <= len) {
            uint8_t opt_type = data[off];
            uint8_t opt_len8 = data[off + 1];
            uint32_t opt_len = (uint32_t)opt_len8 * 8;
            if (opt_len == 0) break;
            if (opt_type == NDP_OPT_TGT_LLADDR && opt_len >= 8) {
                tllao = data + off + 2;
                break;
            }
            off += opt_len;
        }
        /* Cache the target's MAC from TLLAO */
        if (tllao) {
            ndp_cache_update(target, tllao);
        }
        break;
    }

    case ICMPV6_RA:
        kprintf("[ICMPv6] RA received from %x:%x:%x:%x:%x:%x:%x:%x\n",
                (uint32_t)((src[0]<<8)|src[1]), (uint32_t)((src[2]<<8)|src[3]),
                (uint32_t)((src[4]<<8)|src[5]), (uint32_t)((src[6]<<8)|src[7]),
                (uint32_t)((src[8]<<8)|src[9]), (uint32_t)((src[10]<<8)|src[11]),
                (uint32_t)((src[12]<<8)|src[13]), (uint32_t)((src[14]<<8)|src[15]));
        if (icmpv6_ra_callback) {
            icmpv6_ra_callback(src, data, len);
        }
        break;

    case ICMPV6_RS:
        kprintf("[ICMPv6] RS received from %x:%x:%x:%x:%x:%x:%x:%x\n",
                (uint32_t)((src[0]<<8)|src[1]), (uint32_t)((src[2]<<8)|src[3]),
                (uint32_t)((src[4]<<8)|src[5]), (uint32_t)((src[6]<<8)|src[7]),
                (uint32_t)((src[8]<<8)|src[9]), (uint32_t)((src[10]<<8)|src[11]),
                (uint32_t)((src[12]<<8)|src[13]), (uint32_t)((src[14]<<8)|src[15]));
        break;

    case ICMPV6_MLD_QUERY: {
        if (len < 24) break;
        const uint8_t* mcast_addr = hdr->data + 4;
        int is_general = 1;
        for (int i = 0; i < 16; i++) {
            if (mcast_addr[i] != 0) { is_general = 0; break; }
        }
        if (is_general) {
            ipv6_mcast_report_all();
        } else {
            mldv1_send_report(mcast_addr);
        }
        break;
    }

    default:
        kprintf("[ICMPv6] Unhandled type %u\n", hdr->type);
        break;
    }
}

int icmpv6_ping(const uint8_t* dst, int timeout_ms) {
    if (!icmpv6_initialized) return ERR_NOSYS;

    uint8_t buf[sizeof(icmpv6_hdr_t) + 4 + ICMPV6_PING_DATALEN];
    icmpv6_hdr_t* hdr = (icmpv6_hdr_t*)buf;

    kmemcpy(icmpv6_ping_wait.dst, dst, 16);
    icmpv6_ping_wait.id = icmpv6_next_id++;
    icmpv6_ping_wait.seq++;
    icmpv6_ping_wait.replied = 0;

    hdr->type = ICMPV6_ECHO_REQ;
    hdr->code = 0;
    hdr->checksum = 0;
    *(uint16_t*)hdr->data = __builtin_bswap16(icmpv6_ping_wait.id);
    *(uint16_t*)(hdr->data + 2) = __builtin_bswap16(icmpv6_ping_wait.seq);
    kmemcpy(hdr->data + 4, ICMPV6_PING_DATA, ICMPV6_PING_DATALEN);

    uint32_t icmp_len = sizeof(icmpv6_hdr_t) + 4 + ICMPV6_PING_DATALEN;
    uint8_t my_ip[16];
    ndp_make_lladdr(nic.mac, my_ip);
    hdr->checksum = icmpv6_checksum(my_ip, dst, hdr, icmp_len);
    ipv6_send(dst, 58, buf, icmp_len);

    int step = 50;
    int steps = timeout_ms / step;
    for (int i = 0; i < steps && !icmpv6_ping_wait.replied; i++) {
        eth_rx_poll();
        thread_sleep((uint64_t)step);
    }

    return icmpv6_ping_wait.replied ? ERR_OK : ERR_TIMEOUT;
}

int icmpv6_send_ns(const uint8_t* target_ip) {
    if (!icmpv6_initialized) return ERR_NOSYS;

    /* NS: ICMPv6 hdr (4) + reserved (4) + target (16) + SLLAO (8) = 32 */
    uint8_t buf[4 + 4 + 16 + 8];
    kmemset(buf, 0, sizeof(buf));
    buf[0] = ICMPV6_NS;    /* type=135 */
    buf[1] = 0;            /* code=0 */
    kmemcpy(buf + 8, target_ip, 16); /* target address */
    buf[24] = NDP_OPT_SRC_LLADDR;    /* type=1 */
    buf[25] = 1;                     /* len=1 (8 octets) */
    kmemcpy(buf + 26, nic.mac, 6);

    /* Solicited-node multicast: ff02::1:ffXX:XXXX */
    uint8_t mc[16];
    ndp_make_solicited_node(target_ip, mc);

    uint8_t my_ip[16];
    ndp_make_lladdr(nic.mac, my_ip);

    uint32_t ns_len = sizeof(buf);
    uint16_t csum = icmpv6_checksum(my_ip, mc, buf, ns_len);
    *(uint16_t*)&buf[2] = csum;

    int e = ipv6_send(mc, 58, buf, ns_len);
    kprintf("[ICMPv6] NS sent for target %x:%x:%x:%x:%x:%x:%x:%x ret=%d\n",
            (uint32_t)((target_ip[0]<<8)|target_ip[1]),
            (uint32_t)((target_ip[2]<<8)|target_ip[3]),
            (uint32_t)((target_ip[4]<<8)|target_ip[5]),
            (uint32_t)((target_ip[6]<<8)|target_ip[7]),
            (uint32_t)((target_ip[8]<<8)|target_ip[9]),
            (uint32_t)((target_ip[10]<<8)|target_ip[11]),
            (uint32_t)((target_ip[12]<<8)|target_ip[13]),
            (uint32_t)((target_ip[14]<<8)|target_ip[15]), e);
    return e;
}

void icmpv6_send_rs(void) {
    if (!icmpv6_initialized) return;

    uint8_t buf[8 + 8]; /* ICMPv6 RS header (8) + SLLAO option (8) */
    kmemset(buf, 0, sizeof(buf));
    buf[0] = ICMPV6_RS; /* type=133 */
    buf[1] = 0;         /* code=0 */

    /* Source Link-Layer Address option */
    buf[8] = NDP_OPT_SRC_LLADDR; /* type=1 */
    buf[9] = 1;                  /* len=1 (8 octets) */
    kmemcpy(buf + 10, nic.mac, 6);

    /* Destination: ff02::2 (all-routers) */
    uint8_t all_routers[16];
    kmemset(all_routers, 0, 16);
    all_routers[0] = 0xFF;
    all_routers[1] = 0x02;
    all_routers[15] = 0x02;

    uint8_t my_ip[16];
    ndp_make_lladdr(nic.mac, my_ip);

    uint32_t icmp_len = sizeof(buf);
    uint16_t csum = icmpv6_checksum(my_ip, all_routers, (void*)buf, icmp_len);
    /* csum is already byte-swapped from icmpv6_checksum */
    *(uint16_t*)&buf[2] = csum;

    int e = ipv6_send(all_routers, 58, buf, icmp_len);
    kprintf("[ICMPv6] RS sent: %d\n", e);
}

/* ── MLDv1 ─────────────────────────────────────────────────────────────── */
void mldv1_send_report(const uint8_t* group_addr) {
    if (!icmpv6_initialized) return;
    uint8_t buf[4 + 20]; /* hdr(4) + maxresp(2) + reserved(2) + addr(16) */
    kmemset(buf, 0, sizeof(buf));
    buf[0] = ICMPV6_MLD_REPORT;
    buf[1] = 0;
    kmemcpy(buf + 8, group_addr, 16); /* addr at offset 8 */

    uint8_t my_ip[16];
    ndp_make_lladdr(nic.mac, my_ip);
    uint16_t csum = icmpv6_checksum(my_ip, group_addr, buf, sizeof(buf));
    *(uint16_t*)&buf[2] = csum;

    ipv6_send(group_addr, 58, buf, sizeof(buf));
    KDEBUG("[MLDv1] Report sent\n");
}

void mldv1_send_done(const uint8_t* group_addr) {
    if (!icmpv6_initialized) return;
    uint8_t buf[4 + 20];
    kmemset(buf, 0, sizeof(buf));
    buf[0] = ICMPV6_MLD_DONE;
    buf[1] = 0;
    kmemcpy(buf + 8, group_addr, 16);

    uint8_t all_routers[16];
    kmemset(all_routers, 0, 16);
    all_routers[0] = 0xFF;
    all_routers[1] = 0x02;
    all_routers[15] = 0x02;

    uint8_t my_ip[16];
    ndp_make_lladdr(nic.mac, my_ip);
    uint16_t csum = icmpv6_checksum(my_ip, all_routers, buf, sizeof(buf));
    *(uint16_t*)&buf[2] = csum;

    ipv6_send(all_routers, 58, buf, sizeof(buf));
    KDEBUG("[MLDv1] Done sent\n");
}

void icmpv6_init(void) {
    if (icmpv6_initialized) return;

    ipv6_register_handler(58, icmpv6_handler);
    kprintf("[ICMPv6] Initialized\n");

    icmpv6_initialized = 1;
}

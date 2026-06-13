#include "kernel.h"
#include "icmp.h"
#include "ipv4.h"
#include "eth.h"
#include "hal.h"
#include "sched.h"

#define ICMP_PING_DATA "opencode ping"
#define ICMP_PING_DATALEN 13

static int icmp_initialized = 0;
static uint16_t icmp_next_id = 1;

static struct {
    ipv4_addr_t dst;
    uint16_t id;
    uint16_t seq;
    int replied;
} icmp_ping_wait;

static uint16_t icmp_checksum(const void* data, uint32_t len) {
    uint32_t sum = 0;
    const uint16_t* p = (const uint16_t*)data;
    for (uint32_t i = 0; i < len / 2; i++) {
        sum += __builtin_bswap16(p[i]);
    }
    if (len & 1) {
        sum += (uint16_t)((const uint8_t*)data)[len - 1] << 8;
    }
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return ~sum & 0xFFFF;
}

static void icmpv4_handler(ipv4_addr_t src, ipv4_addr_t dst,
                             uint8_t protocol,
                             const uint8_t* data, uint32_t len) {
    (void)protocol;
    (void)dst;
    if (!icmp_initialized) return;
    if (len < ICMPV4_HDR_LEN) return;

    const icmpv4_hdr_t* hdr = (const icmpv4_hdr_t*)data;

    if (hdr->type == ICMP_ECHO_REQUEST) {
        uint16_t id = __builtin_bswap16(hdr->id);
        uint16_t seq = __builtin_bswap16(hdr->sequence);

        kprintf("[ICMP] Echo request from %d.%d.%d.%d id=%u seq=%u\n",
                src.bytes[0], src.bytes[1], src.bytes[2], src.bytes[3],
                id, seq);

        uint8_t reply_buf[ICMPV4_HDR_LEN + len - ICMPV4_HDR_LEN];
        icmpv4_hdr_t* rep = (icmpv4_hdr_t*)reply_buf;
        rep->type = ICMP_ECHO_REPLY;
        rep->code = 0;
        rep->checksum = 0;
        rep->id = hdr->id;
        rep->sequence = hdr->sequence;
        kmemcpy(rep->data, hdr->data, len - ICMPV4_HDR_LEN);
        rep->checksum = __builtin_bswap16(
            icmp_checksum(rep, len));
        ipv4_send(src, 1, reply_buf, len);
    }

    if (hdr->type == ICMP_ECHO_REPLY) {
        uint16_t id = __builtin_bswap16(hdr->id);
        uint16_t seq = __builtin_bswap16(hdr->sequence);

        kprintf("[ICMP] Echo reply from %d.%d.%d.%d id=%u seq=%u\n",
                src.bytes[0], src.bytes[1], src.bytes[2], src.bytes[3],
                id, seq);

        if (icmp_ping_wait.replied) return;

        if (ipv4_addr_equal(src, icmp_ping_wait.dst) &&
            id == icmp_ping_wait.id &&
            seq == icmp_ping_wait.seq) {
            icmp_ping_wait.replied = 1;
        }
    }
}

int icmpv4_ping(ipv4_addr_t dst, int timeout_ms) {
    if (!icmp_initialized) return ERR_NOSYS;

    uint8_t buf[ICMPV4_HDR_LEN + ICMP_PING_DATALEN];
    icmpv4_hdr_t* hdr = (icmpv4_hdr_t*)buf;

    icmp_ping_wait.dst = dst;
    icmp_ping_wait.id = icmp_next_id++;
    icmp_ping_wait.seq++;
    icmp_ping_wait.replied = 0;

    hdr->type = ICMP_ECHO_REQUEST;
    hdr->code = 0;
    hdr->checksum = 0;
    hdr->id = __builtin_bswap16(icmp_ping_wait.id);
    hdr->sequence = __builtin_bswap16(icmp_ping_wait.seq);
    kmemcpy(hdr->data, ICMP_PING_DATA, ICMP_PING_DATALEN);
    hdr->checksum = __builtin_bswap16(
        icmp_checksum(buf, sizeof(buf)));

    ipv4_send(dst, 1, buf, sizeof(buf));

    int step = 50;
    int steps = timeout_ms / step;
    for (int i = 0; i < steps && !icmp_ping_wait.replied; i++) {
        eth_rx_poll();
        thread_sleep((uint64_t)step);
    }

    return icmp_ping_wait.replied ? ERR_OK : ERR_TIMEOUT;
}

void icmpv4_init(void) {
    if (icmp_initialized) return;

    ipv4_register_handler(1, icmpv4_handler);
    kprintf("[ICMP] Initialized\n");

    icmp_initialized = 1;
}

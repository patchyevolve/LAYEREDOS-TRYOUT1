#include "kernel.h"
#include "slaac.h"
#include "icmpv6.h"
#include "ipv6.h"
#include "ndp.h"
#include "route.h"
#include "nic.h"
#include "eth.h"
#include "sched.h"
#include "hpet.h"

#define SLAAC_TIMEOUT_MS 2000

static struct {
    int received;
    uint8_t src[16];
    uint8_t data[512];
    uint32_t len;
} slaac_ra;

static void slaac_handle_ra(const uint8_t* src, const uint8_t* data, uint32_t len) {
    if (len > 512) len = 512;
    kmemcpy(slaac_ra.src, src, 16);
    kmemcpy(slaac_ra.data, data, len);
    slaac_ra.len = len;
    slaac_ra.received = 1;
}

static void slaac_make_global_addr(const uint8_t* prefix, int prefix_len,
                                   const uint8_t* mac, uint8_t* addr_out) {
    kmemcpy(addr_out, prefix, 16);
    /* Replace lower 64 bits with EUI-64 from MAC */
    addr_out[8] = mac[0] ^ 0x02;
    addr_out[9] = mac[1];
    addr_out[10] = mac[2];
    addr_out[11] = 0xFF;
    addr_out[12] = 0xFE;
    addr_out[13] = mac[3];
    addr_out[14] = mac[4];
    addr_out[15] = mac[5];

    kprintf("[SLAAC] Formed global addr from /%d prefix: "
            "%x:%x:%x:%x:%x:%x:%x:%x\n",
            prefix_len,
            (uint32_t)((addr_out[0]<<8)|addr_out[1]),
            (uint32_t)((addr_out[2]<<8)|addr_out[3]),
            (uint32_t)((addr_out[4]<<8)|addr_out[5]),
            (uint32_t)((addr_out[6]<<8)|addr_out[7]),
            (uint32_t)((addr_out[8]<<8)|addr_out[9]),
            (uint32_t)((addr_out[10]<<8)|addr_out[11]),
            (uint32_t)((addr_out[12]<<8)|addr_out[13]),
            (uint32_t)((addr_out[14]<<8)|addr_out[15]));
}

err_t slaac_init(void) {
    icmpv6_set_ra_callback(slaac_handle_ra);
    kprintf("[SLAAC] Initialized\n");
    return ERR_OK;
}

err_t slaac_configure(void) {
    if (!nic.present) {
        kprintf("[SLAAC] NIC not present\n");
        return ERR_IO;
    }

    kprintf("[SLAAC] Sending Router Solicitation...\n");
    slaac_ra.received = 0;
    icmpv6_send_rs();

    int step = 100;
    int steps = SLAAC_TIMEOUT_MS / step;
    uint64_t hpet_deadline = hpet_present ? hpet_ns() + (uint64_t)SLAAC_TIMEOUT_MS * 1000000ULL : 0;
    for (int i = 0; i < steps; i++) {
        eth_rx_poll();
        if (slaac_ra.received) {
            kprintf("[SLAAC] RA received within %d ms\n", i * step);
            break;
        }
        if (hpet_present && hpet_ns() >= hpet_deadline) break;
        thread_yield();
    }

    if (!slaac_ra.received) {
        kprintf("[SLAAC] No RA received within timeout\n");
        return ERR_TIMEOUT;
    }

    /* Parse RA for prefix options */
    const uint8_t* ra = slaac_ra.data;
    uint32_t ra_len = slaac_ra.len;

    if (ra_len < 16) {
        kprintf("[SLAAC] RA too short\n");
        return ERR_INVAL;
    }

    uint8_t flags = ra[5];
    int managed = !!(flags & 0x80);
    int other = !!(flags & 0x40);

    kprintf("[SLAAC] RA: hop_limit=%u flags=M:%d,O:%d lifetime=%u\n",
            ra[4], managed, other,
            (uint32_t)ra[6] << 8 | ra[7]);

    /* Parse options starting after the fixed RA header (16 bytes) */
    uint32_t off = 16;
    int found_prefix = 0;

    while (off + 2 <= ra_len) {
        uint8_t opt_type = ra[off];
        uint8_t opt_len8 = ra[off + 1];
        if (opt_len8 == 0) break;
        uint32_t opt_len = (uint32_t)opt_len8 * 8;
        if (off + opt_len > ra_len) break;

        if (opt_type == NDP_OPT_PREFIX && opt_len >= 32) {
            const uint8_t* prefix_opt = ra + off;
            uint8_t prefix_len = prefix_opt[2];
            uint8_t pf_flags = prefix_opt[3];
            int on_link = !!(pf_flags & 0x80);
            int autonomous = !!(pf_flags & 0x40);

            kprintf("[SLAAC] Prefix option: len=%d L=%d A=%d valid=%u pref=%u\n",
                    prefix_len, on_link, autonomous,
                    (uint32_t)prefix_opt[4]<<24 | (uint32_t)prefix_opt[5]<<16 |
                    (uint32_t)prefix_opt[6]<<8  | (uint32_t)prefix_opt[7],
                    (uint32_t)prefix_opt[8]<<24 | (uint32_t)prefix_opt[9]<<16 |
                    (uint32_t)prefix_opt[10]<<8 | (uint32_t)prefix_opt[11]);

            if (autonomous && prefix_len == 64) {
                uint8_t global_addr[16];
                slaac_make_global_addr(prefix_opt + 16, prefix_len,
                                       nic.mac, global_addr);
                ipv6_set_addr(global_addr);

                /* Add route: prefix/64 via RA source (the router) */
                route_add_v6(prefix_opt + 16, prefix_len, slaac_ra.src);
                kprintf("[SLAAC] Added route for /%d prefix via router\n", prefix_len);

                /* Add default route ::/0 via RA source */
                uint8_t all_zero[16];
                kmemset(all_zero, 0, 16);
                route_add_v6(all_zero, 0, slaac_ra.src);
                kprintf("[SLAAC] Added default route via RA source\n");

                found_prefix = 1;
            }
        }

        off += opt_len;
    }

    if (!found_prefix) {
        kprintf("[SLAAC] No suitable prefix option found\n");
        return ERR_NOENT;
    }

    kprintf("[SLAAC] Configuration complete\n");
    return ERR_OK;
}

#include "kernel.h"
#include "ipv6.h"
#include "eth.h"
#include "ndp.h"
#include "route.h"
#include "nic.h"
#include "e1000.h"
#include "icmpv6.h"

#define IPV6_HOP_LIMIT 64
#define IPV6_DISPATCH_SLOTS 8

static int ipv6_initialized = 0;
static struct {
    uint8_t         next_header;
    ipv6_handler_t  handler;
} ipv6_dispatch[IPV6_DISPATCH_SLOTS];

static uint8_t GLOBAL_IPV6[16];
static uint8_t LINK_LOCAL[16];

/* Multicast group table */
static struct {
    uint8_t addr[16];
    int     used;
} ipv6_mcast_groups[IPV6_MAX_MCAST_GROUPS];

int ipv6_register_handler(uint8_t next_header, ipv6_handler_t handler) {
    for (int i = 0; i < IPV6_DISPATCH_SLOTS; i++) {
        if (!ipv6_dispatch[i].handler) {
            ipv6_dispatch[i].next_header = next_header;
            ipv6_dispatch[i].handler = handler;
            return ERR_OK;
        }
    }
    return ERR_NOSPACE;
}

static void ipv6_eth_handler(const uint8_t* src_mac, uint16_t type,
                              const uint8_t* data, uint32_t len) {
    (void)type;
    if (!ipv6_initialized) return;
    if (len < IPV6_HDR_LEN) return;

    const ipv6_hdr_t* hdr = (const ipv6_hdr_t*)data;
    uint32_t ver_tc = __builtin_bswap32(hdr->ver_tc_flow);
    if ((ver_tc >> 28) != 6) return;

    /* Check destination — must be us, multicast, or joined multicast group */
    int dst_is_ll = (kmemcmp(hdr->dst, LINK_LOCAL, 16) == 0);
    int dst_is_global = (ipv6_has_global_addr() && kmemcmp(hdr->dst, GLOBAL_IPV6, 16) == 0);
    int dst_is_mcast = (hdr->dst[0] == 0xFF);
    if (!dst_is_ll && !dst_is_global && !dst_is_mcast &&
        !ipv6_mcast_is_member(hdr->dst)) return;

    /* Learn source IPv6 → MAC mapping from valid received IPv6 packet */
    ndp_cache_update(hdr->src, src_mac);

    uint8_t next = hdr->next_header;

    KDEBUG("[IP6 RX] nh=%u src=%x:%x:%x:%x:%x:%x:%x:%x dst=%x:%x:%x:%x:%x:%x:%x:%x\n",
            next,
            (uint32_t)((hdr->src[0]<<8)|hdr->src[1]),
            (uint32_t)((hdr->src[2]<<8)|hdr->src[3]),
            (uint32_t)((hdr->src[4]<<8)|hdr->src[5]),
            (uint32_t)((hdr->src[6]<<8)|hdr->src[7]),
            (uint32_t)((hdr->src[8]<<8)|hdr->src[9]),
            (uint32_t)((hdr->src[10]<<8)|hdr->src[11]),
            (uint32_t)((hdr->src[12]<<8)|hdr->src[13]),
            (uint32_t)((hdr->src[14]<<8)|hdr->src[15]),
            (uint32_t)((hdr->dst[0]<<8)|hdr->dst[1]),
            (uint32_t)((hdr->dst[2]<<8)|hdr->dst[3]),
            (uint32_t)((hdr->dst[4]<<8)|hdr->dst[5]),
            (uint32_t)((hdr->dst[6]<<8)|hdr->dst[7]),
            (uint32_t)((hdr->dst[8]<<8)|hdr->dst[9]),
            (uint32_t)((hdr->dst[10]<<8)|hdr->dst[11]),
            (uint32_t)((hdr->dst[12]<<8)|hdr->dst[13]),
            (uint32_t)((hdr->dst[14]<<8)|hdr->dst[15]));

    uint32_t payload_len = __builtin_bswap16(hdr->payload_len);
    const uint8_t* payload = data + IPV6_HDR_LEN;

    if (payload_len > len - IPV6_HDR_LEN) {
        payload_len = len - IPV6_HDR_LEN;
    }

    for (int i = 0; i < IPV6_DISPATCH_SLOTS; i++) {
        if (ipv6_dispatch[i].handler &&
            ipv6_dispatch[i].next_header == next) {
            ipv6_dispatch[i].handler(hdr->src, hdr->dst, next, payload, payload_len);
        }
    }
}

int ipv6_send(const uint8_t* dst, uint8_t next_header,
              const uint8_t* data, uint32_t len) {
    if (!ipv6_initialized) return ERR_NOSYS;

    uint8_t buf[IPV6_HDR_LEN + len];
    ipv6_hdr_t* hdr = (ipv6_hdr_t*)buf;

    hdr->ver_tc_flow = __builtin_bswap32(0x60000000);
    hdr->payload_len = __builtin_bswap16(len);
    hdr->next_header = next_header;
    hdr->hop_limit = IPV6_HOP_LIMIT;

    if (ipv6_has_global_addr() && (dst[0] != 0xFE || dst[1] != 0x80)) {
        kmemcpy(hdr->src, GLOBAL_IPV6, 16);
    } else {
        kmemcpy(hdr->src, LINK_LOCAL, 16);
    }
    kmemcpy(hdr->dst, dst, 16);
    kmemcpy(buf + IPV6_HDR_LEN, data, len);

    uint8_t next_mac[6];
    int e;
    if (dst[0] == 0xFF) {
        /* IPv6 multicast → Ethernet multicast MAC 33:33:XX:XX:XX:XX */
        next_mac[0] = 0x33; next_mac[1] = 0x33;
        next_mac[2] = dst[12]; next_mac[3] = dst[13];
        next_mac[4] = dst[14]; next_mac[5] = dst[15];
        e = ERR_OK;
    } else {
        e = ndp_resolve(dst, next_mac, 30000);
    }
    if (e != ERR_OK) {
        kprintf("[IPv6] NDP resolve failed\n");
        return e;
    }

    e = eth_send(next_mac, ETHERTYPE_IPV6, buf, IPV6_HDR_LEN + len);
    if (e != ERR_OK) {
        kprintf("[IPv6] eth_send failed: %d\n", e);
        return e;
    }

    return ERR_OK;
}

void ipv6_get_lladdr(uint8_t* addr_out) {
    kmemcpy(addr_out, LINK_LOCAL, 16);
}

void ipv6_set_addr(const uint8_t* addr) {
    kmemcpy(GLOBAL_IPV6, addr, 16);
    kprintf("[IPv6] Global address set: %x:%x:%x:%x:%x:%x:%x:%x\n",
            (uint32_t)((addr[0]<<8)|addr[1]),
            (uint32_t)((addr[2]<<8)|addr[3]),
            (uint32_t)((addr[4]<<8)|addr[5]),
            (uint32_t)((addr[6]<<8)|addr[7]),
            (uint32_t)((addr[8]<<8)|addr[9]),
            (uint32_t)((addr[10]<<8)|addr[11]),
            (uint32_t)((addr[12]<<8)|addr[13]),
            (uint32_t)((addr[14]<<8)|addr[15]));
}

int ipv6_has_global_addr(void) {
    for (int i = 0; i < 16; i++)
        if (GLOBAL_IPV6[i] != 0) return 1;
    return 0;
}

/* ---- Multicast group management ---- */

int ipv6_mcast_join(const uint8_t* group_addr) {
    if (group_addr[0] != 0xFF) return ERR_INVAL; /* not a multicast addr */

    /* Check if already a member */
    if (ipv6_mcast_is_member(group_addr)) return ERR_OK;

    /* Find a free slot */
    for (int i = 0; i < IPV6_MAX_MCAST_GROUPS; i++) {
        if (!ipv6_mcast_groups[i].used) {
            kmemcpy(ipv6_mcast_groups[i].addr, group_addr, 16);
            ipv6_mcast_groups[i].used = 1;
            kprintf("[IPv6] Joined multicast group %x:%x:%x:%x:%x:%x:%x:%x\n",
                    (uint32_t)((group_addr[0]<<8)|group_addr[1]),
                    (uint32_t)((group_addr[2]<<8)|group_addr[3]),
                    (uint32_t)((group_addr[4]<<8)|group_addr[5]),
                    (uint32_t)((group_addr[6]<<8)|group_addr[7]),
                    (uint32_t)((group_addr[8]<<8)|group_addr[9]),
                    (uint32_t)((group_addr[10]<<8)|group_addr[11]),
                    (uint32_t)((group_addr[12]<<8)|group_addr[13]),
                    (uint32_t)((group_addr[14]<<8)|group_addr[15]));
            ipv6_mcast_update_mta();
            mldv1_send_report(group_addr);
            return ERR_OK;
        }
    }
    return ERR_NOSPACE;
}

int ipv6_mcast_leave(const uint8_t* group_addr) {
    for (int i = 0; i < IPV6_MAX_MCAST_GROUPS; i++) {
        if (ipv6_mcast_groups[i].used &&
            kmemcmp(ipv6_mcast_groups[i].addr, group_addr, 16) == 0) {
            ipv6_mcast_groups[i].used = 0;
            kprintf("[IPv6] Left multicast group\n");
            mldv1_send_done(ipv6_mcast_groups[i].addr);
            ipv6_mcast_update_mta();
            return ERR_OK;
        }
    }
    return ERR_NOENT;
}

int ipv6_mcast_is_member(const uint8_t* group_addr) {
    for (int i = 0; i < IPV6_MAX_MCAST_GROUPS; i++) {
        if (ipv6_mcast_groups[i].used &&
            kmemcmp(ipv6_mcast_groups[i].addr, group_addr, 16) == 0)
            return 1;
    }
    return 0;
}

void ipv6_mcast_update_mta(void) {
    uint8_t mac[6];
    for (int i = 0; i < IPV6_MAX_MCAST_GROUPS; i++) {
        if (ipv6_mcast_groups[i].used) {
            mac[0] = 0x33; mac[1] = 0x33;
            mac[2] = ipv6_mcast_groups[i].addr[12];
            mac[3] = ipv6_mcast_groups[i].addr[13];
            mac[4] = ipv6_mcast_groups[i].addr[14];
            mac[5] = ipv6_mcast_groups[i].addr[15];
            e1000_mta_set(mac);
        }
    }
}

void ipv6_mcast_report_all(void) {
    for (int i = 0; i < IPV6_MAX_MCAST_GROUPS; i++) {
        if (ipv6_mcast_groups[i].used) {
            mldv1_send_report(ipv6_mcast_groups[i].addr);
        }
    }
}

void ipv6_init(void) {
    if (ipv6_initialized) return;

    kmemset(ipv6_dispatch, 0, sizeof(ipv6_dispatch));
    kmemset(GLOBAL_IPV6, 0, 16);
    kmemset(ipv6_mcast_groups, 0, sizeof(ipv6_mcast_groups));

    err_t e = eth_register(ETHERTYPE_IPV6, ipv6_eth_handler);
    if (e != ERR_OK) {
        kprintf("[IPv6] Failed to register handler: %d\n", e);
        return;
    }

    ndp_make_lladdr(nic.mac, LINK_LOCAL);
    kprintf("[IPv6] Initialized, LL=%x:%x:%x:%x:%x:%x:%x:%x\n",
            (uint32_t)((LINK_LOCAL[0]<<8)|LINK_LOCAL[1]),
            (uint32_t)((LINK_LOCAL[2]<<8)|LINK_LOCAL[3]),
            (uint32_t)((LINK_LOCAL[4]<<8)|LINK_LOCAL[5]),
            (uint32_t)((LINK_LOCAL[6]<<8)|LINK_LOCAL[7]),
            (uint32_t)((LINK_LOCAL[8]<<8)|LINK_LOCAL[9]),
            (uint32_t)((LINK_LOCAL[10]<<8)|LINK_LOCAL[11]),
            (uint32_t)((LINK_LOCAL[12]<<8)|LINK_LOCAL[13]),
            (uint32_t)((LINK_LOCAL[14]<<8)|LINK_LOCAL[15]));

    ipv6_initialized = 1;
}

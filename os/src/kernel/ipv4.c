#include "kernel.h"
#include "ipv4.h"
#include "eth.h"
#include "arp.h"
#include "route.h"
#include "nic.h"
#include "igmp.h"
#include "net_ns.h"

#define IPV4_TTL_DEFAULT 64
#define IPV4_ID_INIT     0x4000

static int ipv4_handler_registered = 0;

#define OUR_IPV4 (get_current_ns()->our_ipv4)
#define ipv4_initialized (get_current_ns()->ipv4_initialized)
#define ipv4_next_id (get_current_ns()->ipv4_next_id)
#define ipv4_dispatch (get_current_ns()->ipv4_dispatch)

static uint16_t ipv4_checksum(const void* hdr, int len) {
    uint32_t sum = 0;
    const uint16_t* p = (const uint16_t*)hdr;
    for (int i = 0; i < len / 2; i++) {
        sum += __builtin_bswap16(p[i]);
    }
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return ~sum & 0xFFFF;
}

int ipv4_register_handler(uint8_t protocol, ipv4_handler_t handler) {
    for (int i = 0; i < IPV4_DISPATCH_SLOTS; i++) {
        if (!ipv4_dispatch[i].handler) {
            ipv4_dispatch[i].protocol = protocol;
            ipv4_dispatch[i].handler = handler;
            return ERR_OK;
        }
    }
    return ERR_NOSPACE;
}

static uint16_t ipv4_htons(uint16_t v) {
    return __builtin_bswap16(v);
}

int ipv4_send_from(ipv4_addr_t src, ipv4_addr_t dst, uint8_t protocol,
                   const uint8_t* data, uint32_t len) {
    if (!ipv4_initialized) return ERR_NOSYS;

    uint8_t buf[IPV4_HDR_LEN + len];
    ipv4_hdr_t* hdr = (ipv4_hdr_t*)buf;

    hdr->ver_ihl = 0x45;
    hdr->dscp_ecn = 0;
    hdr->total_len = ipv4_htons(IPV4_HDR_LEN + len);
    hdr->id = ipv4_htons(ipv4_next_id++);
    hdr->flags_frag = ipv4_htons(0x4000);
    hdr->ttl = IPV4_TTL_DEFAULT;
    hdr->protocol = protocol;
    kmemcpy(hdr->src, src.bytes, 4);
    kmemcpy(hdr->dst, dst.bytes, 4);
    hdr->checksum = 0;
    hdr->checksum = ipv4_htons(ipv4_checksum(hdr, IPV4_HDR_LEN));

    kmemcpy(buf + IPV4_HDR_LEN, data, len);

    ipv4_addr_t bcast = ipv4_from_bytes(255, 255, 255, 255);
    if (ipv4_addr_equal(dst, bcast)) {
        uint8_t bmac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        return eth_send(bmac, ETHERTYPE_IPV4, buf, IPV4_HDR_LEN + len);
    }

    ipv4_addr_t next_hop;
    int e = route_lookup_v4(dst, &next_hop);
    if (e != ERR_OK) {
        kprintf("[IPv4] No route to %d.%d.%d.%d\n",
                dst.bytes[0], dst.bytes[1], dst.bytes[2], dst.bytes[3]);
        return e;
    }

    uint8_t next_mac[6];
    e = arp_resolve(next_hop, next_mac, 2000);
    if (e != ERR_OK) {
        kprintf("[IPv4] ARP failed for %d.%d.%d.%d\n",
                next_hop.bytes[0], next_hop.bytes[1],
                next_hop.bytes[2], next_hop.bytes[3]);
        return e;
    }

    e = eth_send(next_mac, ETHERTYPE_IPV4, buf, IPV4_HDR_LEN + len);
    if (e != ERR_OK) {
        kprintf("[IPv4] eth_send failed: %d\n", e);
        return e;
    }

    return ERR_OK;
}

int ipv4_send(ipv4_addr_t dst, uint8_t protocol,
              const uint8_t* data, uint32_t len) {
    return ipv4_send_from(OUR_IPV4, dst, protocol, data, len);
}

void ipv4_set_addr(ipv4_addr_t addr) {
    OUR_IPV4 = addr;
    get_current_ns()->ipv4_prefix_len = 24;
    kprintf("[IPv4] Address set to %d.%d.%d.%d\n",
            addr.bytes[0], addr.bytes[1], addr.bytes[2], addr.bytes[3]);
}

void ipv4_set_addr_prefix(ipv4_addr_t addr, int prefix_len) {
    OUR_IPV4 = addr;
    get_current_ns()->ipv4_prefix_len = prefix_len;
    kprintf("[IPv4] Address set to %d.%d.%d.%d/%d\n",
            addr.bytes[0], addr.bytes[1], addr.bytes[2], addr.bytes[3], prefix_len);
}

ipv4_addr_t ipv4_get_addr(void) {
    return OUR_IPV4;
}

int ipv4_get_prefix_len(void) {
    return get_current_ns()->ipv4_prefix_len;
}

static void ipv4_dispatch_pkt(uint8_t protocol, ipv4_addr_t src,
                               ipv4_addr_t dst,
                               const uint8_t* payload, uint32_t len) {
    for (int i = 0; i < IPV4_DISPATCH_SLOTS; i++) {
        if (ipv4_dispatch[i].handler &&
            ipv4_dispatch[i].protocol == protocol) {
            ipv4_dispatch[i].handler(src, dst, protocol, payload, len);
        }
    }
}

static void ipv4_eth_handler(const uint8_t* src_mac, uint16_t type,
                              const uint8_t* data, uint32_t len) {
    (void)src_mac; (void)type;
    if (!ipv4_initialized) return;
    if (len < IPV4_HDR_LEN) return;

    const ipv4_hdr_t* hdr = (const ipv4_hdr_t*)data;
    uint8_t ver_ihl = hdr->ver_ihl;

    if ((ver_ihl >> 4) != 4) return;
    int ihl = (ver_ihl & 0x0F) * 4;
    if (ihl < IPV4_HDR_LEN || (uint32_t)ihl > len) return;

    if (ipv4_checksum(hdr, ihl) != 0) return;

    ipv4_addr_t src;
    ipv4_addr_t dst;
    kmemcpy(src.bytes, hdr->src, 4);
    kmemcpy(dst.bytes, hdr->dst, 4);

    int is_mcast = (dst.bytes[0] & 0xF0) == 0xE0;
    ipv4_addr_t bcast = ipv4_from_bytes(255, 255, 255, 255);
    if (!is_mcast && !ipv4_addr_equal(dst, OUR_IPV4) && !ipv4_addr_equal(dst, bcast)) return;

    uint8_t protocol = hdr->protocol;
    uint32_t payload_len = len - ihl;
    const uint8_t* payload = data + ihl;

    ipv4_dispatch_pkt(protocol, src, dst, payload, payload_len);
}

void ipv4_init(void) {
    if (ipv4_initialized) return;

    ipv4_next_id = IPV4_ID_INIT;

    if (!ipv4_handler_registered) {
        err_t e = eth_register(ETHERTYPE_IPV4, ipv4_eth_handler);
        if (e != ERR_OK) {
            kprintf("[IPv4] Failed to register handler: %d\n", e);
            return;
        }
        ipv4_handler_registered = 1;
    }

    kprintf("[IPv4] Initialized, IP=%d.%d.%d.%d\n",
            OUR_IPV4.bytes[0], OUR_IPV4.bytes[1],
            OUR_IPV4.bytes[2], OUR_IPV4.bytes[3]);

    igmp_init();

    ipv4_initialized = 1;
}

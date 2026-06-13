#include "kernel.h"
#include "route.h"

static route_entry_t route_table[ROUTE_TABLE_SIZE];
static int route_initialized = 0;

void route_clear(void) {
    kmemset(route_table, 0, sizeof(route_table));
}

void route_init(void) {
    if (route_initialized) return;

    kmemset(route_table, 0, sizeof(route_table));

    /* Directly connected: 10.0.2.0/24 (SLiRP default subnet) */
    route_add_v4(ipv4_from_bytes(10, 0, 2, 0), 24, ipv4_from_bytes(0, 0, 0, 0));

    /* Default gateway: 0.0.0.0/0 -> 10.0.2.2 */
    route_add_v4(ipv4_from_bytes(0, 0, 0, 0), 0, ipv4_from_bytes(10, 0, 2, 2));

    route_initialized = 1;
    kprintf("[ROUTE] Initialized\n");
}

int route_add_v4(ipv4_addr_t dst, int prefix_len, ipv4_addr_t gateway) {
    if (prefix_len < 0 || prefix_len > 32) return ERR_INVAL;
    for (int i = 0; i < ROUTE_TABLE_SIZE; i++) {
        if (!route_table[i].used) {
            route_table[i].used = 1;
            route_table[i].af = AF_INET;
            route_table[i].dst.v4 = dst;
            route_table[i].prefix_len = prefix_len;
            route_table[i].gw.v4 = gateway;
            route_table[i].metric = 0;
            kprintf("[ROUTE] Add v4 %d.%d.%d.%d/%d -> %d.%d.%d.%d\n",
                    dst.bytes[0], dst.bytes[1], dst.bytes[2], dst.bytes[3],
                    prefix_len,
                    gateway.bytes[0], gateway.bytes[1],
                    gateway.bytes[2], gateway.bytes[3]);
            return ERR_OK;
        }
    }
    kprintf("[ROUTE] Table full\n");
    return ERR_NOSPACE;
}

int route_add_v6(const uint8_t* dst, int prefix_len, const uint8_t* gateway) {
    if (prefix_len < 0 || prefix_len > 128) return ERR_INVAL;
    for (int i = 0; i < ROUTE_TABLE_SIZE; i++) {
        if (!route_table[i].used) {
            route_table[i].used = 1;
            route_table[i].af = AF_INET6;
            kmemcpy(route_table[i].dst.v6, dst, 16);
            route_table[i].prefix_len = prefix_len;
            kmemcpy(route_table[i].gw.v6, gateway, 16);
            route_table[i].metric = 0;
            kprintf("[ROUTE] Add v6 %x:%x:%x:%x:%x:%x:%x:%x/%d\n",
                    (uint32_t)((dst[0]<<8)|dst[1]),
                    (uint32_t)((dst[2]<<8)|dst[3]),
                    (uint32_t)((dst[4]<<8)|dst[5]),
                    (uint32_t)((dst[6]<<8)|dst[7]),
                    (uint32_t)((dst[8]<<8)|dst[9]),
                    (uint32_t)((dst[10]<<8)|dst[11]),
                    (uint32_t)((dst[12]<<8)|dst[13]),
                    (uint32_t)((dst[14]<<8)|dst[15]),
                    prefix_len);
            return ERR_OK;
        }
    }
    kprintf("[ROUTE] Table full\n");
    return ERR_NOSPACE;
}

int route_lookup_v4(ipv4_addr_t dst, ipv4_addr_t* next_hop) {
    int best_idx = -1;
    int best_prefix = -1;

    for (int i = 0; i < ROUTE_TABLE_SIZE; i++) {
        if (!route_table[i].used || route_table[i].af != AF_INET) continue;

        /* Build netmask from prefix_len */
        uint32_t mask = 0;
        int pl = route_table[i].prefix_len;
        if (pl > 32) pl = 32;
        if (pl > 0) {
            mask = (uint32_t)(0xFFFFFFFFULL << (32 - pl));
        }

        uint32_t dst_raw = ipv4_to_u32(dst);
        uint32_t route_dst = ipv4_to_u32(route_table[i].dst.v4);

        if ((dst_raw & mask) == (route_dst & mask)) {
            if (route_table[i].prefix_len > best_prefix) {
                best_prefix = route_table[i].prefix_len;
                best_idx = i;
            }
        }
    }

    if (best_idx < 0) return ERR_NOENT;

    if (ipv4_addr_equal(route_table[best_idx].gw.v4, ipv4_from_bytes(0, 0, 0, 0))) {
        /* Directly connected — destination IS the next-hop */
        *next_hop = dst;
    } else {
        *next_hop = route_table[best_idx].gw.v4;
    }
    return ERR_OK;
}

static int ipv6_prefix_match(const uint8_t* a, const uint8_t* b, int prefix_len) {
    int full_bytes = prefix_len / 8;
    int remaining_bits = prefix_len % 8;
    for (int i = 0; i < full_bytes && i < 16; i++) {
        if (a[i] != b[i]) return 0;
    }
    if (remaining_bits > 0 && full_bytes < 16) {
        uint8_t mask = (uint8_t)(0xFF << (8 - remaining_bits));
        if ((a[full_bytes] & mask) != (b[full_bytes] & mask)) return 0;
    }
    return 1;
}

int route_lookup_v6(const uint8_t* dst, uint8_t* next_hop) {
    int best_idx = -1;
    int best_prefix = -1;

    for (int i = 0; i < ROUTE_TABLE_SIZE; i++) {
        if (!route_table[i].used || route_table[i].af != AF_INET6) continue;
        if (ipv6_prefix_match(dst, route_table[i].dst.v6, route_table[i].prefix_len)) {
            if (route_table[i].prefix_len > best_prefix) {
                best_prefix = route_table[i].prefix_len;
                best_idx = i;
            }
        }
    }

    if (best_idx < 0) return ERR_NOENT;

    /* Check if gateway is :: (directly connected) */
    uint8_t zero[16];
    kmemset(zero, 0, 16);
    if (kmemcmp(route_table[best_idx].gw.v6, zero, 16) == 0) {
        kmemcpy(next_hop, dst, 16);
    } else {
        kmemcpy(next_hop, route_table[best_idx].gw.v6, 16);
    }
    return ERR_OK;
}

#include "kernel.h"
#include "route.h"
#include "net_ns.h"
#include "sync.h"

#define route_initialized (get_current_ns()->route_initialized)
#define route_table (get_current_ns()->route_table)
#define route_lock (get_current_ns()->route_lock)

void route_clear(void) {
    cpu_flags_t flags;
    spinlock_acquire(&route_lock, &flags);
    for (int i = 0; i < ROUTE_TABLE_SIZE; i++)
        route_table[i].used = 0;
    spinlock_release(&route_lock, flags);
}

int route_del_v4(ipv4_addr_t dst, int prefix_len) {
    cpu_flags_t flags;
    spinlock_acquire(&route_lock, &flags);
    for (int i = 0; i < ROUTE_TABLE_SIZE; i++) {
        if (route_table[i].used && route_table[i].af == AF_INET &&
            route_table[i].prefix_len == prefix_len &&
            ipv4_addr_equal(route_table[i].dst.v4, dst)) {
            route_table[i].used = 0;
            spinlock_release(&route_lock, flags);
            return ERR_OK;
        }
    }
    spinlock_release(&route_lock, flags);
    return ERR_NOENT;
}

int route_del_v6(const uint8_t* dst, int prefix_len) {
    cpu_flags_t flags;
    spinlock_acquire(&route_lock, &flags);
    for (int i = 0; i < ROUTE_TABLE_SIZE; i++) {
        if (route_table[i].used && route_table[i].af == AF_INET6 &&
            route_table[i].prefix_len == prefix_len &&
            kmemcmp(route_table[i].dst.v6, dst, 16) == 0) {
            route_table[i].used = 0;
            spinlock_release(&route_lock, flags);
            return ERR_OK;
        }
    }
    spinlock_release(&route_lock, flags);
    return ERR_NOENT;
}

void route_init(void) {
    if (route_initialized) return;

    spinlock_init(&route_lock, "route_lock");
    route_initialized = 1;

    route_add_v4(ipv4_from_bytes(10, 0, 2, 0), 24, ipv4_from_bytes(0, 0, 0, 0));
    route_add_v4(ipv4_from_bytes(0, 0, 0, 0), 0, ipv4_from_bytes(10, 0, 2, 2));

    kprintf("[ROUTE] Initialized\n");
}

int route_add_v4(ipv4_addr_t dst, int prefix_len, ipv4_addr_t gateway) {
    if (prefix_len < 0 || prefix_len > 32) return ERR_INVAL;
    cpu_flags_t flags;
    spinlock_acquire(&route_lock, &flags);
    for (int i = 0; i < ROUTE_TABLE_SIZE; i++) {
        if (!route_table[i].used) {
            route_table[i].used = 1;
            route_table[i].af = AF_INET;
            route_table[i].dst.v4 = dst;
            route_table[i].prefix_len = prefix_len;
            route_table[i].gw.v4 = gateway;
            route_table[i].metric = 0;
            spinlock_release(&route_lock, flags);
            return ERR_OK;
        }
    }
    spinlock_release(&route_lock, flags);
    return ERR_NOSPACE;
}

int route_add_v6(const uint8_t* dst, int prefix_len, const uint8_t* gateway) {
    if (prefix_len < 0 || prefix_len > 128) return ERR_INVAL;
    cpu_flags_t flags;
    spinlock_acquire(&route_lock, &flags);
    for (int i = 0; i < ROUTE_TABLE_SIZE; i++) {
        if (!route_table[i].used) {
            route_table[i].used = 1;
            route_table[i].af = AF_INET6;
            kmemcpy(route_table[i].dst.v6, dst, 16);
            route_table[i].prefix_len = prefix_len;
            kmemcpy(route_table[i].gw.v6, gateway, 16);
            route_table[i].metric = 0;
            spinlock_release(&route_lock, flags);
            return ERR_OK;
        }
    }
    spinlock_release(&route_lock, flags);
    return ERR_NOSPACE;
}

int route_lookup_v4(ipv4_addr_t dst, ipv4_addr_t* next_hop) {
    int best_idx = -1;
    int best_prefix = -1;

    cpu_flags_t flags;
    spinlock_acquire(&route_lock, &flags);
    for (int i = 0; i < ROUTE_TABLE_SIZE; i++) {
        if (!route_table[i].used || route_table[i].af != AF_INET) continue;

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

    if (best_idx < 0) {
        spinlock_release(&route_lock, flags);
        return ERR_NOENT;
    }

    ipv4_addr_t gw = route_table[best_idx].gw.v4;
    int is_direct = ipv4_addr_equal(gw, ipv4_from_bytes(0, 0, 0, 0));
    spinlock_release(&route_lock, flags);

    if (is_direct) {
        *next_hop = dst;
    } else {
        *next_hop = gw;
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

    cpu_flags_t flags;
    spinlock_acquire(&route_lock, &flags);
    for (int i = 0; i < ROUTE_TABLE_SIZE; i++) {
        if (!route_table[i].used || route_table[i].af != AF_INET6) continue;
        if (ipv6_prefix_match(dst, route_table[i].dst.v6, route_table[i].prefix_len)) {
            if (route_table[i].prefix_len > best_prefix) {
                best_prefix = route_table[i].prefix_len;
                best_idx = i;
            }
        }
    }

    if (best_idx < 0) {
        spinlock_release(&route_lock, flags);
        return ERR_NOENT;
    }

    uint8_t zero[16];
    kmemset(zero, 0, 16);
    int is_direct = (kmemcmp(route_table[best_idx].gw.v6, zero, 16) == 0);
    if (is_direct) {
        kmemcpy(next_hop, dst, 16);
    } else {
        kmemcpy(next_hop, route_table[best_idx].gw.v6, 16);
    }
    spinlock_release(&route_lock, flags);
    return ERR_OK;
}

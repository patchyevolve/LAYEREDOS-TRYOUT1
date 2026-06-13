#include "kernel.h"
#include "ndp.h"
#include "icmpv6.h"
#include "nic.h"
#include "eth.h"
#include "hal.h"
#include "sched.h"
#include "sync.h"

static ndp_cache_entry_t ndp_cache[NDP_CACHE_SIZE];
static int ndp_initialized = 0;
static spinlock_t ndp_lock;

static void ndp_cache_evict(void) {
    int lru = 0;
    uint64_t oldest = hal_timer_get_ns();
    for (int i = 0; i < NDP_CACHE_SIZE; i++) {
        if (!ndp_cache[i].valid) { lru = i; break; }
        if (ndp_cache[i].last_seen < oldest) {
            oldest = ndp_cache[i].last_seen;
            lru = i;
        }
    }
    ndp_cache[lru].valid = 0;
}

void ndp_cache_update(const uint8_t* ipv6, const uint8_t* mac) {
    if (!ndp_initialized) return;

    cpu_flags_t flags;
    spinlock_acquire(&ndp_lock, &flags);

    for (int i = 0; i < NDP_CACHE_SIZE; i++) {
        if (ndp_cache[i].valid &&
            kmemcmp(ndp_cache[i].ipv6, ipv6, IPV6_ADDR_LEN) == 0) {
            kmemcpy(ndp_cache[i].mac, mac, 6);
            ndp_cache[i].last_seen = hal_timer_get_ns();
            spinlock_release(&ndp_lock, flags);
            return;
        }
    }

    int slot = -1;
    for (int i = 0; i < NDP_CACHE_SIZE; i++) {
        if (!ndp_cache[i].valid) { slot = i; break; }
    }
    if (slot < 0) {
        ndp_cache_evict();
        for (int i = 0; i < NDP_CACHE_SIZE; i++) {
            if (!ndp_cache[i].valid) { slot = i; break; }
        }
        if (slot < 0) { spinlock_release(&ndp_lock, flags); return; }
    }

    ndp_cache[slot].valid = 1;
    kmemcpy(ndp_cache[slot].ipv6, ipv6, IPV6_ADDR_LEN);
    kmemcpy(ndp_cache[slot].mac, mac, 6);
    ndp_cache[slot].last_seen = hal_timer_get_ns();

    spinlock_release(&ndp_lock, flags);
}

int ndp_cache_lookup(const uint8_t* ipv6, uint8_t* mac) {
    if (!ndp_initialized) return 0;

    cpu_flags_t flags;
    spinlock_acquire(&ndp_lock, &flags);

    for (int i = 0; i < NDP_CACHE_SIZE; i++) {
        if (ndp_cache[i].valid &&
            kmemcmp(ndp_cache[i].ipv6, ipv6, IPV6_ADDR_LEN) == 0) {
            kmemcpy(mac, ndp_cache[i].mac, 6);
            ndp_cache[i].last_seen = hal_timer_get_ns();
            spinlock_release(&ndp_lock, flags);
            return 1;
        }
    }

    spinlock_release(&ndp_lock, flags);
    return 0;
}

void ndp_make_lladdr(const uint8_t* mac, uint8_t* ipv6_out) {
    /* fe80:: + EUI-64: invert bit 7 of first byte, insert ff:fe in middle */
    kmemset(ipv6_out, 0, IPV6_ADDR_LEN);
    ipv6_out[0] = 0xFE;
    ipv6_out[1] = 0x80;

    ipv6_out[8] = mac[0] ^ 0x02;
    ipv6_out[9] = mac[1];
    ipv6_out[10] = mac[2];
    ipv6_out[11] = 0xFF;
    ipv6_out[12] = 0xFE;
    ipv6_out[13] = mac[3];
    ipv6_out[14] = mac[4];
    ipv6_out[15] = mac[5];
}

void ndp_make_solicited_node(const uint8_t* ipv6, uint8_t* mc_out) {
    /* FF02::1:FFxx:xxxx where xx:xxxx = last 3 bytes of target address */
    kmemset(mc_out, 0, IPV6_ADDR_LEN);
    mc_out[0]  = 0xFF;
    mc_out[1]  = 0x02;
    mc_out[11] = 0x01;
    mc_out[12] = 0xFF;
    mc_out[13] = ipv6[13];
    mc_out[14] = ipv6[14];
    mc_out[15] = ipv6[15];
}

int ndp_resolve(const uint8_t* ipv6, uint8_t* mac, int timeout_ms) {
    if (ndp_cache_lookup(ipv6, mac)) return ERR_OK;

    kprintf("[NDP] Resolving %x:%x:%x:%x:%x:%x:%x:%x...\n",
            (uint32_t)((ipv6[0]<<8)|ipv6[1]),
            (uint32_t)((ipv6[2]<<8)|ipv6[3]),
            (uint32_t)((ipv6[4]<<8)|ipv6[5]),
            (uint32_t)((ipv6[6]<<8)|ipv6[7]),
            (uint32_t)((ipv6[8]<<8)|ipv6[9]),
            (uint32_t)((ipv6[10]<<8)|ipv6[11]),
            (uint32_t)((ipv6[12]<<8)|ipv6[13]),
            (uint32_t)((ipv6[14]<<8)|ipv6[15]));

    int e = icmpv6_send_ns(ipv6);
    if (e != ERR_OK) return e;

    int step = 50;
    int steps = timeout_ms / step;
    for (int i = 0; i < steps; i++) {
        eth_rx_poll();
        if (ndp_cache_lookup(ipv6, mac)) return ERR_OK;
        thread_sleep((uint64_t)step);
    }

    return ERR_TIMEOUT;
}

err_t ndp_init(void) {
    if (ndp_initialized) return ERR_OK;

    spinlock_init(&ndp_lock, "ndp");
    kmemset(ndp_cache, 0, sizeof(ndp_cache));

    ndp_initialized = 1;

    /* Derive and print our link-local address */
    uint8_t lladdr[IPV6_ADDR_LEN];
    ndp_make_lladdr(nic.mac, lladdr);
    kprintf("[NDP] Initialized (cache %d entries)\n", NDP_CACHE_SIZE);
    kprintf("[NDP] Link-local: %x:%x:%x:%x:%x:%x:%x:%x\n",
            (uint32_t)((lladdr[0]<<8)|lladdr[1]),
            (uint32_t)((lladdr[2]<<8)|lladdr[3]),
            (uint32_t)((lladdr[4]<<8)|lladdr[5]),
            (uint32_t)((lladdr[6]<<8)|lladdr[7]),
            (uint32_t)((lladdr[8]<<8)|lladdr[9]),
            (uint32_t)((lladdr[10]<<8)|lladdr[11]),
            (uint32_t)((lladdr[12]<<8)|lladdr[13]),
            (uint32_t)((lladdr[14]<<8)|lladdr[15]));

    return ERR_OK;
}

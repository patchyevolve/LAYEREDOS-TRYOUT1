#include "kernel.h"
#include "arp.h"
#include "eth.h"
#include "nic.h"
#include "ipv4.h"
#include "hal.h"
#include "sched.h"
#include "sync.h"
#include "net_ns.h"
#include "work.h"

/* Global (not per-namespace): eth handler registration */
static int arp_handler_registered = 0;

/* Per-namespace state accessed via get_current_ns() */
#define arp_cache (get_current_ns()->arp_cache)
#define arp_initialized (get_current_ns()->arp_initialized)
#define arp_lock (get_current_ns()->arp_lock)

/* Deferred ARP reply ring buffer.
 * Queued inside arp_handle (which may be called from IRQ context with IF=0).
 * Flushed from thread context via system work queue. */
#define ARP_REPLY_QUEUE_MAX 8
static struct {
    uint8_t target_mac[6];
    ipv4_addr_t target_ip;
} arp_reply_queue[ARP_REPLY_QUEUE_MAX];
static volatile int arp_reply_head = 0;
static volatile int arp_reply_tail = 0;
static spinlock_t arp_reply_lock;
static work_item_t arp_reply_work_item;

static void arp_add_cache(ipv4_addr_t ip, const uint8_t* mac) {
    int lru = 0;
    uint64_t oldest = hal_timer_get_ns();

    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!arp_cache[i].valid) {
            lru = i;
            break;
        }
        if (arp_cache[i].last_used < oldest) {
            oldest = arp_cache[i].last_used;
            lru = i;
        }
    }

    arp_cache[lru].ip = ip;
    kmemcpy(arp_cache[lru].mac, mac, 6);
    arp_cache[lru].valid = 1;
    arp_cache[lru].last_used = hal_timer_get_ns();
}

static int arp_find_cache(ipv4_addr_t ip, uint8_t* mac) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && ipv4_addr_equal(arp_cache[i].ip, ip)) {
            arp_cache[i].last_used = hal_timer_get_ns();
            kmemcpy(mac, arp_cache[i].mac, 6);
            return 1;
        }
    }
    return 0;
}

static void arp_send_request(ipv4_addr_t target_ip) {
    uint8_t broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    arp_pkt_t req;

    req.htype = __builtin_bswap16(ARP_HTYPE_ETHER);
    req.ptype = __builtin_bswap16(ARP_PTYPE_IPV4);
    req.hlen  = ARP_HLEN_ETHER;
    req.plen  = ARP_PLEN_IPV4;
    req.oper  = __builtin_bswap16(ARP_OP_REQUEST);
    kmemcpy(req.sha, nic.mac, 6);
    kmemcpy(req.spa, ipv4_get_addr().bytes, 4);
    kmemset(req.tha, 0, 6);
    kmemcpy(req.tpa, target_ip.bytes, 4);

    err_t e = eth_send(broadcast, ETHERTYPE_ARP, (const uint8_t*)&req, sizeof(req));
    kprintf("[ARP] Sent request to %d.%d.%d.%d: %d\n",
            target_ip.bytes[0], target_ip.bytes[1],
            target_ip.bytes[2], target_ip.bytes[3], e);
    eth_rx_poll();
}

static void arp_send_reply(const uint8_t* target_mac, ipv4_addr_t target_ip) {
    arp_pkt_t rep;

    rep.htype = __builtin_bswap16(ARP_HTYPE_ETHER);
    rep.ptype = __builtin_bswap16(ARP_PTYPE_IPV4);
    rep.hlen  = ARP_HLEN_ETHER;
    rep.plen  = ARP_PLEN_IPV4;
    rep.oper  = __builtin_bswap16(ARP_OP_REPLY);
    kmemcpy(rep.sha, nic.mac, 6);
    kmemcpy(rep.spa, ipv4_get_addr().bytes, 4);
    kmemcpy(rep.tha, target_mac, 6);
    kmemcpy(rep.tpa, target_ip.bytes, 4);

    eth_send(target_mac, ETHERTYPE_ARP, (const uint8_t*)&rep, sizeof(rep));
}

/* Deferred ARP reply flusher — runs from system work queue (thread context). */
static void arp_flush_replies(void* arg) {
    (void)arg;
    cpu_flags_t _f;
    for (;;) {
        spinlock_acquire(&arp_reply_lock, &_f);
        if (arp_reply_head == arp_reply_tail) {
            spinlock_release(&arp_reply_lock, _f);
            break;
        }
        int idx = arp_reply_head % ARP_REPLY_QUEUE_MAX;
        uint8_t mac[6];
        ipv4_addr_t ip;
        kmemcpy(mac, arp_reply_queue[idx].target_mac, 6);
        kmemcpy(ip.bytes, arp_reply_queue[idx].target_ip.bytes, 4);
        arp_reply_head++;
        spinlock_release(&arp_reply_lock, _f);
        /* Send from thread context (IF=1) — no spinlock, no IRQ context */
        arp_send_reply(mac, ip);
    }
}

static void arp_handle(const uint8_t* src_mac, uint16_t type,
                        const uint8_t* data, uint32_t len) {
    (void)src_mac;
    (void)type;
    if (len < sizeof(arp_pkt_t)) return;

    cpu_flags_t flags;
    spinlock_acquire(&arp_lock, &flags);

    const arp_pkt_t* pkt = (const arp_pkt_t*)data;

    uint16_t htype = __builtin_bswap16(pkt->htype);
    uint16_t ptype = __builtin_bswap16(pkt->ptype);
    uint16_t oper  = __builtin_bswap16(pkt->oper);

    if (htype != ARP_HTYPE_ETHER || ptype != ARP_PTYPE_IPV4) { spinlock_release(&arp_lock, flags); return; }
    if (pkt->hlen != ARP_HLEN_ETHER || pkt->plen != ARP_PLEN_IPV4) { spinlock_release(&arp_lock, flags); return; }

    ipv4_addr_t sender_ip;
    ipv4_addr_t target_ip;
    kmemcpy(sender_ip.bytes, pkt->spa, 4);
    kmemcpy(target_ip.bytes, pkt->tpa, 4);

    int need_reply = (oper == ARP_OP_REQUEST && ipv4_addr_equal(target_ip, ipv4_get_addr()));

    arp_add_cache(sender_ip, pkt->sha);
    spinlock_release(&arp_lock, flags);

    kprintf("[ARP] handle: oper=%d sip=%d.%d.%d.%d tip=%d.%d.%d.%d\n",
            oper,
            sender_ip.bytes[0], sender_ip.bytes[1],
            sender_ip.bytes[2], sender_ip.bytes[3],
            target_ip.bytes[0], target_ip.bytes[1],
            target_ip.bytes[2], target_ip.bytes[3]);

    /* Defer ARP reply to thread context — never send from IRQ handler (IF=0) or
     * while holding a spinlock (e1000_send may spin on TX descriptor completion
     * which requires timer ISR → interrupts must be enabled). */
    if (need_reply) {
        cpu_flags_t _f;
        spinlock_acquire(&arp_reply_lock, &_f);
        int next = (arp_reply_tail + 1) % ARP_REPLY_QUEUE_MAX;
        if (next != arp_reply_head) {
            kmemcpy(arp_reply_queue[arp_reply_tail].target_mac, pkt->sha, 6);
            kmemcpy(arp_reply_queue[arp_reply_tail].target_ip.bytes, sender_ip.bytes, 4);
            arp_reply_tail = next;
            work_queue_schedule(&system_wq, &arp_reply_work_item);
        }
        spinlock_release(&arp_reply_lock, _f);
    }
}

int arp_resolve(ipv4_addr_t ip, uint8_t* mac, int timeout_ms) {
    if (!arp_initialized) return ERR_NOSYS;
    if (!nic.present) return ERR_IO;

    cpu_flags_t flags;
    spinlock_acquire(&arp_lock, &flags);

    int found = arp_find_cache(ip, mac);
    spinlock_release(&arp_lock, flags);
    if (found) return ERR_OK;

    for (int retry = 0; retry < ARP_RESOLVE_RETRIES; retry++) {
        arp_send_request(ip);

        int step = 50;
        int steps = timeout_ms / step;
        if (steps < 1) steps = 1;

        for (int i = 0; i < steps; i++) {
            thread_sleep((uint64_t)step);
            eth_rx_poll();
            spinlock_acquire(&arp_lock, &flags);
            found = arp_find_cache(ip, mac);
            spinlock_release(&arp_lock, flags);
            if (found) return ERR_OK;
        }
    }

    return ERR_TIMEOUT;
}

void arp_set(ipv4_addr_t ip, const uint8_t* mac) {
    cpu_flags_t flags;
    spinlock_acquire(&arp_lock, &flags);
    arp_add_cache(ip, mac);
    spinlock_release(&arp_lock, flags);
}

int arp_del(ipv4_addr_t ip) {
    cpu_flags_t flags;
    spinlock_acquire(&arp_lock, &flags);
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && ipv4_addr_equal(arp_cache[i].ip, ip)) {
            arp_cache[i].valid = 0;
            spinlock_release(&arp_lock, flags);
            return ERR_OK;
        }
    }
    spinlock_release(&arp_lock, flags);
    return ERR_NOENT;
}

err_t arp_init(void) {
    if (arp_initialized) return ERR_OK;

    spinlock_init(&arp_lock, "arp");
    spinlock_init(&arp_reply_lock, "arp_reply");
    arp_reply_work_item.func = arp_flush_replies;
    arp_reply_work_item.data = NULL;

    if (!arp_handler_registered) {
        err_t e = eth_register(ETHERTYPE_ARP, arp_handle);
        if (e != ERR_OK) {
            kprintf("[ARP] Failed to register handler: %d\n", e);
            return e;
        }
        arp_handler_registered = 1;
    }

    arp_initialized = 1;
    kprintf("[ARP] Initialized (cache %d entries)\n", ARP_CACHE_SIZE);
    return ERR_OK;
}

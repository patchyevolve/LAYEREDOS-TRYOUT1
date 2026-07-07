#include "kernel.h"
#include "veth.h"
#include "eth.h"
#include "net_ns.h"
#include "sched.h"
#include "process.h"
#include "sync.h"

static veth_pair_t veth_pairs[VETH_MAX_PAIRS];
static uint64_t veth_mac_counter = 0x525400000001ULL;
static spinlock_t veth_lock;



void veth_init(void) {
    spinlock_init(&veth_lock, "veth_lock");
    kmemset(veth_pairs, 0, sizeof(veth_pairs));
}

static void veth_gen_mac(uint8_t* mac) {
    uint64_t c = __sync_fetch_and_add(&veth_mac_counter, 1);
    mac[0] = (uint8_t)(c >> 40);
    mac[1] = (uint8_t)(c >> 32);
    mac[2] = (uint8_t)(c >> 24);
    mac[3] = (uint8_t)(c >> 16);
    mac[4] = (uint8_t)(c >> 8);
    mac[5] = (uint8_t)(c);
}

veth_end_t* veth_find_end(const uint8_t* mac) {
    cpu_flags_t flags;
    spinlock_acquire(&veth_lock, &flags);
    for (int i = 0; i < VETH_MAX_PAIRS; i++) {
        if (!veth_pairs[i].used) continue;
        if (kmemcmp(veth_pairs[i].a.mac, mac, ETH_ALEN) == 0) {
            spinlock_release(&veth_lock, flags);
            return &veth_pairs[i].a;
        }
        if (kmemcmp(veth_pairs[i].b.mac, mac, ETH_ALEN) == 0) {
            spinlock_release(&veth_lock, flags);
            return &veth_pairs[i].b;
        }
    }
    spinlock_release(&veth_lock, flags);
    return NULL;
}

err_t veth_deliver(veth_end_t* end, const uint8_t* src_mac, uint16_t type,
                   const uint8_t* data, uint32_t len) {
    if (!end) return ERR_INVAL;

    cpu_flags_t flags;
    spinlock_acquire(&veth_lock, &flags);
    if (!end->used) {
        spinlock_release(&veth_lock, flags);
        return ERR_INVAL;
    }
    uint8_t mac_buf[ETH_ALEN];
    kmemcpy(mac_buf, end->mac, ETH_ALEN);
    net_ns_t* target_ns = end->ns;
    spinlock_release(&veth_lock, flags);

    uint32_t frame_len = ETH_HDR_LEN + len;
    if (frame_len < ETH_MIN_FRAME) frame_len = ETH_MIN_FRAME;
    if (frame_len > ETH_MAX_FRAME) return ERR_INVAL;

    uint8_t buf[ETH_MAX_FRAME];
    eth_hdr_t* hdr = (eth_hdr_t*)buf;
    kmemcpy(hdr->dst, mac_buf, ETH_ALEN);
    kmemcpy(hdr->src, src_mac, ETH_ALEN);
    hdr->type = __builtin_bswap16(type);
    kmemcpy(hdr->payload, data, len);

    net_ns_t* saved_ns = NULL;
    if (current_thread && current_thread->proc && current_thread->proc->net_ns) {
        saved_ns = current_thread->proc->net_ns;
        current_thread->proc->net_ns = target_ns;
    }

    eth_dispatch_frame(hdr->src, type, hdr->payload, len);

    if (saved_ns) current_thread->proc->net_ns = saved_ns;

    return ERR_OK;
}

err_t veth_pair_create(int* out_idx) {
    if (!out_idx) return ERR_INVAL;

    net_ns_t* ns = get_current_ns();

    cpu_flags_t flags;
    spinlock_acquire(&veth_lock, &flags);
    for (int i = 0; i < VETH_MAX_PAIRS; i++) {
        if (!veth_pairs[i].used) {
            net_ns_retain(ns);
            veth_pairs[i].used = 1;
            veth_pairs[i].a.used = 1;
            veth_pairs[i].a.ns = ns;
            veth_pairs[i].a.peer = &veth_pairs[i].b;
            veth_gen_mac(veth_pairs[i].a.mac);

            veth_pairs[i].b.used = 1;
            veth_pairs[i].b.ns = ns;
            veth_pairs[i].b.peer = &veth_pairs[i].a;
            veth_gen_mac(veth_pairs[i].b.mac);

            kstrncpy(veth_pairs[i].name, "veth", sizeof(veth_pairs[i].name) - 1);
            int nlen = kstrlen(veth_pairs[i].name);
            if (nlen < (int)sizeof(veth_pairs[i].name) - 2)
                veth_pairs[i].name[nlen] = '0' + (char)i;
            spinlock_release(&veth_lock, flags);
            *out_idx = i;
            return ERR_OK;
        }
    }
    spinlock_release(&veth_lock, flags);
    return ERR_NOSPACE;
}

err_t veth_end_move(int pair_idx, int end_sel, struct net_ns* target_ns) {
    if (pair_idx < 0 || pair_idx >= VETH_MAX_PAIRS) return ERR_INVAL;
    if (!target_ns) return ERR_INVAL;

    cpu_flags_t flags;
    spinlock_acquire(&veth_lock, &flags);
    if (!veth_pairs[pair_idx].used) {
        spinlock_release(&veth_lock, flags);
        return ERR_NOENT;
    }
    veth_end_t* end = (end_sel == 0) ? &veth_pairs[pair_idx].a
                                     : &veth_pairs[pair_idx].b;
    if (!end->used) {
        spinlock_release(&veth_lock, flags);
        return ERR_NOENT;
    }
    net_ns_release(end->ns);
    end->ns = target_ns;
    net_ns_retain(target_ns);
    spinlock_release(&veth_lock, flags);
    return ERR_OK;
}

const uint8_t* veth_get_mac(int pair_idx, int end_sel) {
    if (pair_idx < 0 || pair_idx >= VETH_MAX_PAIRS) return NULL;
    cpu_flags_t flags;
    spinlock_acquire(&veth_lock, &flags);
    if (!veth_pairs[pair_idx].used) {
        spinlock_release(&veth_lock, flags);
        return NULL;
    }
    veth_end_t* end = (end_sel == 0) ? &veth_pairs[pair_idx].a
                                     : &veth_pairs[pair_idx].b;
    if (!end->used) {
        spinlock_release(&veth_lock, flags);
        return NULL;
    }
    uint8_t* mac = end->mac;
    spinlock_release(&veth_lock, flags);
    return mac;
}

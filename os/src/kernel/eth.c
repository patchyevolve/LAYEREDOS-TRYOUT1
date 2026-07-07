#include "kernel.h"
#include "eth.h"
#include "nic.h"
#include "veth.h"
#include "sync.h"

#define ETH_MAX_HANDLERS 8

static int eth_initialized = 0;
static spinlock_t eth_lock;

static struct {
    uint16_t       type;
    eth_handler_t  handler;
} eth_handlers[ETH_MAX_HANDLERS];

err_t eth_init(void) {
    if (eth_initialized) return ERR_OK;
    spinlock_init(&eth_lock, "eth_lock");
    for (int i = 0; i < ETH_MAX_HANDLERS; i++) {
        eth_handlers[i].type = 0;
        eth_handlers[i].handler = NULL;
    }
    eth_initialized = 1;
    kprintf("[ETH] Ethernet layer initialized\n");
    return ERR_OK;
}

int eth_dispatch_frame(const uint8_t* src_mac, uint16_t type,
                       const uint8_t* data, uint32_t len) {
    eth_handler_t handler = NULL;
    cpu_flags_t flags;
    spinlock_acquire(&eth_lock, &flags);
    for (int i = 0; i < ETH_MAX_HANDLERS; i++) {
        if (eth_handlers[i].handler && eth_handlers[i].type == type) {
            handler = eth_handlers[i].handler;
            break;
        }
    }
    spinlock_release(&eth_lock, flags);
    if (handler) {
        handler(src_mac, type, data, len);
        return 1;
    }
    return 0;
}

int eth_try_veth(const uint8_t* dst_mac, uint16_t type,
                 const uint8_t* data, uint32_t len) {
    veth_end_t* end = veth_find_end(dst_mac);
    if (!end) return 0;
    /* Deliver to this end (the peer's side will receive it) */
    /* Use the peer's MAC as the source for the delivered frame */
    veth_deliver(end, end->peer->mac, type, data, len);
    return 1;
}

err_t eth_send(const uint8_t* dst_mac, uint16_t type,
               const uint8_t* data, uint32_t len) {
    /* Check veth pairs first */
    if (eth_try_veth(dst_mac, type, data, len))
        return ERR_OK;

    if (!nic.present) return ERR_IO;
    if (!eth_initialized) return ERR_NOSYS;

    uint32_t frame_len = ETH_HDR_LEN + len;
    if (frame_len < ETH_MIN_FRAME) frame_len = ETH_MIN_FRAME;
    if (frame_len > NIC_MAX_FRAME) return ERR_INVAL;

    uint8_t buf[NIC_MAX_FRAME];
    eth_hdr_t* hdr = (eth_hdr_t*)buf;

    kmemcpy(hdr->dst, dst_mac, ETH_ALEN);
    kmemcpy(hdr->src, nic.mac, ETH_ALEN);
    hdr->type = __builtin_bswap16(type);
    kmemcpy(hdr->payload, data, len);

    return nic.send(&nic, buf, frame_len);
}

void eth_rx_poll(void) {
    if (!nic.present || !eth_initialized) return;

    uint8_t buf[NIC_MAX_FRAME];
    int len;

    while ((len = nic.poll(&nic, buf, sizeof(buf))) > 0) {
        if ((uint32_t)len < ETH_HDR_LEN) continue;

        eth_hdr_t* hdr = (eth_hdr_t*)buf;
        uint16_t type = __builtin_bswap16(hdr->type);
        uint32_t payload_len = (uint32_t)len - ETH_HDR_LEN;

        eth_handler_t handler = NULL;
        cpu_flags_t flags;
        spinlock_acquire(&eth_lock, &flags);
        for (int i = 0; i < ETH_MAX_HANDLERS; i++) {
            if (eth_handlers[i].handler && eth_handlers[i].type == type) {
                handler = eth_handlers[i].handler;
                break;
            }
        }
        spinlock_release(&eth_lock, flags);

        if (handler) {
            handler(hdr->src, type, hdr->payload, payload_len);
        } else {
            kprintf("[ETH] Unhandled EtherType 0x%04x (len=%u)\n", type, payload_len);
        }
    }
}

err_t eth_register(uint16_t type, eth_handler_t handler) {
    if (!eth_initialized) return ERR_NOSYS;

    cpu_flags_t flags;
    spinlock_acquire(&eth_lock, &flags);
    for (int i = 0; i < ETH_MAX_HANDLERS; i++) {
        if (!eth_handlers[i].handler) {
            eth_handlers[i].type = type;
            eth_handlers[i].handler = handler;
            spinlock_release(&eth_lock, flags);
            kprintf("[ETH] Registered handler for EtherType 0x%04x\n", type);
            return ERR_OK;
        }
    }
    spinlock_release(&eth_lock, flags);
    return ERR_NOMEM;
}

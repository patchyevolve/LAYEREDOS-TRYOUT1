#include "kernel.h"
#include "eth.h"
#include "nic.h"

#define ETH_MAX_HANDLERS 8

static int eth_initialized = 0;

static struct {
    uint16_t       type;
    eth_handler_t  handler;
} eth_handlers[ETH_MAX_HANDLERS];

err_t eth_init(void) {
    if (eth_initialized) return ERR_OK;
    for (int i = 0; i < ETH_MAX_HANDLERS; i++) {
        eth_handlers[i].type = 0;
        eth_handlers[i].handler = NULL;
    }
    eth_initialized = 1;
    kprintf("[ETH] Ethernet layer initialized\n");
    return ERR_OK;
}

err_t eth_send(const uint8_t* dst_mac, uint16_t type,
               const uint8_t* data, uint32_t len) {
    if (!nic.present) return ERR_IO;
    if (!eth_initialized) return ERR_NOSYS;

    kprintf("[ETH TX] type=0x%04x len=%u dst=%02x:%02x:%02x:%02x:%02x:%02x\n",
            type, len,
            dst_mac[0], dst_mac[1], dst_mac[2],
            dst_mac[3], dst_mac[4], dst_mac[5]);

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

        kprintf("[ETH RX] type=0x%04x len=%u src=%02x:%02x:%02x:%02x:%02x:%02x\n",
                type, len,
                hdr->src[0], hdr->src[1], hdr->src[2],
                hdr->src[3], hdr->src[4], hdr->src[5]);

        int found = 0;
        for (int i = 0; i < ETH_MAX_HANDLERS; i++) {
            if (eth_handlers[i].handler && eth_handlers[i].type == type) {
                eth_handlers[i].handler(hdr->src, type, hdr->payload, payload_len);
                found = 1;
                break;
            }
        }

        if (!found) {
            kprintf("[ETH] Unhandled EtherType 0x%04x (len=%u)\n", type, payload_len);
        }
    }
}

err_t eth_register(uint16_t type, eth_handler_t handler) {
    if (!eth_initialized) return ERR_NOSYS;

    for (int i = 0; i < ETH_MAX_HANDLERS; i++) {
        if (!eth_handlers[i].handler) {
            eth_handlers[i].type = type;
            eth_handlers[i].handler = handler;
            kprintf("[ETH] Registered handler for EtherType 0x%04x\n", type);
            return ERR_OK;
        }
    }
    return ERR_NOMEM;
}

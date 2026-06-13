#ifndef ETH_H
#define ETH_H

#include "types.h"

#define ETH_ALEN        6
#define ETH_HDR_LEN     14
#define ETH_MIN_FRAME   60
#define ETH_MAX_FRAME   1518

#define ETHERTYPE_IPV4  0x0800
#define ETHERTYPE_ARP   0x0806
#define ETHERTYPE_IPV6  0x86DD
#define ETHERTYPE_LOOP  0x9000

typedef struct __attribute__((packed)) {
    uint8_t  dst[ETH_ALEN];
    uint8_t  src[ETH_ALEN];
    uint16_t type;
    uint8_t  payload[];
} eth_hdr_t;

typedef void (*eth_handler_t)(const uint8_t* src_mac, uint16_t type,
                               const uint8_t* data, uint32_t len);

err_t eth_send(const uint8_t* dst_mac, uint16_t type,
               const uint8_t* data, uint32_t len);
void  eth_rx_poll(void);
err_t eth_register(uint16_t type, eth_handler_t handler);
err_t eth_init(void);

#endif

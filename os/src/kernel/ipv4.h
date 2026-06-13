#ifndef IPV4_H
#define IPV4_H

#include "types.h"
#include "ip.h"

#define IPV4_PROTO_ICMP  1
#define IPV4_PROTO_TCP   6
#define IPV4_PROTO_UDP   17

typedef struct __attribute__((packed)) {
    uint8_t  ver_ihl;
    uint8_t  dscp_ecn;
    uint16_t total_len;
    uint16_t id;
    uint16_t flags_frag;
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t checksum;
    uint8_t  src[4];
    uint8_t  dst[4];
} ipv4_hdr_t;

#define IPV4_HDR_LEN 20

typedef void (*ipv4_handler_t)(ipv4_addr_t src, ipv4_addr_t dst,
                                uint8_t protocol,
                                const uint8_t* data, uint32_t len);

void ipv4_init(void);
int  ipv4_send(ipv4_addr_t dst, uint8_t protocol,
               const uint8_t* data, uint32_t len);
int  ipv4_send_from(ipv4_addr_t src, ipv4_addr_t dst, uint8_t protocol,
                    const uint8_t* data, uint32_t len);
int  ipv4_register_handler(uint8_t protocol, ipv4_handler_t handler);
void ipv4_set_addr(ipv4_addr_t addr);
ipv4_addr_t ipv4_get_addr(void);

#endif

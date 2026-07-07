#ifndef ARP_H
#define ARP_H

#include "types.h"
#include "ip.h"

#define ARP_CACHE_SIZE    16
#define ARP_HTYPE_ETHER   1
#define ARP_PTYPE_IPV4    0x0800
#define ARP_HLEN_ETHER    6
#define ARP_PLEN_IPV4     4
#define ARP_OP_REQUEST    1
#define ARP_OP_REPLY      2
#define ARP_RESOLVE_RETRIES 2
#define ARP_RESOLVE_TIMEOUT_MS 2000

typedef struct __attribute__((packed)) {
    uint16_t htype;
    uint16_t ptype;
    uint8_t  hlen;
    uint8_t  plen;
    uint16_t oper;
    uint8_t  sha[6];
    uint8_t  spa[4];
    uint8_t  tha[6];
    uint8_t  tpa[4];
} arp_pkt_t;

err_t arp_init(void);
int   arp_resolve(ipv4_addr_t ip, uint8_t* mac, int timeout_ms);
void  arp_set(ipv4_addr_t ip, const uint8_t* mac);
int   arp_del(ipv4_addr_t ip);

#endif

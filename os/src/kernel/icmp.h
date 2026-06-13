#ifndef ICMP_H
#define ICMP_H

#include "types.h"
#include "ip.h"

#define ICMP_ECHO_REPLY   0
#define ICMP_ECHO_REQUEST 8

typedef struct __attribute__((packed)) {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;
    uint16_t sequence;
    uint8_t  data[];
} icmpv4_hdr_t;

#define ICMPV4_HDR_LEN 8

void icmpv4_init(void);
int  icmpv4_ping(ipv4_addr_t dst, int timeout_ms);

#endif

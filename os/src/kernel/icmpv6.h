#ifndef ICMPV6_H
#define ICMPV6_H

#include "types.h"
#include "ip.h"

#define ICMPV6_ECHO_REQ   128
#define ICMPV6_ECHO_REPLY 129
#define ICMPV6_RS         133
#define ICMPV6_RA         134
#define ICMPV6_NS         135
#define ICMPV6_NA         136
#define ICMPV6_MLD_QUERY  130
#define ICMPV6_MLD_REPORT 131
#define ICMPV6_MLD_DONE   132

typedef struct __attribute__((packed)) {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint8_t  data[];
} icmpv6_hdr_t;

void icmpv6_init(void);
int  icmpv6_ping(const uint8_t* dst, int timeout_ms);
void icmpv6_send_rs(void);
int  icmpv6_send_ns(const uint8_t* target_ip);
void icmpv6_set_ra_callback(void (*cb)(const uint8_t* src, const uint8_t* data, uint32_t len));
void mldv1_send_report(const uint8_t* group_addr);
void mldv1_send_done(const uint8_t* group_addr);

#endif

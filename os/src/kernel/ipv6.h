#ifndef IPV6_H
#define IPV6_H

#include "types.h"
#include "ip.h"

#define IPV6_NEXT_ICMPV6  58
#define IPV6_NEXT_TCP     6
#define IPV6_NEXT_UDP     17

typedef struct __attribute__((packed)) {
    uint32_t ver_tc_flow;
    uint16_t payload_len;
    uint8_t  next_header;
    uint8_t  hop_limit;
    uint8_t  src[16];
    uint8_t  dst[16];
} ipv6_hdr_t;

#define IPV6_HDR_LEN 40

typedef void (*ipv6_handler_t)(const uint8_t* src, const uint8_t* dst,
                                uint8_t next_header,
                                const uint8_t* data, uint32_t len);

void ipv6_init(void);
int  ipv6_send(const uint8_t* dst, uint8_t next_header,
               const uint8_t* data, uint32_t len);
int  ipv6_register_handler(uint8_t next_header, ipv6_handler_t handler);
void ipv6_set_addr(const uint8_t* addr);
int  ipv6_has_global_addr(void);
void ipv6_get_lladdr(uint8_t* addr_out);

/* Multicast group membership */
#define IPV6_MAX_MCAST_GROUPS 8
int  ipv6_mcast_join(const uint8_t* group_addr);
int  ipv6_mcast_leave(const uint8_t* group_addr);
int  ipv6_mcast_is_member(const uint8_t* group_addr);

/* Recompute E1000 MTA from all joined groups */
void ipv6_mcast_update_mta(void);

/* Send MLD reports for all joined groups */
void ipv6_mcast_report_all(void);

#endif

#ifndef ROUTE_H
#define ROUTE_H

#include "types.h"
#include "ip.h"

#define AF_INET  4
#define AF_INET6 6

#define ROUTE_TABLE_SIZE 8

typedef struct {
    int   used;
    int   af;
    union {
        ipv4_addr_t v4;
        uint8_t      v6[16];
    } dst;
    int   prefix_len;
    union {
        ipv4_addr_t v4;
        uint8_t      v6[16];
    } gw;
    int   metric;
} route_entry_t;

void route_init(void);
void route_clear(void);
int  route_lookup_v4(ipv4_addr_t dst, ipv4_addr_t* next_hop);
int  route_lookup_v6(const uint8_t* dst, uint8_t* next_hop);
int  route_add_v4(ipv4_addr_t dst, int prefix_len, ipv4_addr_t gateway);
int  route_add_v6(const uint8_t* dst, int prefix_len, const uint8_t* gateway);
int  route_del_v4(ipv4_addr_t dst, int prefix_len);
int  route_del_v6(const uint8_t* dst, int prefix_len);

#endif

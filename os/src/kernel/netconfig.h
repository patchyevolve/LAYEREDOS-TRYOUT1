#ifndef NETCONFIG_H
#define NETCONFIG_H

#include "types.h"
#include "ip.h"

#define NETCONFIG_SET_IPV4       1
#define NETCONFIG_ADD_ROUTE_V4   2
#define NETCONFIG_DEL_ROUTE_V4   3
#define NETCONFIG_SET_ARP        4
#define NETCONFIG_DEL_ARP        5
#define NETCONFIG_SET_IPV6       6
#define NETCONFIG_ADD_ROUTE_V6   7
#define NETCONFIG_DEL_ROUTE_V6   8
#define NETCONFIG_SET_NDP        9
#define NETCONFIG_DEL_NDP       10
#define NETCONFIG_GET_IPV4      11

typedef struct {
    int op;
    ipv4_addr_t addr4;
    int prefix_len;
    ipv4_addr_t gw4;
    uint8_t     mac[6];
    uint8_t     addr6[16];
    uint8_t     gw6[16];
} netconfig_req_t;

#endif

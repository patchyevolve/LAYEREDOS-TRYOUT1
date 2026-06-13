#ifndef NDP_H
#define NDP_H

#include "types.h"

#define NDP_CACHE_SIZE     16
#define IPV6_ADDR_LEN      16

/* ICMPv6 types for NDP */
#define ICMPV6_RS          133
#define ICMPV6_RA          134
#define ICMPV6_NS          135
#define ICMPV6_NA          136

/* NDP option types */
#define NDP_OPT_SRC_LLADDR 1
#define NDP_OPT_TGT_LLADDR 2
#define NDP_OPT_PREFIX     3

/* Solicited-node multicast prefix (FF02::1:FFxx:xxxx) */
#define SOLICITED_NODE_MC_PREFIX {0xFF, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, \
                                  0x00, 0x00, 0x00, 0x01, 0xFF, 0x00, 0x00, 0x00}

typedef struct {
    uint8_t  ipv6[IPV6_ADDR_LEN];
    uint8_t  mac[6];
    int      valid;
    uint64_t last_seen;
} ndp_cache_entry_t;

typedef struct __attribute__((packed)) {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint32_t reserved;
    uint8_t  target_addr[IPV6_ADDR_LEN];
} ndp_ns_t;

typedef struct __attribute__((packed)) {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint32_t flags;
    uint8_t  target_addr[IPV6_ADDR_LEN];
} ndp_na_t;

typedef struct __attribute__((packed)) {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint32_t reserved;
    uint8_t  options[];
} ndp_rs_t;

typedef struct __attribute__((packed)) {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint8_t  hop_limit;
    uint8_t  flags;
    uint16_t lifetime;
    uint32_t reachable_time;
    uint32_t retrans_timer;
    uint8_t  options[];
} ndp_ra_t;

typedef struct __attribute__((packed)) {
    uint8_t  type;
    uint8_t  len;
    uint8_t  data[];
} ndp_option_t;

typedef struct __attribute__((packed)) {
    uint8_t  type;
    uint8_t  len;
    uint8_t  prefix_len;
    uint8_t  flags;
    uint32_t valid_lifetime;
    uint32_t preferred_lifetime;
    uint32_t reserved;
    uint8_t  prefix[IPV6_ADDR_LEN];
} ndp_prefix_opt_t;

err_t ndp_init(void);
void  ndp_cache_update(const uint8_t* ipv6, const uint8_t* mac);
int   ndp_cache_lookup(const uint8_t* ipv6, uint8_t* mac);
int   ndp_resolve(const uint8_t* ipv6, uint8_t* mac, int timeout_ms);
void  ndp_make_lladdr(const uint8_t* mac, uint8_t* ipv6_out);
void  ndp_make_solicited_node(const uint8_t* ipv6, uint8_t* mc_out);

#endif

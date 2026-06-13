#ifndef IP_H
#define IP_H

#include "types.h"

typedef struct {
    uint8_t bytes[4];
} ipv4_addr_t;

typedef struct {
    uint8_t bytes[16];
} ipv6_addr_t;

static inline ipv4_addr_t ipv4_from_bytes(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    ipv4_addr_t addr = { .bytes = {a, b, c, d} };
    return addr;
}

static inline uint32_t ipv4_to_u32(ipv4_addr_t addr) {
    return (uint32_t)addr.bytes[0] | ((uint32_t)addr.bytes[1] << 8) |
           ((uint32_t)addr.bytes[2] << 16) | ((uint32_t)addr.bytes[3] << 24);
}

static inline ipv4_addr_t ipv4_from_u32(uint32_t host_u32) {
    ipv4_addr_t addr;
    addr.bytes[0] = host_u32 & 0xFF;
    addr.bytes[1] = (host_u32 >> 8) & 0xFF;
    addr.bytes[2] = (host_u32 >> 16) & 0xFF;
    addr.bytes[3] = (host_u32 >> 24) & 0xFF;
    return addr;
}

static inline int ipv4_addr_equal(ipv4_addr_t a, ipv4_addr_t b) {
    return a.bytes[0] == b.bytes[0] && a.bytes[1] == b.bytes[1] &&
           a.bytes[2] == b.bytes[2] && a.bytes[3] == b.bytes[3];
}

static inline int ipv6_addr_equal(const uint8_t a[16], const uint8_t b[16]) {
    for (int i = 0; i < 16; i++)
        if (a[i] != b[i]) return 0;
    return 1;
}

#endif

#ifndef UDP_H
#define UDP_H

#include "types.h"
#include "ip.h"

#define UDP_HDR_LEN 8
#define UDP_MAX_ENDPOINTS 16
#define UDP_DGRAM_QUEUE_SIZE 16
#define UDP_DGRAM_MAX_SIZE 1500

typedef struct __attribute__((packed)) {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint16_t checksum;
} udp_hdr_t;

typedef struct {
    uint8_t  data[UDP_DGRAM_MAX_SIZE];
    uint32_t len;
    int      af;
    uint8_t  src_addr[16];
    uint16_t src_port;
} udp_dgram_t;

typedef struct {
    int       used;
    int       af;
    uint16_t  port;
    uint8_t   addr[16];

    udp_dgram_t queue[UDP_DGRAM_QUEUE_SIZE];
    int         q_head;
    int         q_tail;
    int         q_count;

    /* Receive and send timeouts */
    int recv_timeout;
    int send_timeout;
    int ipv6only;
} udp_endpoint_t;

void  udp_init(void);
int   udp_sendto(int af, const void* dst_ip, uint16_t dst_port,
                 uint16_t src_port,
                 const uint8_t* data, uint32_t len);

/* Endpoint-based API for sockets */
int   udp_bind_endpoint(int af, const uint8_t* addr, uint16_t port,
                         int recv_timeout, int send_timeout, int ipv6only);
void  udp_unbind_endpoint(udp_endpoint_t* ep);
udp_endpoint_t* udp_find_endpoint(int af, uint16_t port);
void  udp_endpoint_enqueue(udp_endpoint_t* ep, int af, const void* src_ip,
                           uint16_t src_port,
                           const uint8_t* data, uint32_t len);
int   udp_endpoint_dequeue(udp_endpoint_t* ep, uint8_t* buf, uint32_t size,
                           int* out_af, void* out_src_addr,
                           uint16_t* out_src_port, int timeout_ms);

#endif

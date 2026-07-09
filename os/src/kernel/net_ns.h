#ifndef NET_NS_H
#define NET_NS_H

#include "types.h"
#include "ip.h"
#include "sync.h"
#include "route.h"
#include "arp.h"
#include "ndp.h"
#include "tcp.h"
#include "udp.h"
#include "ipv4.h"
#include "ipv6.h"
#include "igmp.h"
#include "net.h"

/* Forward declaration from sched.h */
struct thread;

/* ---- Network namespace (per-namespace network state) ---- */

typedef struct net_ns {
    /* Routing */
    int route_initialized;
    route_entry_t route_table[ROUTE_TABLE_SIZE];
    spinlock_t route_lock;

    /* ARP cache */
    struct {
        ipv4_addr_t ip;
        uint8_t     mac[6];
        int         valid;
        uint64_t    last_used;
    } arp_cache[ARP_CACHE_SIZE];
    int arp_initialized;
    spinlock_t arp_lock;

    /* NDP cache */
    ndp_cache_entry_t ndp_cache[NDP_CACHE_SIZE];
    int ndp_initialized;
    spinlock_t ndp_lock;

    /* IPv4 */
    ipv4_addr_t our_ipv4;
    int ipv4_prefix_len;
    int ipv4_initialized;
    uint16_t ipv4_next_id;
    struct {
        uint8_t        protocol;
        ipv4_handler_t handler;
    } ipv4_dispatch[IPV4_DISPATCH_SLOTS];

    /* IPv6 */
    int ipv6_initialized;
    struct {
        uint8_t        next_header;
        ipv6_handler_t handler;
    } ipv6_dispatch[IPV6_DISPATCH_SLOTS];
    uint8_t global_ipv6[16];
    uint8_t link_local[16];
    struct {
        uint8_t addr[16];
        int     used;
    } ipv6_mcast_groups[IPV6_MAX_MCAST_GROUPS];

    /* IGMP */
    int igmp_initialized;
    uint32_t igmp_groups[IGMP_MAX_GROUPS];
    int      igmp_group_used[IGMP_MAX_GROUPS];
    spinlock_t igmp_lock;

    /* TCP */
    int tcp_initialized;
    spinlock_t tcp_lock;
    uint16_t tcp_ephemeral_port;
    tcp_conn_t* tcp_conns;

    /* UDP */
    int udp_initialized;
    spinlock_t udp_lock;
    udp_endpoint_t* udp_endpoints;

    /* Sockets */
    int net_initialized;
    spinlock_t sockets_lock;
    struct {
        int        used;
        socket_t*  sock;
    } net_sockets[NET_MAX_SOCKETS];

    /* ICMPv6 */
    int icmpv6_initialized;
    uint16_t icmpv6_next_id;

    /* ICMPv6 RA callback */
    void (*icmpv6_ra_callback)(const uint8_t* src, const uint8_t* data, uint32_t len);

    /* Reference counting */
    int refcount;

    /* Namespace name (diagnostic) */
    char name[16];
} net_ns_t;

/* The initial (default) network namespace */
extern net_ns_t init_net_ns;

/* Get the current thread's network namespace.
 * Returns init_net_ns for kernel threads without a process context or
 * during early boot before scheduling is active. */
net_ns_t* get_current_ns(void);

/* Allocate a new network namespace (zeroed, refcount=1).
 * Returns NULL on OOM. */
net_ns_t* net_ns_alloc(void);

/* Increment refcount */
void net_ns_retain(net_ns_t* ns);

/* Decrement refcount; free when zero */
void net_ns_release(net_ns_t* ns);

/* Initialize net_ns subsystem (sets up init_net_ns) */
void net_ns_init(void);

/* Copy namespace state from src to dst (used by unshare) */
void net_ns_copy(net_ns_t* dst, const net_ns_t* src);

#endif

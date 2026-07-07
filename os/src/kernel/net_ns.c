#include "kernel.h"
#include "net_ns.h"
#include "sched.h"
#include "process.h"
#include "kmalloc.h"

/* The default network namespace (used by all processes initially) */
net_ns_t init_net_ns;

static int net_ns_initialized = 0;

net_ns_t* get_current_ns(void) {
    return &init_net_ns;
}

static err_t alloc_arrays(net_ns_t* ns) {
    ns->tcp_conns = (tcp_conn_t*)kmalloc(sizeof(tcp_conn_t) * TCP_MAX_CONN);
    if (!ns->tcp_conns) return ERR_NOMEM;
    kmemset(ns->tcp_conns, 0, sizeof(tcp_conn_t) * TCP_MAX_CONN);

    ns->udp_endpoints = (udp_endpoint_t*)kmalloc(sizeof(udp_endpoint_t) * UDP_MAX_ENDPOINTS);
    if (!ns->udp_endpoints) {
        kfree(ns->tcp_conns);
        ns->tcp_conns = NULL;
        return ERR_NOMEM;
    }
    kmemset(ns->udp_endpoints, 0, sizeof(udp_endpoint_t) * UDP_MAX_ENDPOINTS);
    return ERR_OK;
}

static void free_arrays(net_ns_t* ns) {
    if (ns->tcp_conns) { kfree(ns->tcp_conns); ns->tcp_conns = NULL; }
    if (ns->udp_endpoints) { kfree(ns->udp_endpoints); ns->udp_endpoints = NULL; }
}

net_ns_t* net_ns_alloc(void) {
    net_ns_t* ns = (net_ns_t*)kmalloc(sizeof(net_ns_t));
    if (!ns) return NULL;
    kmemset(ns, 0, sizeof(net_ns_t));
    if (alloc_arrays(ns) != ERR_OK) { kfree(ns); return NULL; }
    ns->refcount = 1;
    ns->tcp_ephemeral_port = 32768;
    return ns;
}

void net_ns_retain(net_ns_t* ns) {
    if (ns) __sync_fetch_and_add(&ns->refcount, 1);
}

void net_ns_release(net_ns_t* ns) {
    if (!ns || ns == &init_net_ns) return;
    int old = __sync_fetch_and_sub(&ns->refcount, 1);
    if (old <= 1) {
        free_arrays(ns);
        kfree(ns);
    }
}

void net_ns_copy(net_ns_t* dst, const net_ns_t* src) {
    kmemcpy(dst, src, sizeof(net_ns_t));
    dst->tcp_conns = NULL;
    dst->udp_endpoints = NULL;
    dst->refcount = 1;
    dst->route_initialized = 0;
    dst->arp_initialized = 0;
    dst->ndp_initialized = 0;
    dst->ipv4_initialized = 0;
    dst->ipv6_initialized = 0;
    dst->igmp_initialized = 0;
    dst->tcp_initialized = 0;
    dst->udp_initialized = 0;
    dst->net_initialized = 0;
    dst->icmpv6_initialized = 0;
    alloc_arrays(dst);
    if (src->tcp_conns) kmemcpy(dst->tcp_conns, src->tcp_conns, sizeof(tcp_conn_t) * TCP_MAX_CONN);
    if (src->udp_endpoints) kmemcpy(dst->udp_endpoints, src->udp_endpoints, sizeof(udp_endpoint_t) * UDP_MAX_ENDPOINTS);
}

/* Global arrays for init_net_ns (must persist for system lifetime) */
static tcp_conn_t init_tcp_conns[TCP_MAX_CONN];
static udp_endpoint_t init_udp_endpoints[UDP_MAX_ENDPOINTS];

void net_ns_init(void) {
    kmemset(&init_net_ns, 0, sizeof(init_net_ns));
    kmemset(init_tcp_conns, 0, sizeof(init_tcp_conns));
    kmemset(init_udp_endpoints, 0, sizeof(init_udp_endpoints));
    init_net_ns.tcp_conns = init_tcp_conns;
    init_net_ns.udp_endpoints = init_udp_endpoints;
    init_net_ns.refcount = 1;
    init_net_ns.tcp_ephemeral_port = 49152;
    init_net_ns.icmpv6_next_id = 1;
    kmemcpy(init_net_ns.name, "init", 5);
    net_ns_initialized = 1;
}

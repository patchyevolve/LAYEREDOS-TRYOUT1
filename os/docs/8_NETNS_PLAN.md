# OPERtur Network Namespaces Implementation Plan

**Version:** 1.0
**Target:** Per-process network stack isolation via `CLONE_NEWNET`
**Architecture:** Kernel-level — add `net_ns` struct containing per-namespace copies of network state

---

## Overview

Network namespaces isolate network state per process (or group of processes). Each namespace has its own:

- IP addresses (IPv4 + IPv6)
- Routing table
- ARP/NDP cache
- TCP connection table
- UDP endpoints
- Multicast group memberships (IGMP/MLD)
- Socket table

A default `init_net_ns` exists at boot. Processes inherit their parent's namespace via shared pointer. `unshare(CLONE_NEWNET)` creates a new namespace with empty tables. Virtual Ethernet (veth) pairs connect namespaces.

---

## Task TN.1 — Define `net_ns_t` Structure

**What:** Define the namespace structure that holds all per-instance network state.

```c
typedef struct net_ns {
    int refcount;

    // IP addresses
    ipv4_addr_t ipv4_addr;         // OUR_IPV4
    uint8_t     ipv6_global[16];   // GLOBAL_IPV6
    int         ipv6_has_global;   // whether global addr is set

    // Routing
    route_entry_t route_table[ROUTE_TABLE_SIZE];

    // ARP cache
    arp_entry_t arp_cache[ARP_CACHE_SIZE];
    spinlock_t  arp_lock;

    // NDP cache
    ndp_entry_t ndp_cache[NDP_CACHE_SIZE];  // Future

    // TCP
    tcp_conn_t tcp_conns[TCP_MAX_CONN];
    uint16_t   tcp_ephemeral_port;
    spinlock_t tcp_lock;

    // UDP
    udp_endpoint_t udp_endpoints[UDP_MAX_ENDPOINTS];
    spinlock_t udp_lock;

    // Sockets
    struct { int used; socket_t* sock; } sockets[NET_MAX_SOCKETS];
    int socket_count;

    // Multicast groups
    uint32_t igmp_groups[IGMP_MAX_GROUPS];
    int      igmp_group_used[IGMP_MAX_GROUPS];
    uint8_t  ipv6_mcast_groups[IPV6_MAX_MCAST_GROUPS][16];
    int      ipv6_mcast_used[IPV6_MAX_MCAST_GROUPS];

    // Parent namespace link
    struct net_ns* parent;  // for nested/veth routing

    char name[32];
} net_ns_t;
```

**Files:** `src/kernel/net_ns.h`

**Depends on:** Nothing (struct definition only)

---

## Task TN.2 — Add `net_ns*` to Process

**What:** Add `net_ns*` field to `process_t`, initialize at boot.

**Changes:**
- `process.h`: `net_ns_t* net_ns;` field
- `process.c` in `process_create()`: inherit parent's `net_ns` (increment refcount)
- `main.c` `init_net_ns` created at boot, init process gets it

**Default init:**
```c
net_ns_t* init_net_ns;
init_net_ns = kmalloc(sizeof(net_ns_t));
init_net_ns->refcount = 1;
// Initialize empty tables (kmemset zero), then populate with boot defaults:
// - route_init() adds default routes
// - DHCP/SLAAC configure IP
// - TCP/UDP inits clear tables
```

**Files:** `src/include/process.h`, `src/kernel/process.c`, `src/kernel/main.c`

**Depends on:** TN.1 (struct definition)

---

## Task TN.3 — Refactor Global Tables into net_ns

**What:** Move all global network tables from their current files into `net_ns_t`.

**Changes by file:**

| File | Current global | New accessor |
|------|---------------|--------------|
| `route.c` | `static route_entry_t route_table[8]` | `ns->route_table` |
| `arp.c` | `static arp_cache[16]`, `spinlock_t arp_lock` | `ns->arp_cache`, `ns->arp_lock` |
| `tcp.c` | `static tcp_conn_t tcp_conns[16]`, `tcp_ephemeral_port`, `tcp_lock` | `ns->tcp_conns`, `ns->tcp_ephemeral_port`, `ns->tcp_lock` |
| `udp.c` | `static udp_endpoint_t udp_endpoints[16]`, `udp_lock` | `ns->udp_endpoints`, `ns->udp_lock` |
| `ipv4.c` | `static ipv4_addr_t OUR_IPV4`, `igmp_groups` | `ns->ipv4_addr`, `ns->igmp_groups` |
| `ipv6.c` | `static GLOBAL_IPV6`, `ipv6_mcast_groups` | `ns->ipv6_global`, `ns->ipv6_mcast_groups` |
| `net.c` | `static net_sockets[32]` | `ns->sockets` |

**Design pattern:** Each function that uses a table takes an explicit `net_ns_t*` parameter or fetches it from `current_thread->proc->net_ns`. During the refactor, provide both:
- `route_lookup_v4(ns, dst, next_hop)` — new namespace-aware version
- Keep a compatibility wrapper `route_lookup_v4(dst, next_hop)` that uses `current->proc->net_ns`

**Lock-safe access:** All `ns->*_lock` spinlocks must be used before accessing namespace-scoped tables.

**Files:** `route.c/h`, `arp.c/h`, `tcp.c/h`, `udp.c/h`, `ipv4.c/h`, `ipv6.c/h`, `net.c/h`, `ndp.c/h`, `icmpv6.c/h`, `igmp.c/h`

**Depends on:** TN.1 (struct), TN.2 (process binding)

---

## Task TN.4 — Inbound Packet Dispatch

**What:** Modify inbound Ethernet packet dispatch to check namespace membership.

**Current flow:**
```
eth_rx_poll() → eth_dispatch() → ipv4_eth_handler() / ipv6_eth_handler()
    → tcp_handle_common() / udp_handle_common() / icmpv6_input()
```

**New flow:**
```
eth_rx_poll() → eth_dispatch() (unchanged)

ipv4_eth_handler() (in init_net_ns or all namespaces?):
    Option A: One global dispatch → distribute to matching namespaces
    Option B: Each namespace registers its own handler

Design choice: Option A — single dispatch delivers to ALL namespaces.
The destination IP check now becomes: does ANY namespace own this IP?
```

**Simpler approach:** Keep a single set of per-namespace dispatch tables. When a packet arrives:

1. Parse IP header
2. Determine destination address
3. Iterate all active namespaces
4. If namespace owns the destination IP or has a listener on the port, deliver to that namespace's table

**Optimization:** Maintain a global hash table of `(port, protocol) → namespace` for fast dispatch.

**Files:** `ipv4.c`, `ipv6.c`, `tcp.c`, `udp.c`

**Depends on:** TN.3 (refactored tables)

---

## Task TN.5 — `unshare(CLONE_NEWNET)` Syscall

**What:** Create a new network namespace for the calling process.

**Sub-operations:**
- `sys_unshare(flags)` — if `flags & CLONE_NEWNET`, create new `net_ns_t`
- New namespace: empty tables (no IP, no routes, no connections)
- Process moves from parent's namespace to new one
- Old namespace refcount decremented (freed if zero)

**Additional syscalls:**
- `sys_setns(fd, nstype)` — join an existing namespace via fd (future)
- `CLONE_NEWNET` flag (value: 0x40000000) — for `clone()` and `unshare()`

**Files:** `src/kernel/syscall.c`, `src/include/syscall_defs.h`, `src/kernel/net_ns.c`

**Depends on:** TN.3 (refactored tables), TN.2 (process binding)

---

## Task TN.6 — Virtual Ethernet (veth) Pair

**What:** Create virtual Ethernet interfaces that bridge namespaces.

**Interface:**
```c
// Create a veth pair linking two namespaces
int veth_create(net_ns_t* ns_a, net_ns_t* ns_b,
                uint8_t mac_a[6], uint8_t mac_b[6]);

// Transmit on veth: packet enters the peer namespace
int veth_tx(veth_t* veth, const uint8_t* frame, uint32_t len);
```

**Design:**
- Each veth endpoint has a MAC address and a peer reference
- `veth_tx` on endpoint A delivers frame to endpoint B's receive queue
- B's namespace processes it through normal IP dispatch
- Can assign IP addresses to veth endpoints (via DHCP or static config)
- Allows cross-namespace communication without physical NIC

**Files:** `src/kernel/veth.h`, `src/kernel/veth.c`

**Depends on:** TN.4 (packet dispatch), TN.5 (namespace syscalls)

---

## Task TN.7 — Namespace-Aware Socket Operations

**What:** Ensure socket operations respect namespace boundaries.

**Key changes:**
- `socket_alloc()` binds to `current->proc->net_ns`
- `sock_register()` stores socket in `ns->sockets[]`
- `sock_lookup(fd)` searches `current->proc->net_ns->sockets[]`
- `tcp_find_conn()` only searches `current->proc->net_ns->tcp_conns[]`
- `tcp_conn_bind()` port conflict check only within namespace
- `udp_bind_endpoint()` port conflict check only within namespace
- `tcp_find_listener()` only searches within namespace

**Cross-namespace operations:**
- Processes in different namespaces cannot connect to each other's sockets
- Veth pairs are the bridge for cross-namespace communication
- Default namespace processes see all sockets (backward compatibility)

**Files:** `net.c`, `tcp.c`, `udp.c`

**Depends on:** TN.3 (refactored tables), TN.4 (packet dispatch)

---

## Task TN.8 — Namespace Cleanup on Exit

**What:** When a process exits, decrement namespace refcount. Free namespace when last user exits.

**Changes:**
- `process_exit()`: `net_ns_release(proc->net_ns)` which decrements refcount
- `net_ns_free(ns)`: frees all connections, unbinds all endpoints, closes all sockets
- On refcount == 0: call cleanup functions, then `kfree(ns)`

**Cleanup actions:**
- `tcp_conn_destroy()` for all active connections (sends RST if established)
- `udp_unbind_endpoint()` for all endpoints
- `sock_unregister()` for all sockets
- Free ARP cache entries

**Files:** `src/kernel/net_ns.c`, `src/kernel/process.c`

**Depends on:** TN.2 (process binding), TN.3 (refactored tables)

---

## Task TN.9 — Default Namespace Migration

**What:** Move boot-time setup from global initialization to `init_net_ns`.

**Changes:**
- `route_init()` → `net_ns_route_init(init_net_ns)` — populates default routes in namespace
- `tcp_init()` → `net_ns_tcp_init(init_net_ns)` — clears TCP table in namespace
- `udp_init()` → `net_ns_udp_init(init_net_ns)` — clears UDP table
- DHCP/SLAAC/NTP configure addresses in `init_net_ns`
- All services (DHCP, NTP, DNS, echo servers) operate within `init_net_ns`

**Files:** `main.c`, `route.c`, `tcp.c`, `udp.c`, `dhcp.c`, `slaac.c`, `ntp.c`, `dns.c`

**Depends on:** TN.3 (refactored tables)

---

## Task TN.10 — Test

**What:** Verify namespace isolation.

**Tests:**
1. Default namespace works (existing tests pass)
2. `unshare(CLONE_NEWNET)` within a process → new namespace has no IP
3. Two processes in different namespaces cannot connect via TCP to same port
4. Veth pair connects two namespaces, TCP works across veth
5. Namespace freed when last process exits

**Files:** `os/src/kernel/ns_test.c` (new)

**Depends on:** TN.5 (unshare), TN.6 (veth)

---

## Summary Table

| Task | Description | Files | Est. lines | Difficulty |
|------|-------------|-------|-----------|------------|
| TN.1 | net_ns_t struct | net_ns.h | 30 | Easy |
| TN.2 | Process binding | process.h/c, main.c | 50 | Easy |
| TN.3 | Refactor tables | route/arp/tcp/udp/ipv4/ipv6/net/ndp/icmpv6/igmp | ~500 | Hard |
| TN.4 | Inbound dispatch | ipv4/ipv6/tcp/udp | ~200 | Medium |
| TN.5 | unshare + setns | syscall.c, syscall_defs.h, net_ns.c | ~150 | Medium |
| TN.6 | Veth pair | veth.h/c | ~200 | Medium |
| TN.7 | Namespace-aware sockets | net.c, tcp.c, udp.c | ~150 | Hard |
| TN.8 | Cleanup on exit | net_ns.c, process.c | ~100 | Medium |
| TN.9 | Default ns migration | main.c, all init functions | ~150 | Medium |
| TN.10 | Test | ns_test.c | ~100 | Easy |
| **Total** | | **~25 files modified** | **~1630** | |

## Namespace Lifecycle

```
boot ──→ init_net_ns (refcount=1)
            │
            ├── init (PID 1) shares init_net_ns
            ├── fork() → child inherits parent's ns (refcount++)
            ├── unshare(CLONE_NEWNET) → new empty ns (refcount=1)
            │       └── parent ns refcount--
            └── process_exit() → refcount--
                └── refcount == 0 → free all tables + kfree(ns)
```

## Risks and Mitigations

| Risk | Impact | Mitigation |
|------|--------|------------|
| Global dispatch loop iterates all namespaces on every packet | Latency from O(n) namespace check | Keep namespace count low (<10), add hash table for active ports |
| `tcp_lock` migration to per-ns lock introduces deadlock | System hang | All lock acquisitions follow same order: `ns->tcp_lock` before `ns->arp_lock` |
| Backward compatibility broken | All existing tests fail | Keep `current_ns()` helper that returns init_net_ns when no process context exists (boot) |
| Veth TX path re-enters IP dispatch | Stack overflow | Veth TX enqueues to peer's receive queue; peer processes in its own context or thread_sleep polling loop |

## Syscall Numbers

- `SYS_UNSHARE` (68) — `unshare(flags)` where `flags & CLONE_NEWNET` creates new namespace
- `SYS_SETNS` (69) — `setns(fd, nstype)` joins an existing namespace
- `SYSCALL_COUNT` = 70

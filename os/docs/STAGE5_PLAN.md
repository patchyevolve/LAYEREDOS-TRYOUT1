# Stage 5 — Networking Implementation Plan

## Target Environment

QEMU `-net nic -net user` (default) provides:

| Resource | IPv4 | IPv6 |
|----------|------|------|
| **NIC** | Intel 82540EM (PCI 00:03.0, 0x8086:0x100E, IRQ 11) | same |
| **Guest IP** | 10.0.2.15 (via DHCP) | fe80::/10 link-local + SLAAC global |
| **Gateway** | 10.0.2.2 | fe80::2 (QEMU SLiRP) |
| **DNS** | 10.0.2.3 (forwarded) | same |
| **DHCP server** | 10.0.2.4 | N/A (SLAAC) |

## Data Flow

```
Userspace app → socket(AF_INET|AF_INET6)/send()/recv()
         ↕
Socket API (kernel syscalls 38–46, dual-stack)
         ↕
TCP/UDP layer (port mux, stream/datagram, v4/v6 agnostic)
         ↕
IP layer ──→ IPv4 (TTL, checksum, frag)
         └─→ IPv6 (hop-limit, flow label, extension headers)
         ↕
Address Resolution ──→ ARP (IPv4 → MAC)
                 └─→ NDP (IPv6 → MAC, ICMPv6 NS/NA/RS/RA)
         ↕
Ethernet layer (frame encode/decode, EtherType 0x0800/0x86DD/0x0806)
         ↕
NIC driver (E1000: descriptor rings, MMIO, IRQ)
         ↕
PCI device (00:03.0, Intel 82540EM)
```

## Layer Discipline

Each layer calls only the layer directly below it:
- Socket API → TCP/UDP
- TCP/UDP → IP layer (v4 or v6 based on socket family)
- IP layer → Address Resolution (ARP for v4, NDP for v6) then Ethernet
- Ethernet → NIC driver

All layers polled from NIC RX path: IRQ → ethernet RX → dispatch by EtherType (0x0800=IPv4, 0x86DD=IPv6, 0x0806=ARP) → further dispatch.

---

## Phases

### Phase 1: NIC Driver (E1000)

**Files:** `os/src/kernel/e1000.c`, `os/src/kernel/e1000.h`, `os/src/kernel/nic.h`

**NIC abstraction (`nic.h`):**
```c
typedef struct nic {
    uint8_t  mac[6];
    int      irq;
    int      present;
    err_t  (*send)(const struct nic* nic, const uint8_t* frame, uint32_t len);
    int    (*poll)(const struct nic* nic, uint8_t* buf, uint32_t max_len);
} nic_t;

extern nic_t nic;
err_t nic_init(void);
void  nic_handle_irq(void);
```

**Implementation:**
- PCI detect: class 0x02, subclass 0x00, match 0x8086:0x100E (also support 0x8086:0x100F, 0x8086:0x10D3)
- Enable bus mastering, map BAR0 (MMIO at kernel virtual address)
- Reset via CTRL.RST, wait for self-test complete (STATUS)
- Read MAC from RAL0/RAH0 registers
- RCTL: buffer size=2048, strip CRC, enable broadcast/multicast
- TCTL: enable, pad short packets to 64 bytes
- **RX ring**: 32 descriptors × 16 bytes, 2 KB bounce buffer each, physically contiguous pages
- **TX ring**: 32 descriptors × 16 bytes, 2 KB bounce buffer each
- Pointers: set RDBAL/RDBAH/RDLEN, TDBAL/TDBAH/TDLEN, zero RDH/ RDT, TDH/TDT
- Enable RCTL.EN + TCTL.EN
- `send()`: get next TX descriptor index, memcpy frame to bounce buffer, set length+cmd(EOP|IFCS|RS), advance TDT, poll DD bit with timeout
- `poll()`: check RX descriptor DD bit, copy frame data, advance RDT

**Verification:**
| # | Test | Method | Expected output |
|---|------|--------|-----------------|
| 1.1 | PCI detection | `nic_init()` | `[NIC] Found Intel 82540EM at 00:03.0` |
| 1.2 | MAC address | Print from RAL/RAH | `[NIC] MAC = 52:54:00:12:34:56` |
| 1.3 | Ring init | Print config | `[NIC] RX=32 desc, TX=32 desc` |
| 1.4 | Send broadcast | `nic.send(broadcast, 60)` | `[NIC] TX OK (slot N)` |
| 1.5 | RX poll | Transmit then poll | `[NIC] RX frame len=60` |

---

### Phase 2: Ethernet Layer

**Files:** `os/src/kernel/eth.c`, `os/src/kernel/eth.h`

```c
#define ETH_ALEN 6
#define ETHERTYPE_ARP   0x0806
#define ETHERTYPE_IPV4  0x0800
#define ETHERTYPE_IPV6  0x86DD

typedef struct __attribute__((packed)) {
    uint8_t  dst[ETH_ALEN];
    uint8_t  src[ETH_ALEN];
    uint16_t type;       // big-endian EtherType
    uint8_t  payload[];
} eth_hdr_t;

typedef void (*eth_handler_t)(const uint8_t* src_mac, uint16_t type,
                               const uint8_t* data, uint32_t len);

err_t eth_send(const uint8_t* dst_mac, uint16_t type,
               const uint8_t* data, uint32_t len);
void  eth_rx_poll(void);
void  eth_register(uint16_t type, eth_handler_t handler);
```

- `eth_send`: build frame (dst=arg, src=nic.mac, type=network order), call nic.send()
- `eth_rx_poll`: loop nic.poll(), parse header, dispatch by type to registered handlers
- Registered handlers: ARP (0x0806), IPv4 (0x0800), IPv6 (0x86DD)

**Verification:**
| # | Test | Method | Expected |
|---|------|--------|----------|
| 2.1 | Send frame | `eth_send(broadcast, 0x9000, test, 64)` | Frame transmitted |
| 2.2 | RX dispatch | Inject test frame via handler | Correct handler called |

---

### Phase 3: Address Resolution (ARP + NDP)

**Files:** `os/src/kernel/arp.c`, `os/src/kernel/arp.h`, `os/src/kernel/ndp.c`, `os/src/kernel/ndp.h`

**3a. ARP (IPv4 → MAC)**

```c
#define ARP_OP_REQUEST 1
#define ARP_OP_REPLY   2

typedef struct __attribute__((packed)) {
    uint16_t htype;         // 1 = Ethernet
    uint16_t ptype;         // 0x0800
    uint8_t  hlen;          // 6
    uint8_t  plen;          // 4
    uint16_t oper;
    uint8_t  sha[6];        // sender MAC
    uint8_t  spa[4];        // sender IP
    uint8_t  tha[6];        // target MAC
    uint8_t  tpa[4];        // target IP
} arp_pkt_t;

void arp_init(void);
int  arp_resolve(uint32_t ip, uint8_t* mac, int timeout_ms);
void arp_handle(const uint8_t* src_mac, const uint8_t* data, uint32_t len);
void arp_set(uint32_t ip, const uint8_t* mac);
```

- Register with Ethernet for EtherType 0x0806
- Cache: 16 entries, LRU eviction, each with (ip, mac, valid)
- On ARP request for our IP: update cache, send reply
- On ARP reply: update cache
- `arp_resolve`: cache hit → instant copy; cache miss → send request, wait 1s, retry up to 3×, timeout with ERR_TIMEOUT

**3b. NDP (IPv6 → MAC)**

IPv6 Neighbor Discovery replaces ARP using ICMPv6 messages:
- **Neighbor Solicitation** (ICMPv6 type 135) — like ARP request
- **Neighbor Advertisement** (ICMPv6 type 136) — like ARP reply
- **Router Solicitation** (ICMPv6 type 133) — request RA for SLAAC
- **Router Advertisement** (ICMPv6 type 134) — prefix/lifetime for auto-config

```c
typedef struct {
    uint8_t  mac[6];
    int      valid;
    uint64_t last_seen;    // tick for GC
} ndp_cache_entry_t;

#define NDP_CACHE_SIZE 16

void ndp_init(void);
int  ndp_resolve(const uint8_t* ipv6, uint8_t* mac, int timeout_ms);
void ndp_handle_na(const uint8_t* src_ipv6, const uint8_t* target_ipv6,
                   const uint8_t* mac);
void ndp_handle_ns(const uint8_t* src_ipv6, const uint8_t* target_ipv6);
void ndp_handle_ra(const uint8_t* src_ipv6, const uint8_t* mac,
                   const uint8_t* prefix, int prefix_len, uint32_t lifetime);
```

- Register with ICMPv6 for types 135 (NS), 136 (NA), 134 (RA)
- NS received for our address → send NA with our MAC
- NA received → update neighbor cache
- RA received → store prefix for SLAAC (link-local always configured)
- Solicited-node multicast address for NS target

**Verification:**
| # | Test | Method | Expected |
|---|------|--------|----------|
| 3.1 | ARP resolve gateway | `arp_resolve(10.0.2.2, mac, 3000)` | MAC in 3s |
| 3.2 | ARP cache hit | 2nd resolve same IP | Instant |
| 3.3 | ARP self | `arp_resolve(10.0.2.15, ...)` | Our own MAC |
| 3.4 | ARP timeout | `arp_resolve(10.0.2.99, ..., 1000)` | ERR_TIMEOUT |
| 3.5 | NDP resolve gateway | `ndp_resolve(fe80::2, mac, 3000)` | MAC in 3s |
| 3.6 | NDP self | `ndp_resolve(our_linklocal, ...)` | Our own MAC |

---

### Phase 4: IP Layer (IPv4 + IPv6)

**Files:** `os/src/kernel/ipv4.c`, `os/src/kernel/ipv4.h`, `os/src/kernel/ipv6.c`, `os/src/kernel/ipv6.h`, `os/src/kernel/route.c`, `os/src/kernel/route.h`

**4a. IPv4**

```c
typedef struct __attribute__((packed)) {
    uint8_t  ver_ihl;       // 4 | ihl
    uint8_t  dscp_ecn;
    uint16_t total_len;
    uint16_t id;
    uint16_t flags_frag;
    uint8_t  ttl;
    uint8_t  protocol;      // 1=ICMP, 6=TCP, 17=UDP
    uint16_t checksum;
    uint32_t src;
    uint32_t dst;
} ipv4_hdr_t;

void ipv4_init(void);
int  ipv4_send(uint32_t dst, uint8_t protocol,
               const uint8_t* data, uint32_t len);
void ipv4_handle(const uint8_t* data, uint32_t len);
```

- Register with Ethernet for EtherType 0x0800 (via a tiny dispatcher in net.c)
- `ipv4_handle`: verify checksum, check dst is our IP or broadcast, dispatch protocol handler
- `ipv4_send`: build header, calculate checksum, call `arp_resolve` for next-hop MAC, then `eth_send`
- Fragment: not yet (error on > MTU for now)

**4b. IPv6**

```c
typedef struct __attribute__((packed)) {
    uint32_t ver_tc_flow;   // version(4) | traffic class(8) | flow label(20)
    uint16_t payload_len;
    uint8_t  next_header;   // 58=ICMPv6, 6=TCP, 17=UDP
    uint8_t  hop_limit;
    uint8_t  src[16];
    uint8_t  dst[16];
} ipv6_hdr_t;

void ipv6_init(void);
int  ipv6_send(const uint8_t* dst, uint8_t next_header,
               const uint8_t* data, uint32_t len);
void ipv6_handle(const uint8_t* data, uint32_t len);
```

- Register with Ethernet for EtherType 0x86DD
- No checksum in IPv6 header (unlike IPv4)
- `ipv6_handle`: verify version=6, check dst is our address or multicast, follow next_header chain (skip hop-by-hop, routing headers), dispatch to next_header handler
- `ipv6_send`: build header, resolve next-hop MAC via NDP (`ndp_resolve`), then `eth_send`
- Our address: link-local (derived from MAC via EUI-64) + any SLAAC global addresses

**4c. Routing table**

```c
typedef struct {
    int   used;
    int   af;              // AF_INET or AF_INET6
    uint8_t  dst[16];      // network prefix (zero = default)
    int   prefix_len;      // CIDR prefix length
    uint8_t  gateway[16];  // next-hop (zero = directly connected)
    int   metric;
} route_entry_t;

#define ROUTE_TABLE_SIZE 8

void route_init(void);
int  route_lookup(int af, const void* dst, void* next_hop);
int  route_add(int af, const void* dst, int prefix_len,
               const void* gateway);
```

- Initialize with defaults: IPv4 default → 10.0.2.2, IPv6 default → fe80::2, directly connected 10.0.2.0/24 and fe80::/10
- `route_lookup`: longest-prefix match, return next-hop IP

**Static config (until DHCP/SLAAC):**
```c
// IPv4 (hardcoded)
#define IPV4_ADDR      0x0A00020F  // 10.0.2.15
#define IPV4_GATEWAY   0x0A000202  // 10.0.2.2
#define IPV4_NETMASK   0xFFFFFF00

// IPv6 link-local derived from MAC via EUI-64
// fe80:: + ff:fe + mac[0..2] + mac[3..5]
#define IPV6_LL_PREFIX 0xFE80000000000000ULL
```

**Verification:**
| # | Test | Method | Expected |
|---|------|--------|----------|
| 4.1 | IPv4 checksum | Known buffer | Matches reference |
| 4.2 | IPv4 send | `ipv4_send(10.0.2.2, 1, test, 4)` | ARP + eth_send succeeds |
| 4.3 | IPv6 link-local | Print derived address | `[IPv6] LL = fe80::5054:ff:fe12:3456` |
| 4.4 | Route lookup | `route_lookup(AF_INET, 10.0.2.2, &gw)` | Returns 10.0.2.2 (direct) |
| 4.5 | Route default | `route_lookup(AF_INET, 8.8.8.8, &gw)` | Returns 10.0.2.2 |

---

### Phase 5: ICMP (ICMPv4 + ICMPv6)

**Files:** `os/src/kernel/icmp.c`, `os/src/kernel/icmp.h`, `os/src/kernel/icmpv6.c`, `os/src/kernel/icmpv6.h`

**5a. ICMPv4**

```c
#define ICMP_ECHO_REPLY   0
#define ICMP_ECHO_REQUEST 8

void icmpv4_init(void);
int  icmpv4_ping(uint32_t dst, int timeout_ms);
void icmpv4_handle(uint32_t src, const uint8_t* data, uint32_t len);
```

- Register with IPv4 for protocol 1
- `icmpv4_handle`: echo request → swap src/dst, change type to 0, recalc checksum, send reply
- `icmpv4_ping`: send echo request, wait for matching echo reply (match ID+sequence), timeout

**5b. ICMPv6**

```c
#define ICMPV6_RS         133    // Router Solicitation
#define ICMPV6_RA         134    // Router Advertisement
#define ICMPV6_NS         135    // Neighbor Solicitation
#define ICMPV6_NA         136    // Neighbor Advertisement
#define ICMPV6_ECHO_REQ   128
#define ICMPV6_ECHO_REPLY 129

void icmpv6_init(void);
int  icmpv6_ping(const uint8_t* dst, int timeout_ms);
void icmpv6_handle(const uint8_t* src, const uint8_t* data, uint32_t len);
void icmpv6_send_rs(void);     // Router Solicitation for SLAAC
```

- Register with IPv6 for next_header 58
- `icmpv6_handle`: dispatch by ICMPv6 type:
  - Echo request → echo reply (swap addrs, recalc checksum)
  - NS → call ndp_handle_ns (send NA)
  - NA → call ndp_handle_na (update neighbor cache)
  - RA → call ndp_handle_ra (store prefix for SLAAC)
- ICMPv6 checksum mandatory (pseudo-header + ICMPv6 message)

**Verification:**
| # | Test | Method | Expected |
|---|------|--------|----------|
| 5.1 | Ping IPv4 gateway | `icmpv4_ping(10.0.2.2, 3000)` | Reply received |
| 5.2 | Host pings kernel | Ping 10.0.2.15 from host | Kernel sends echo reply |
| 5.3 | Sequential v4 pings | 5× `icmpv4_ping(10.0.2.2, 1000)` | All 5 succeed |
| 5.4 | Ping IPv6 gateway | `icmpv6_ping(fe80::2, 3000)` | Reply received |
| 5.5 | RS/RA exchange | `icmpv6_send_rs()` | RA received within 3s |
| 5.6 | Sequential v6 pings | 5× `icmpv6_ping(fe80::2, 1000)` | All 5 succeed |

---

### Phase 6: UDP (Dual-Stack)

**Files:** `os/src/kernel/udp.c`, `os/src/kernel/udp.h`

```c
typedef struct __attribute__((packed)) {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint16_t checksum;     // 0 = no checksum (IPv4); mandatory for IPv6
} udp_hdr_t;

typedef struct {
    int   used;
    int   af;              // AF_INET or AF_INET6
    uint16_t port;
    union {
        uint32_t ipv4;
        uint8_t  ipv6[16];
    } src_addr;            // filled on receive
    uint16_t src_port;     // filled on receive
    void (*callback)(int af, const void* src_ip, uint16_t src_port,
                     const uint8_t* data, uint32_t len);
} udp_socket_t;

void  udp_init(void);
int   udp_listen(int af, uint16_t port, void (*cb)(...));
int   udp_sendto(int af, const void* dst_ip, uint16_t dst_port,
                 uint16_t src_port,
                 const uint8_t* data, uint32_t len);
void  udp_handle(int af, const void* src_ip,
                 const uint8_t* data, uint32_t len);
```

- Register with IPv4 (protocol 17) and IPv6 (next_header 17)
- `udp_handle`: parse header, match (af, dst_port) against registered sockets, call callback with src info
- `udp_sendto`: build header, compute checksum (mandatory for IPv6 with pseudo-header, optional for IPv4), call ipv4_send or ipv6_send
- Max 16 sockets, shared between v4 and v6

**Verification:**
| # | Test | Method | Expected |
|---|------|--------|----------|
| 6.1 | DNS query | Send to 10.0.2.3:53 | Response received |
| 6.2 | UDP echo | Send to 10.0.2.2:7 (echo) | Echo response |
| 6.3 | Two listeners | Register two different ports | Both receive |
| 6.4 | UDPv6 send | Send to [fe80::2]:7 | Echo response |

---

### Phase 7: TCP (Dual-Stack)

**Files:** `os/src/kernel/tcp.c`, `os/src/kernel/tcp.h`

```c
typedef struct __attribute__((packed)) {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t  offset;       // data offset (4 bits) | reserved (4 bits)
    uint8_t  flags;        // FIN=1, SYN=2, RST=4, PSH=8, ACK=16, URG=32
    uint16_t window;
    uint16_t checksum;     // mandatory (pseudo-header)
    uint16_t urgent;
} tcp_hdr_t;

typedef enum {
    TCP_CLOSED, TCP_LISTEN, TCP_SYN_SENT, TCP_SYN_RECEIVED,
    TCP_ESTABLISHED, TCP_FIN_WAIT1, TCP_FIN_WAIT2,
    TCP_CLOSE_WAIT, TCP_CLOSING, TCP_LAST_ACK, TCP_TIME_WAIT
} tcp_state_t;

typedef struct tcp_conn {
    int         used;
    int         af;          // AF_INET or AF_INET6
    tcp_state_t state;
    uint16_t    local_port;
    union { uint32_t v4; uint8_t v6[16]; } remote_ip;
    uint16_t    remote_port;
    uint32_t    snd_nxt;     // next seq to send
    uint32_t    rcv_nxt;     // next seq expected
    uint32_t    snd_una;     // oldest unacknowledged
    uint8_t     rcv_buf[65536];
    uint32_t    rcv_pos;
    uint32_t    rcv_total;
    wait_queue_t rcv_waitq;
    void (*on_recv)(struct tcp_conn* conn, const uint8_t* data, uint32_t len);
    void (*on_close)(struct tcp_conn* conn);
} tcp_conn_t;
```

```
void tcp_init(void);
tcp_conn_t* tcp_listen(int af, uint16_t port,
                        void (*on_accept)(tcp_conn_t* conn));
tcp_conn_t* tcp_connect(int af, const void* dst_ip, uint16_t dst_port,
                         void (*on_recv)(...), int timeout_ms);
int  tcp_send(tcp_conn_t* conn, const uint8_t* data, uint32_t len);
int  tcp_close(tcp_conn_t* conn);
void tcp_handle(int af, const void* src_ip,
                const uint8_t* data, uint32_t len);
```

- Register with IPv4 (protocol 6) and IPv6 (next_header 6)
- State machine: CLOSED → LISTEN → SYN_RCVD → ESTABLISHED (server path); CLOSED → SYN_SENT → ESTABLISHED (client path)
- `tcp_handle`: match (af, src_ip, src_port, dst_port) against connection table, update state, buffer data, send ACK
- `tcp_send`: build header, seq = snd_nxt, ack = rcv_nxt, flags as appropriate, checksum (mandatory with pseudo-header), call ip_send
- No retransmit, no congestion control in initial version (minimal functional TCP)
- Max 16 connections, shared between v4 and v6
- Window: fixed at 65535

**Verification:**
| # | Test | Method | Expected |
|---|------|--------|----------|
| 7.1 | TCP connect | `tcp_connect(AF_INET, 10.0.2.2, 7, ...)` | SYN→SYN+ACK→ACK |
| 7.2 | TCP echo | Send data to port 7 | Echo response |
| 7.3 | HTTP request | Send `GET / HTTP/1.0\r\n\r\n` to 80 | HTTP response data |
| 7.4 | TCPv6 connect | `tcp_connect(AF_INET6, fe80::2, 7, ...)` | Connection established |

---

### Phase 8: Sockets API (Dual-Stack)

**Files:** `os/src/kernel/socket.c`, `os/src/kernel/socket.h`

**Syscalls (38–46):**
```c
#define SYS_SOCKET    38   // socket(domain, type, protocol) → fd
#define SYS_BIND      39   // bind(fd, addr, addrlen)
#define SYS_LISTEN    40   // listen(fd, backlog)
#define SYS_ACCEPT    41   // accept(fd, addr, addrlen) → fd
#define SYS_CONNECT   42   // connect(fd, addr, addrlen)
#define SYS_SEND      43   // send(fd, buf, len, flags)
#define SYS_RECV      44   // recv(fd, buf, len, flags)
#define SYS_SENDTO    45   // sendto(fd, buf, len, flags, addr, addrlen)
#define SYS_RECVFROM  46   // recvfrom(fd, buf, len, flags, addr, addrlen)
```

**Socket address structures:**
```c
#define AF_INET   2
#define AF_INET6  10
#define SOCK_DGRAM   1     // UDP
#define SOCK_STREAM  2     // TCP

struct sockaddr_in {
    uint16_t sin_family;    // AF_INET
    uint16_t sin_port;      // big-endian
    uint32_t sin_addr;      // big-endian
    uint8_t  sin_zero[8];
};

struct sockaddr_in6 {
    uint16_t sin6_family;   // AF_INET6
    uint16_t sin6_port;     // big-endian
    uint32_t sin6_flowinfo;
    uint8_t  sin6_addr[16];
    uint32_t sin6_scope_id;
};

// IPv4-mapped IPv6 address for v6-only sockets talking to v4
// ::ffff:10.0.2.15
#define IN6_IS_ADDR_V4MAPPED(a) \
    ((a)[0]==0&&(a)[1]==0&&(a)[2]==0&&(a)[3]==0&&(a)[4]==0&&(a)[5]==0&&(a)[6]==0&&(a)[7]==0&&(a)[8]==0&&(a)[9]==0&&(a)[10]==0xff&&(a)[11]==0xff)
```

**Internal socket:**
```c
typedef enum { SOCK_UNUSED, SOCK_UDP, SOCK_TCP } socket_type_t;

typedef struct {
    int         used;
    socket_type_t type;
    int         af;              // AF_INET or AF_INET6
    uint16_t    local_port;
    union { uint32_t v4; uint8_t v6[16]; } local_ip;
    union { uint32_t v4; uint8_t v6[16]; } remote_ip;
    uint16_t    remote_port;
    int         bound;
    int         listening;
    tcp_conn_t* tcp_conn;
    uint8_t     rcv_buf[65536];
    uint32_t    rcv_pos;
    uint32_t    rcv_total;
    wait_queue_t rcv_waitq;
} socket_t;
```

**Syscall implementations:**
- `sys_socket`: allocate socket_t table entry, return fd (uses FD table)
- `sys_bind`: validate addr, assign port (auto-pick ephemeral if 0), mark bound
- `sys_listen`: TCP only, mark listening, call tcp_listen
- `sys_accept`: TCP only, block on accept queue, create new fd for accepted connection
- `sys_connect`: UDP → set remote addr; TCP → call tcp_connect, block until established/timeout
- `sys_sendto`: UDP → udp_sendto; TCP (NULL addr) → tcp_send
- `sys_recvfrom`: block on socket rcv_waitq, copy to user, return src addr for UDP

**Verification:**
| # | Test | Method | Expected |
|---|------|--------|----------|
| 8.1 | socket+sendto | `fd=socket(AF_INET,SOCK_DGRAM,0); sendto(fd, buf, 4, ..., &dest)` | Returns 4 |
| 8.2 | TCP echo via sockets | socket+connect+send+recv to port 7 | Echo round-trip |
| 8.3 | AF_INET6 socket | `socket(AF_INET6, SOCK_DGRAM, 0)` | Returns fd |
| 8.4 | v4-mapped-v6 | `socket(AF_INET6, ...)` accept v4 connection | Works via ::ffff:x.x.x.x |

---

### Phase 9: DNS Resolver (Dual-Stack)

**Files:** `os/src/kernel/dns.c`, `os/src/kernel/dns.h`

```c
#define DNS_SERVER_V4  0x0A000203   // 10.0.2.3
#define DNS_SERVER_V6  /* from RA or hardcoded */

int dns_resolve(const char* hostname, int af, void* out_addr, int timeout_ms);
// af = AF_INET → out_addr is uint32_t* (IPv4)
// af = AF_INET6 → out_addr is uint8_t[16]* (IPv6)
// af = AF_UNSPEC → try A first, then AAAA
```

- Build DNS query (wire format: header + question with QNAME compression)
- Send via UDP to DNS_SERVER:53
- Parse response header, extract answer records (type A for v4, AAAA for v6)
- Retry 3× with 2s timeout
- `AF_UNSPEC` mode: try A record, if no result try AAAA

**Verification:**
| # | Test | Method | Expected |
|---|------|--------|----------|
| 9.1 | Resolve A | `dns_resolve("google.com", AF_INET, &ip, 5000)` | Returns IPv4 |
| 9.2 | Resolve AAAA | `dns_resolve("google.com", AF_INET6, &ip, 5000)` | Returns IPv6 |
| 9.3 | Bad name | `dns_resolve("nx.invalid", AF_INET, &ip, 2000)` | ERR_TIMEOUT |

---

### Phase 10: DHCP Client + SLAAC

**Files:** `os/src/kernel/dhcp.c`, `os/src/kernel/dhcp.h`

```c
int dhcp_request(int timeout_ms);
// Sets IPV4_ADDR, IPV4_GATEWAY, IPV4_NETMASK, DNS_SERVER_V4
```

- Build DHCPDISCOVER (broadcast, UDP src=68, dst=67, client MAC + transaction ID)
- Receive DHCPOFFER from 10.0.2.4 → extract offered IP, server IP, lease time, DNS
- Build DHCPREQUEST (broadcast, include offered IP from OFFER)
- Receive DHCPACK → set IP configuration, configure route
- Renewal: schedule deferred task at 50% lease time

SLAAC (for IPv6) handled by ICMPv6 RS/RA exchange in Phase 5:
- Send Router Solicitation on boot
- Receive Router Advertisement → extract prefix, form global address via EUI-64
- Set IPv6 default route to RA source

**Verification:**
| # | Test | Method | Expected |
|---|------|--------|----------|
| 10.1 | DHCP request | `dhcp_request(5000)` | Gets 10.0.2.x/24 within 5s |
| 10.2 | Network up | Ping gateway after DHCP | Works |
| 10.3 | SLAAC | Boot w/ IPv6, check global addr | Global address assigned |

---

### Phase 11: NTP Client

**Files:** `os/src/kernel/ntp.c`, `os/src/kernel/ntp.h`

```c
int ntp_sync(const void* server_ip, int af, int timeout_ms);
// Updates system clock via hal_timer_set_epoch()
```

- Build NTP request (version 3/4, mode 3 = client, transmit timestamp)
- Send via UDP to server:123
- Parse response: extract reference timestamp (seconds + fraction)
- Convert to Unix epoch, call `hal_timer_set_epoch()`
- Retry 3× with 2s timeout

**Verification:**
| # | Test | Method | Expected |
|---|------|--------|----------|
| 11.1 | NTP sync | `ntp_sync(10.0.2.2, AF_INET, 5000)` | Clock updated (print new UNIX time) |

---

## Automated Test Plan (`make test` additions)

Each phase adds steps to the `make test` target:
```
Phase 1:  echo "nic_test"              → prints MAC, ring stats
Phase 3:  echo "arp_test 10.0.2.2"     → resolves MAC, prints it
Phase 4:  echo "ipv6_ll_test"          → prints link-local address
Phase 5:  echo "ping 10.0.2.2"         → echo reply received OK
Phase 5:  echo "ping6 fe80::2"         → ICMPv6 echo reply OK
Phase 6:  echo "dns_test google.com"   → prints resolved IPv4 + IPv6
Phase 7:  echo "tcp_connect_test 7"    → TCP echo, prints "PASS"/"FAIL"
Phase 10: echo "dhcp_renew"            → prints new IP config
Phase 11: echo "ntp_sync"              → prints new system time
```

## File Manifest

New kernel files:
```
os/src/kernel/nic.h      — NIC abstraction
os/src/kernel/e1000.c    — E1000 PCI driver
os/src/kernel/e1000.h    — E1000 registers
os/src/kernel/eth.c      — Ethernet frame layer
os/src/kernel/eth.h
os/src/kernel/arp.c      — ARP cache + protocol
os/src/kernel/arp.h
os/src/kernel/ndp.c      — NDP cache + protocol (IPv6)
os/src/kernel/ndp.h
os/src/kernel/ipv4.c     — IPv4 send/receive
os/src/kernel/ipv4.h
os/src/kernel/ipv6.c     — IPv6 send/receive
os/src/kernel/ipv6.h
os/src/kernel/route.c    — Routing table (v4 + v6)
os/src/kernel/route.h
os/src/kernel/icmp.c     — ICMPv4 echo
os/src/kernel/icmp.h
os/src/kernel/icmpv6.c   — ICMPv6 echo + RS/RA/NS/NA
os/src/kernel/icmpv6.h
os/src/kernel/udp.c      — UDP (v4 + v6)
os/src/kernel/udp.h
os/src/kernel/tcp.c      — TCP state machine (v4 + v6)
os/src/kernel/tcp.h
os/src/kernel/socket.c   — Socket syscalls (AF_INET + AF_INET6)
os/src/kernel/socket.h
os/src/kernel/dns.c      — DNS resolver (A + AAAA)
os/src/kernel/dns.h
os/src/kernel/dhcp.c     — DHCP client
os/src/kernel/dhcp.h
os/src/kernel/ntp.c      — NTP client
os/src/kernel/ntp.h
os/src/kernel/net.c      — Network init + test harness
os/src/kernel/net.h
```

Modified files:
```
os/src/include/syscall_defs.h  — Add SYS_SOCKET..SYS_RECVFROM (38–46)
os/src/kernel/syscall.c       — Add handler entries
os/src/kernel/main.c          — Add net_init() call
os/src/kernel/shell.c         — Add ping, ping6, dns, dhcp shell commands
os/docs/PROGRESS.md           — Update per phase
os/docs/ROADMAP.md            — Update per phase
```

Userspace additions:
```
os/src/include/sys/socket.h   — AF_INET, AF_INET6, sockaddr_in, sockaddr_in6, SOCK_DGRAM, SOCK_STREAM
os/src/lib/libuser/unistd.c   — socket/bind/listen/accept/connect/send/recv wrappers
os/src/include/unistd.h       — Declarations
```

## Error Handling

All networking functions return `err_t` (negative `ERR_*` values):
- `ERR_IO` — NIC hardware failure, TX/RX descriptor timeout
- `ERR_TIMEOUT` — ARP/NDP resolve, connect, DNS, DHCP
- `ERR_NOMEM` — Socket/connection table full, bounce buffer alloc failure
- `ERR_INVAL` — Invalid address family, port, socket parameters
- `ERR_AGAIN` — Would block (non-blocking I/O)
- `ERR_NOSYS` — Protocol/family not supported
- `ERR_EXIST` — Port already in use (bind)

## Testing Methodology Per Phase

1. **Boot-time self-test:** `net_init()` runs `nic_test()`, then `arp_test()`, `ping_test()`, etc. after init. Results printed with `[NET] PASS/FAIL`.
2. **Shell commands:** Interactive `ping <ip>`, `ping6 <ipv6>`, `dns <name>`, `dhcp`, `ntp`.
3. **Automated test:** `make test` script sends shell commands, greps for `PASS`/`FAIL` markers.

## Build & Test Command

```bash
make clean && make -j4         # build
make headless                  # run, watch serial for net test output
make test                      # full automated suite
```

For TCP testing, add QEMU port forwarding:
```makefile
# In Makefile, append to QEMU flags:
QEMU_NET := -nic user,hostfwd=tcp:127.0.0.1:8080-:80
```

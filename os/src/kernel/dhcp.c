#include "kernel.h"
#include "dhcp.h"
#include "udp.h"
#include "ipv4.h"
#include "route.h"
#include "nic.h"
#include "eth.h"
#include "sched.h"

#define DHCP_BOOTREQUEST 1
#define DHCP_BOOTREPLY   2
#define DHCP_HTYPE_ETH   1
#define DHCP_HLEN_ETH    6

#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68

#define DHCP_DISCOVER 1
#define DHCP_OFFER    2
#define DHCP_REQUEST  3
#define DHCP_ACK      5
#define DHCP_NAK      6

#define DHCP_OPT_PAD         0
#define DHCP_OPT_SUBNET_MASK 1
#define DHCP_OPT_ROUTER      3
#define DHCP_OPT_DNS         6
#define DHCP_OPT_HOSTNAME   12
#define DHCP_OPT_REQ_IP     50
#define DHCP_OPT_LEASE_TIME 51
#define DHCP_OPT_MSG_TYPE   53
#define DHCP_OPT_SERVER_ID  54
#define DHCP_OPT_PARAM_REQ  55
#define DHCP_OPT_END       255

#define DHCP_MAGIC_COOKIE 0x63825363

#define DHCP_TIMEOUT_MS 1500
#define DHCP_RETRIES    1

typedef struct __attribute__((packed)) {
    uint8_t  op;
    uint8_t  htype;
    uint8_t  hlen;
    uint8_t  hops;
    uint32_t xid;
    uint16_t secs;
    uint16_t flags;
    uint8_t  ciaddr[4];
    uint8_t  yiaddr[4];
    uint8_t  siaddr[4];
    uint8_t  giaddr[4];
    uint8_t  chaddr[16];
    uint8_t  sname[64];
    uint8_t  file[128];
    uint8_t  options[];
} dhcp_pkt_t;

static uint8_t* dhcp_put_option(uint8_t* p, uint8_t type, uint8_t len, const uint8_t* val) {
    *p++ = type;
    *p++ = len;
    kmemcpy(p, val, len);
    return p + len;
}

static uint8_t* dhcp_put_byte(uint8_t* p, uint8_t type, uint8_t val) {
    *p++ = type;
    *p++ = 1;
    *p++ = val;
    return p;
}

static int dhcp_parse_options(const uint8_t* options, uint32_t len,
                              uint8_t* out_msg_type,
                              uint8_t* out_yiaddr,
                              uint8_t* out_subnet,
                              uint8_t* out_router,
                              uint8_t* out_server_id,
                              uint32_t* out_lease) {
    uint32_t off = 0;
    *out_msg_type = 0;

    while (off < len) {
        uint8_t type = options[off++];
        if (type == DHCP_OPT_END) break;
        if (type == DHCP_OPT_PAD) continue;
        if (off >= len) break;

        uint8_t opt_len = options[off++];
        if (off + opt_len > len) break;

        switch (type) {
        case DHCP_OPT_MSG_TYPE:
            if (opt_len >= 1) *out_msg_type = options[off];
            break;
        case DHCP_OPT_SUBNET_MASK:
            if (opt_len >= 4) kmemcpy(out_subnet, options + off, 4);
            break;
        case DHCP_OPT_ROUTER:
            if (opt_len >= 4) kmemcpy(out_router, options + off, 4);
            break;
        case DHCP_OPT_SERVER_ID:
            if (opt_len >= 4) kmemcpy(out_server_id, options + off, 4);
            break;
        case DHCP_OPT_LEASE_TIME:
            if (opt_len >= 4) {
                *out_lease = ((uint32_t)options[off] << 24) |
                             ((uint32_t)options[off+1] << 16) |
                             ((uint32_t)options[off+2] << 8) |
                             (uint32_t)options[off+3];
            }
            break;
        }
        off += opt_len;
    }
    return 1;
}

static int dhcp_build(uint8_t* buf, uint32_t* out_len,
                      uint8_t msg_type, uint32_t xid,
                      const uint8_t* req_ip, const uint8_t* server_id) {
    dhcp_pkt_t* pkt = (dhcp_pkt_t*)buf;
    kmemset(buf, 0, sizeof(dhcp_pkt_t));

    pkt->op = DHCP_BOOTREQUEST;
    pkt->htype = DHCP_HTYPE_ETH;
    pkt->hlen = DHCP_HLEN_ETH;
    pkt->xid = __builtin_bswap32(xid);
    pkt->flags = __builtin_bswap16(0x8000);
    kmemcpy(pkt->chaddr, nic.mac, 6);

    if (req_ip && msg_type == DHCP_REQUEST) {
        kmemcpy(pkt->ciaddr, req_ip, 4);
    }

    uint8_t* opt = pkt->options;

    uint32_t magic = __builtin_bswap32(DHCP_MAGIC_COOKIE);
    kmemcpy(opt, &magic, 4);
    opt += 4;

    opt = dhcp_put_byte(opt, DHCP_OPT_MSG_TYPE, msg_type);

    if (msg_type == DHCP_REQUEST) {
        if (req_ip) {
            opt = dhcp_put_option(opt, DHCP_OPT_REQ_IP, 4, req_ip);
        }
        if (server_id) {
            opt = dhcp_put_option(opt, DHCP_OPT_SERVER_ID, 4, server_id);
        }
    }

    {
        uint8_t param_req[] = {DHCP_OPT_SUBNET_MASK, DHCP_OPT_ROUTER, DHCP_OPT_DNS,
                               DHCP_OPT_LEASE_TIME, DHCP_OPT_SERVER_ID};
        opt = dhcp_put_option(opt, DHCP_OPT_PARAM_REQ, sizeof(param_req), param_req);
    }

    *opt++ = DHCP_OPT_END;

    *out_len = (uint32_t)(opt - buf);
    return 1;
}

static int dhcp_recv(udp_endpoint_t* ep, uint32_t xid,
                     uint8_t* out_msg_type, uint8_t* out_yiaddr,
                     uint8_t* out_subnet, uint8_t* out_router,
                     uint8_t* out_server_id, uint32_t* out_lease,
                     int timeout_ms) {
    uint8_t buf[548];
    uint8_t src_ip[16];
    int src_af;
    uint16_t src_port;

    int step = 100;
    int steps = timeout_ms / step;
    for (int i = 0; i < steps; i++) {
        eth_rx_poll();
        int n = udp_endpoint_dequeue(ep, buf, sizeof(buf),
                                     &src_af, src_ip, &src_port, step);
        if (n <= 0) continue;
        if ((uint32_t)n < sizeof(dhcp_pkt_t)) continue;

        const dhcp_pkt_t* pkt = (const dhcp_pkt_t*)buf;
        if (pkt->op != DHCP_BOOTREPLY) continue;

        uint32_t rx_xid = __builtin_bswap32(pkt->xid);
        if (rx_xid != xid) continue;

        uint32_t options_len = (uint32_t)n - sizeof(dhcp_pkt_t);
        const uint8_t* options = pkt->options;

        if (options_len < 4) continue;
        uint32_t magic = __builtin_bswap32(*(const uint32_t*)options);
        if (magic != DHCP_MAGIC_COOKIE) continue;

        uint8_t msg_type = 0;
        uint8_t subnet[4] = {0};
        uint8_t router[4] = {0};
        uint8_t server_id[4] = {0};

        uint8_t yiaddr_tmp[4];
        kmemcpy(yiaddr_tmp, pkt->yiaddr, 4);
        dhcp_parse_options(options + 4, options_len - 4,
                           &msg_type, yiaddr_tmp, subnet, router, server_id, out_lease);

        if (msg_type == 0) continue;

        *out_msg_type = msg_type;
        kmemcpy(out_yiaddr, pkt->yiaddr, 4);
        kmemcpy(out_subnet, subnet, 4);
        kmemcpy(out_router, router, 4);
        kmemcpy(out_server_id, server_id, 4);
        return ERR_OK;
    }
    return ERR_TIMEOUT;
}

err_t dhcp_configure(void) {
    kprintf("[DHCP] Starting DHCP configuration...\n");

    if (!nic.present) {
        kprintf("[DHCP] NIC not present\n");
        return ERR_IO;
    }

    uint32_t xid = (uint32_t)nic.mac[0] |
                  ((uint32_t)nic.mac[1] << 8) |
                  ((uint32_t)nic.mac[2] << 16) |
                  ((uint32_t)nic.mac[3] << 24);

    int e = udp_bind_endpoint(AF_INET, NULL, DHCP_CLIENT_PORT, 0, 0, 1);
    if (e != ERR_OK) {
        kprintf("[DHCP] Failed to bind port %d: %d\n", DHCP_CLIENT_PORT, e);
        return e;
    }

    udp_endpoint_t* ep = udp_find_endpoint(AF_INET, DHCP_CLIENT_PORT);

    ipv4_addr_t bcast = ipv4_from_bytes(255, 255, 255, 255);

    uint8_t local_yiaddr[4] = {0};
    uint8_t local_subnet[4] = {0};
    uint8_t local_router[4] = {0};
    uint8_t local_server_id[4] = {0};
    int got_offer = 0;

    for (int retry = 0; retry < DHCP_RETRIES; retry++) {
        uint8_t tx_buf[300];
        uint32_t tx_len;

        dhcp_build(tx_buf, &tx_len, DHCP_DISCOVER, xid, NULL, NULL);
        e = udp_sendto(AF_INET, &bcast, DHCP_SERVER_PORT, DHCP_CLIENT_PORT,
                       tx_buf, tx_len);
        kprintf("[DHCP] Sent DISCOVER (retry=%d): %d\n", retry, e);

        uint8_t msg_type = 0;
        uint8_t yiaddr[4] = {0};
        uint8_t subnet[4] = {0};
        uint8_t router[4] = {0};
        uint8_t server_id[4] = {0};
        uint32_t rx_lease = 0;

        e = dhcp_recv(ep, xid, &msg_type, yiaddr, subnet, router, server_id, &rx_lease,
                      DHCP_TIMEOUT_MS);
        if (e != ERR_OK) {
            kprintf("[DHCP] No OFFER on retry %d\n", retry);
            continue;
        }

        if (msg_type == DHCP_OFFER) {
            kprintf("[DHCP] Got OFFER: IP=%d.%d.%d.%d router=%d.%d.%d.%d "
                    "mask=%d.%d.%d.%d server=%d.%d.%d.%d lease=%u\n",
                    yiaddr[0], yiaddr[1], yiaddr[2], yiaddr[3],
                    router[0], router[1], router[2], router[3],
                    subnet[0], subnet[1], subnet[2], subnet[3],
                    server_id[0], server_id[1], server_id[2], server_id[3],
                    rx_lease);
            kmemcpy(local_yiaddr, yiaddr, 4);
            kmemcpy(local_subnet, subnet, 4);
            kmemcpy(local_router, router, 4);
            kmemcpy(local_server_id, server_id, 4);
            got_offer = 1;
            break;
        }
    }

    if (!got_offer) {
        kprintf("[DHCP] No OFFER received after %d retries\n", DHCP_RETRIES);
        udp_unbind_endpoint(ep);
        return ERR_TIMEOUT;
    }

    /* Send REQUEST */
    for (int retry = 0; retry < DHCP_RETRIES; retry++) {
        uint8_t tx_buf[300];
        uint32_t tx_len;

        dhcp_build(tx_buf, &tx_len, DHCP_REQUEST, xid, local_yiaddr, local_server_id);
        e = udp_sendto(AF_INET, &bcast, DHCP_SERVER_PORT, DHCP_CLIENT_PORT,
                       tx_buf, tx_len);
        kprintf("[DHCP] Sent REQUEST (retry=%d): %d\n", retry, e);

        uint8_t msg_type = 0;
        uint8_t yiaddr[4] = {0};
        uint8_t subnet[4] = {0};
        uint8_t router[4] = {0};
        uint8_t server_id[4] = {0};
        uint32_t rx_lease = 0;

        e = dhcp_recv(ep, xid, &msg_type, yiaddr, subnet, router, server_id, &rx_lease,
                      DHCP_TIMEOUT_MS);
        if (e != ERR_OK) {
            kprintf("[DHCP] No ACK on retry %d\n", retry);
            continue;
        }

        if (msg_type == DHCP_ACK) {
            kprintf("[DHCP] Got ACK: IP=%d.%d.%d.%d router=%d.%d.%d.%d "
                    "mask=%d.%d.%d.%d lease=%u\n",
                    yiaddr[0], yiaddr[1], yiaddr[2], yiaddr[3],
                    router[0], router[1], router[2], router[3],
                    subnet[0], subnet[1], subnet[2], subnet[3],
                    server_id[0], server_id[1], server_id[2], server_id[3],
                    rx_lease);
            kmemcpy(local_yiaddr, yiaddr, 4);
            kmemcpy(local_subnet, subnet, 4);
            kmemcpy(local_router, router, 4);
            goto configured;
        } else if (msg_type == DHCP_NAK) {
            kprintf("[DHCP] Got NAK\n");
            udp_unbind_endpoint(ep);
            return ERR_PERM;
        }
    }

    kprintf("[DHCP] No ACK received\n");
    udp_unbind_endpoint(ep);
    return ERR_TIMEOUT;

configured:
    ipv4_set_addr(ipv4_from_bytes(local_yiaddr[0], local_yiaddr[1],
                                   local_yiaddr[2], local_yiaddr[3]));

    /* Calculate prefix length from subnet mask */
    int prefix_len = 0;
    uint32_t mask_raw = (uint32_t)local_subnet[0] << 24 |
                        (uint32_t)local_subnet[1] << 16 |
                        (uint32_t)local_subnet[2] << 8 |
                        (uint32_t)local_subnet[3];
    for (int i = 31; i >= 0; i--) {
        if (mask_raw & (1U << i)) prefix_len++;
    }

    /* Add directly connected route for subnet */
    ipv4_addr_t subnet_net = ipv4_from_bytes(
        local_yiaddr[0] & local_subnet[0],
        local_yiaddr[1] & local_subnet[1],
        local_yiaddr[2] & local_subnet[2],
        local_yiaddr[3] & local_subnet[3]);
    route_add_v4(subnet_net, prefix_len, ipv4_from_bytes(0, 0, 0, 0));

    /* Add default route via gateway */
    if (local_router[0] != 0 || local_router[1] != 0 ||
        local_router[2] != 0 || local_router[3] != 0) {
        route_add_v4(ipv4_from_bytes(0, 0, 0, 0), 0,
                     ipv4_from_bytes(local_router[0], local_router[1],
                                     local_router[2], local_router[3]));
    }

    kprintf("[DHCP] Configuration complete\n");
    udp_unbind_endpoint(ep);
    return ERR_OK;
}

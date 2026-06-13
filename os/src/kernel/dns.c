#include "kernel.h"
#include "dns.h"
#include "udp.h"
#include "route.h"
#include "sched.h"

static int dns_initialized = 0;

static uint8_t dns_resolver_v4[4] = {8, 8, 8, 8};
static uint8_t dns_resolver_v6[16];

static uint16_t dns_next_port = 50000;

void dns_set_resolver_v4(const uint8_t* addr) {
    kmemcpy(dns_resolver_v4, addr, 4);
}

void dns_set_resolver_v6(const uint8_t* addr) {
    kmemcpy(dns_resolver_v6, addr, 16);
}

static int dns_encode_name(uint8_t* buf, int bufsize, const char* hostname) {
    int pos = 0;
    while (*hostname) {
        const char* dot = hostname;
        while (*dot && *dot != '.') dot++;
        int len = (int)(dot - hostname);
        if (len > 63) return ERR_NAMETOOLONG;
        if (pos + 1 + len > bufsize) return ERR_NOSPACE;
        buf[pos++] = (uint8_t)len;
        kmemcpy(buf + pos, hostname, len);
        pos += len;
        hostname = dot;
        if (*dot == '.') hostname++;
    }
    if (pos >= bufsize) return ERR_NOSPACE;
    buf[pos++] = 0;
    return pos;
}

static int dns_build_query(uint8_t* buf, int bufsize,
                            const char* hostname, uint16_t qtype,
                            uint16_t txid) {
    if (bufsize < 12) return ERR_NOSPACE;
    buf[0] = (uint8_t)(txid >> 8);
    buf[1] = (uint8_t)(txid);
    buf[2] = 0x01;
    buf[3] = 0x00;
    buf[4] = 0;
    buf[5] = 1;
    buf[6] = 0;
    buf[7] = 0;
    buf[8] = 0;
    buf[9] = 0;
    buf[10] = 0;
    buf[11] = 0;
    int pos = 12;
    int e = dns_encode_name(buf + pos, bufsize - pos, hostname);
    if (e < 0) return e;
    pos += e;
    if (pos + 4 > bufsize) return ERR_NOSPACE;
    buf[pos++] = (uint8_t)(qtype >> 8);
    buf[pos++] = (uint8_t)(qtype);
    buf[pos++] = 0;
    buf[pos++] = DNS_CLASS_IN;
    return pos;
}

static int dns_skip_name(const uint8_t* pkt, int pktlen, int pos) {
    int jumped = 0;
    int orig_pos = pos;
    while (pos < pktlen) {
        uint8_t b = pkt[pos];
        if (b == 0) {
            if (!jumped) pos++;
            return pos;
        }
        if ((b & 0xC0) == 0xC0) {
            if (pos + 2 > pktlen) return ERR_INVAL;
            if (!jumped) {
                orig_pos = pos + 2;
                jumped = 1;
            }
            pos = (int)(((uint16_t)(b & 0x3F) << 8) | pkt[pos + 1]);
            continue;
        }
        if (pos + 1 + (int)b > pktlen) return ERR_INVAL;
        pos += 1 + (int)b;
        if (jumped) break;
    }
    return jumped ? orig_pos : pos;
}

static int dns_parse_response(const uint8_t* resp, int rlen,
                               uint16_t expected_id,
                               uint8_t* addr_out, int* addrlen_out) {
    if (rlen < 12) return ERR_INVAL;

    uint16_t id = (uint16_t)((resp[0] << 8) | resp[1]);
    if (id != expected_id) return ERR_AGAIN;

    uint8_t rcode = resp[3] & 0x0F;
    if (rcode == 3) return ERR_NOENT;
    if (rcode != 0) return ERR_GENERAL;
    if (!(resp[2] & 0x80)) return ERR_INVAL;

    uint16_t qdcount = (uint16_t)((resp[4] << 8) | resp[5]);
    uint16_t ancount = (uint16_t)((resp[6] << 8) | resp[7]);
    if (ancount == 0) return ERR_NOENT;

    int pos = 12;
    for (uint16_t i = 0; i < qdcount; i++) {
        int e = dns_skip_name(resp, rlen, pos);
        if (e < 0) return e;
        pos = e + 4;
        if (pos > rlen) return ERR_INVAL;
    }

    for (uint16_t i = 0; i < ancount; i++) {
        int e = dns_skip_name(resp, rlen, pos);
        if (e < 0) return e;
        pos = e;
        if (pos + 10 > rlen) return ERR_INVAL;

        uint16_t rtype  = (uint16_t)((resp[pos] << 8) | resp[pos + 1]);
        uint16_t rclass = (uint16_t)((resp[pos + 2] << 8) | resp[pos + 3]);
        uint32_t rr_ttl  = (uint32_t)((resp[pos + 4] << 24) | (resp[pos + 5] << 16) |
                                      (resp[pos + 6] << 8)  | resp[pos + 7]);
        (void)rr_ttl;
        pos += 8;

        uint16_t rdlen = (uint16_t)((resp[pos] << 8) | resp[pos + 1]);
        pos += 2;
        if (pos + rdlen > rlen) return ERR_INVAL;

        if (rclass == DNS_CLASS_IN) {
            if (rtype == DNS_TYPE_A && rdlen == 4) {
                kmemcpy(addr_out, resp + pos, 4);
                *addrlen_out = 4;
                return ERR_OK;
            }
            if (rtype == DNS_TYPE_AAAA && rdlen == 16) {
                kmemcpy(addr_out, resp + pos, 16);
                *addrlen_out = 16;
                return ERR_OK;
            }
        }
        pos += rdlen;
    }
    return ERR_NOENT;
}

int dns_resolve(const char* hostname, uint8_t* addr_out, int* af_out, int timeout_ms) {
    if (!dns_initialized) return ERR_NOSYS;
    if (!hostname || !addr_out || !af_out) return ERR_INVAL;

    uint8_t query[DNS_MAX_PACKET];
    uint8_t resp[DNS_MAX_PACKET];
    uint16_t port;
    int e;

    if (timeout_ms <= 0) timeout_ms = 5000;

    port = dns_next_port++;
    e = udp_bind_endpoint(AF_INET, NULL, port, timeout_ms, 0, 1);
    if (e == ERR_NOSPACE) {
        port = dns_next_port++;
    e = udp_bind_endpoint(AF_INET, NULL, port, timeout_ms, 0, 1);
    }
    if (e != ERR_OK) return e;

    udp_endpoint_t* ep = udp_find_endpoint(AF_INET, port);

    int addrlen = 0;
    uint16_t qtypes[2] = {DNS_TYPE_A, DNS_TYPE_AAAA};

    for (int qi = 0; qi < 2 && addrlen == 0; qi++) {
        uint16_t txid = (uint16_t)(port ^ ((uint16_t)(uintptr_t)hostname << qi));
        int qlen = dns_build_query(query, sizeof(query), hostname,
                                    qtypes[qi], txid);
        if (qlen < 0) continue;

        if (qtypes[qi] == DNS_TYPE_A) {
            e = udp_sendto(AF_INET, dns_resolver_v4, DNS_PORT, port, query, qlen);
        } else {
            if (dns_resolver_v6[0] == 0 || dns_resolver_v6[1] == 0) continue;
            e = udp_sendto(AF_INET6, dns_resolver_v6, DNS_PORT, port, query, qlen);
        }
        if (e < 0) continue;

        int remaining = timeout_ms;
        while (remaining > 0 && addrlen == 0) {
            int rlen = udp_endpoint_dequeue(ep, resp, sizeof(resp),
                                             NULL, NULL, NULL, remaining);
            if (rlen <= 0) break;

            int parsed_len = 0;
            e = dns_parse_response(resp, rlen, txid, addr_out, &parsed_len);
            if (e == ERR_OK && parsed_len > 0) {
                addrlen = parsed_len;
                break;
            }
            if (e == ERR_AGAIN) {
                remaining -= 100;
                continue;
            }
            break;
        }
    }

    udp_unbind_endpoint(ep);

    if (addrlen == 4) { *af_out = AF_INET; return ERR_OK; }
    if (addrlen == 16) { *af_out = AF_INET6; return ERR_OK; }
    return ERR_NOENT;
}

void dns_init(void) {
    if (dns_initialized) return;
    kmemset(dns_resolver_v6, 0, 16);
    dns_initialized = 1;
    kprintf("[DNS] Resolver initialized (8.8.8.8:53)\n");
}

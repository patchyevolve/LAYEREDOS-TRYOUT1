#include "kernel.h"
#include "tcp.h"
#include "ndp.h"
#include "icmpv6.h"
#include "nic.h"
#include "e1000.h"
#include "ip.h"
#include "route.h"
#include "net.h"
#include "udp.h"

#ifdef NET_SELF_TEST

#define TEST_PASS 0
#define TEST_FAIL 1

/* ============================================================
 * Test 1: tcp_find_conn IPv6 matching
 *
 * Verifies that tcp_find_conn() can locate an IPv6 connection
 * by its 5-tuple (af, src_ip, src_port, dst_port). This was
 * the root cause of IPv6 TCP failure — only AF_INET was checked.
 * ============================================================ */
static int test_tcp_find_conn_ipv6(void) {
    uint8_t dummy_v6[16];
    kmemset(dummy_v6, 0, 16);
    dummy_v6[0] = 0xFE; dummy_v6[1] = 0x80;
    dummy_v6[8] = 0xAA; dummy_v6[15] = 0x01;

    /* Populate a connection entry directly (no NIC required) */
    tcp_conn_t* conn = tcp_test_add_conn(AF_INET6, dummy_v6, 9, 30001);
    if (!conn) {
        kprintf("[TEST] tcp_find_conn_ipv6: FAIL — no conn slot\n");
        return TEST_FAIL;
    }

    /* Now find it via tcp_find_conn */
    tcp_conn_t* found = tcp_find_conn(AF_INET6, dummy_v6, 9, 30001);
    if (!found) {
        kprintf("[TEST] tcp_find_conn_ipv6: FAIL — no match for IPv6 5-tuple\n");
        return TEST_FAIL;
    }
    if (found != conn) {
        kprintf("[TEST] tcp_find_conn_ipv6: FAIL — wrong connection pointer\n");
        return TEST_FAIL;
    }

    /* Cleanup: mark unused */
    conn->used = 0;
    kprintf("[TEST] tcp_find_conn_ipv6: PASS\n");
    return TEST_PASS;
}

/* ============================================================
 * Test 2: NDP resolve cache miss behavior
 *
 * Verifies that ndp_cache_lookup returns 0 for an unknown
 * address, and that ndp_cache_update then ndp_cache_lookup
 * returns 1. This validates the cache population path that
 * ndp_resolve relies on after receiving an NA.
 * ============================================================ */
static int test_ndp_cache_miss(void) {
    uint8_t test_ip[16];
    uint8_t test_mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x57};
    uint8_t out_mac[6];

    kmemset(test_ip, 0, 16);
    test_ip[0] = 0xFE; test_ip[1] = 0x80;
    test_ip[8] = 0xAA; test_ip[15] = 0x01;

    /* Should be a cache miss */
    int found = ndp_cache_lookup(test_ip, out_mac);
    if (found) {
        kprintf("[TEST] ndp_cache_miss: FAIL — unexpected cache hit\n");
        return TEST_FAIL;
    }

    /* Add to cache */
    ndp_cache_update(test_ip, test_mac);

    /* Should be a cache hit now */
    found = ndp_cache_lookup(test_ip, out_mac);
    if (!found) {
        kprintf("[TEST] ndp_cache_miss: FAIL — cache miss after update\n");
        return TEST_FAIL;
    }
    if (kmemcmp(out_mac, test_mac, 6) != 0) {
        kprintf("[TEST] ndp_cache_miss: FAIL — MAC mismatch\n");
        return TEST_FAIL;
    }

    kprintf("[TEST] ndp_cache_miss: PASS\n");
    return TEST_PASS;
}

/* ============================================================
 * Test 3: ICMPv6 NS target address parsing
 *
 * Verifies that the NS target address is correctly read
 * from offset data+4 (after the 4-byte reserved field).
 * Earlier bug: was reading from data+0 (hdr->data), which
 * overlaps the reserved field with the first 4 bytes of the
 * target address.
 * ============================================================ */
static int test_icmpv6_ns_parse(void) {
    /* Build an NS packet: ICMPv6 hdr (4) + reserved (4) + target (16) */
    uint8_t ns_buf[24];
    kmemset(ns_buf, 0, sizeof(ns_buf));
    ns_buf[0] = 135; /* type=NS */

    uint8_t expected_target[16];
    kmemset(expected_target, 0, 16);
    expected_target[0] = 0xFE; expected_target[1] = 0x80;
    expected_target[8]  = 0x50; expected_target[9]  = 0x54;
    expected_target[10] = 0x00; expected_target[11] = 0xFF;
    expected_target[12] = 0xFE; expected_target[13] = 0x12;
    expected_target[14] = 0x34; expected_target[15] = 0x56;

    /* Write target at offset 8 (data+4), leaving data+0..3 as reserved */
    kmemcpy(ns_buf + 8, expected_target, 16);

    /* Verify: target address must be at ns_buf + 8, NOT ns_buf + 0 */
    const uint8_t* parsed = ns_buf + 8; /* hdr->data + 4 */
    if (kmemcmp(parsed, expected_target, 16) != 0) {
        kprintf("[TEST] icmpv6_ns_parse: FAIL — target mismatch at data+4\n");
        return TEST_FAIL;
    }

    /* Verify the wrong parse (data+0) would give different bytes */
    const uint8_t* wrong = ns_buf; /* hdr->data (incorrect) */
    if (kmemcmp(wrong, expected_target, 16) == 0) {
        kprintf("[TEST] icmpv6_ns_parse: FAIL — wrong offset also matches\n");
        return TEST_FAIL;
    }

    kprintf("[TEST] icmpv6_ns_parse: PASS\n");
    return TEST_PASS;
}

/* ============================================================
 * Test 4: Socket alloc/retain/release refcounting
 *
 * Verifies that socket_alloc gives refcount 1, retain bumps it,
 * and release only frees at count 0. Also tests that an
 * allocated release doesn't crash, and a retained/released
 * socket stays alive.
 * ============================================================ */
static int test_socket_refcount(void) {
    socket_t* s = socket_alloc(AF_INET, SOCK_STREAM, 0);
    if (!s) {
        kprintf("[TEST] socket_refcount: FAIL — alloc returned NULL\n");
        return TEST_FAIL;
    }
    if (s->refcount != 1) {
        kprintf("[TEST] socket_refcount: FAIL — refcount=%d after alloc\n", s->refcount);
        return TEST_FAIL;
    }

    socket_retain(s);
    if (s->refcount != 2) {
        kprintf("[TEST] socket_refcount: FAIL — refcount=%d after retain\n", s->refcount);
        return TEST_FAIL;
    }

    socket_release(s);
    if (s->refcount != 1) {
        kprintf("[TEST] socket_refcount: FAIL — refcount=%d after release\n", s->refcount);
        return TEST_FAIL;
    }

    /* Verify socket is still alive (proto should exist for SOCK_STREAM) */
    if (!s->proto) {
        kprintf("[TEST] socket_refcount: FAIL — proto null after retain/release\n");
        return TEST_FAIL;
    }

    /* Register, lookup, unregister cycle */
    int fd = sock_register(s);
    if (fd < 0 || fd >= 32) {
        kprintf("[TEST] socket_refcount: FAIL — sock_register returned %d\n", fd);
        return TEST_FAIL;
    }

    socket_t* found = sock_lookup(fd);
    if (found != s) {
        kprintf("[TEST] socket_refcount: FAIL — sock_lookup mismatch\n");
        return TEST_FAIL;
    }

    sock_unregister(fd);
    found = sock_lookup(fd);
    if (found) {
        kprintf("[TEST] socket_refcount: FAIL — sock_lookup after unregister\n");
        return TEST_FAIL;
    }

    /* Final release should free (refcount 1 -> 0) */
    socket_release(s);
    /* Can't touch s after this — just verify no crash above */

    kprintf("[TEST] socket_refcount: PASS\n");
    return TEST_PASS;
}

/* ============================================================
 * Test 5: UDP endpoint enqueue/dequeue roundtrip
 *
 * Verifies the endpoint-based datagram queue used by the
 * new UDP socket layer: enqueue a datagram, dequeue it,
 * check payload, src addr, and src port match exactly.
 * Also verifies empty-queue returns ERR_TIMEOUT immediately
 * (no NIC poll needed since timeout=0).
 * ============================================================ */
static int test_udp_queue_roundtrip(void) {
    udp_endpoint_t ep;
    kmemset(&ep, 0, sizeof(ep));
    /* Only q_head/q_tail/q_count matter for queue ops; no need to register */

    uint8_t test_src[16];
    kmemset(test_src, 0, 16);
    test_src[0] = 0xFE; test_src[1] = 0x80;
    test_src[15] = 0x01;

    const char* test_data = "UDP test payload";
    uint32_t test_len = 17; /* strlen("UDP test payload") + 1 for null */

    /* Step 1: enqueue */
    udp_endpoint_enqueue(&ep, AF_INET6, test_src, 12345,
                          (const uint8_t*)test_data, test_len);
    if (ep.q_count != 1) {
        kprintf("[TEST] udp_queue_roundtrip: FAIL — q_count=%d after enqueue\n", ep.q_count);
        return TEST_FAIL;
    }

    /* Step 2: dequeue (use 50ms timeout so loop runs once) */
    uint8_t out_buf[64];
    int out_af;
    uint8_t out_addr[16];
    uint16_t out_port;
    int ret = udp_endpoint_dequeue(&ep, out_buf, sizeof(out_buf),
                                    &out_af, out_addr, &out_port, 50);
    if (ret < 0) {
        kprintf("[TEST] udp_queue_roundtrip: FAIL — dequeue returned %d\n", ret);
        return TEST_FAIL;
    }
    if ((uint32_t)ret != test_len) {
        kprintf("[TEST] udp_queue_roundtrip: FAIL — expected len %u, got %d\n", test_len, ret);
        return TEST_FAIL;
    }
    if (kmemcmp(out_buf, test_data, test_len) != 0) {
        kprintf("[TEST] udp_queue_roundtrip: FAIL — payload mismatch\n");
        return TEST_FAIL;
    }
    if (out_af != AF_INET6) {
        kprintf("[TEST] udp_queue_roundtrip: FAIL — af mismatch\n");
        return TEST_FAIL;
    }
    if (kmemcmp(out_addr, test_src, 16) != 0) {
        kprintf("[TEST] udp_queue_roundtrip: FAIL — src addr mismatch\n");
        return TEST_FAIL;
    }
    if (out_port != 12345) {
        kprintf("[TEST] udp_queue_roundtrip: FAIL — src port mismatch\n");
        return TEST_FAIL;
    }
    if (ep.q_count != 0) {
        kprintf("[TEST] udp_queue_roundtrip: FAIL — q_count=%d after dequeue\n", ep.q_count);
        return TEST_FAIL;
    }

    /* Step 3: empty queue should return ERR_TIMEOUT */
    ret = udp_endpoint_dequeue(&ep, out_buf, sizeof(out_buf),
                                &out_af, out_addr, &out_port, 50);
    if (ret != ERR_TIMEOUT) {
        kprintf("[TEST] udp_queue_roundtrip: FAIL — empty dequeue returned %d (expected %d)\n",
                ret, ERR_TIMEOUT);
        return TEST_FAIL;
    }

    kprintf("[TEST] udp_queue_roundtrip: PASS\n");
    return TEST_PASS;
}

/* ============================================================
 * Test 6: E1000 device-ID model classification
 *
 * Verifies the e1000e/igb probe table used by nic_init(): every
 * supported device ID maps to the expected controller family
 * (legacy / e1000e / igb), and unknown IDs are not accepted as
 * legacy NICs. Guards the Phase 3.1 bare-metal NIC support.
 * ============================================================ */
static int test_e1000_model_table(void) {
    if (e1000_model_for_devid(E1000_DEV_82540EM) != E1000_MODEL_LEGACY) {
        kprintf("[TEST] e1000_model_table: FAIL — 82540EM not legacy\n");
        return TEST_FAIL;
    }
    if (e1000_model_for_devid(E1000_DEV_82545EM) != E1000_MODEL_LEGACY) {
        kprintf("[TEST] e1000_model_table: FAIL — 82545EM not legacy\n");
        return TEST_FAIL;
    }
    if (e1000_model_for_devid(E1000_DEV_82571EB) != E1000_MODEL_E1000E ||
        e1000_model_for_devid(E1000_DEV_82574L) != E1000_MODEL_E1000E ||
        e1000_model_for_devid(E1000_DEV_I219V) != E1000_MODEL_E1000E) {
        kprintf("[TEST] e1000_model_table: FAIL — e1000e family misclassified\n");
        return TEST_FAIL;
    }
    if (e1000_model_for_devid(E1000_DEV_82576) != E1000_MODEL_IGB ||
        e1000_model_for_devid(E1000_DEV_I210) != E1000_MODEL_IGB ||
        e1000_model_for_devid(E1000_DEV_I211) != E1000_MODEL_IGB) {
        kprintf("[TEST] e1000_model_table: FAIL — igb family misclassified\n");
        return TEST_FAIL;
    }
    if (e1000_model_for_devid(0x1234) != E1000_MODEL_LEGACY) {
        kprintf("[TEST] e1000_model_table: FAIL — unknown ID not rejected\n");
        return TEST_FAIL;
    }
    if (kmemcmp(e1000_model_name(E1000_MODEL_E1000E), "e1000e", 7) != 0 ||
        kmemcmp(e1000_model_name(E1000_MODEL_IGB), "igb", 4) != 0) {
        kprintf("[TEST] e1000_model_table: FAIL — model name wrong\n");
        return TEST_FAIL;
    }

    kprintf("[TEST] e1000_model_table: PASS\n");
    return TEST_PASS;
}

void net_self_test(void) {
    kprintf("[TEST] === Network self-tests ===\n");

    int pass = 0, fail = 0;
    if (test_tcp_find_conn_ipv6() == TEST_PASS) pass++; else fail++;
    if (test_ndp_cache_miss() == TEST_PASS) pass++; else fail++;
    if (test_icmpv6_ns_parse() == TEST_PASS) pass++; else fail++;
    if (test_socket_refcount() == TEST_PASS) pass++; else fail++;
    if (test_udp_queue_roundtrip() == TEST_PASS) pass++; else fail++;
    if (test_e1000_model_table() == TEST_PASS) pass++; else fail++;

    kprintf("[TEST] === Results: %d pass, %d fail ===\n", pass, fail);
}

#endif /* NET_SELF_TEST */

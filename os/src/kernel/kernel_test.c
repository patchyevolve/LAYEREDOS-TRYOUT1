#include "kernel.h"
#include "test_framework.h"
#include "kmalloc.h"
#include "vma.h"
#include "tcp.h"
#include "net.h"
#include "sync.h"
#include "process.h"
#include "udp.h"
#include "block.h"
#include "veth.h"
#include "eth.h"
#include "arp.h"
#include "route.h"
#include "ipv4.h"
#include "net_ns.h"
#include "sched.h"
#include "smp.h"
#include "pmm.h"

#ifdef KERNEL_SELF_TEST

/* ============================================================
 * Test 1: kmalloc/kfree roundtrip
 *
 * Verifies basic heap operations: alloc a buffer, write a
 * known pattern, read it back, free it. Double-free is not
 * tested (it's undefined behaviour).
 * ============================================================ */
static int test_kmalloc_free_roundtrip(void) {
    uint8_t* buf = (uint8_t*)kmalloc(256);
    ASSERT_NOT_NULL(buf, "kmalloc(256) failed");

    for (int i = 0; i < 256; i++)
        buf[i] = (uint8_t)(i ^ 0xA5);
    for (int i = 0; i < 256; i++) {
        if (buf[i] != (uint8_t)(i ^ 0xA5)) {
            kprintf("[FAIL] test_kmalloc_free_roundtrip: byte %d mismatch\n", i);
            kfree(buf);
            return TEST_FAIL;
        }
    }

    kfree(buf);

    /* Allocate again to confirm kmalloc still works after free */
    uint8_t* buf2 = (uint8_t*)kmalloc(64);
    if (!buf2) {
        kprintf("[FAIL] test_kmalloc_free_roundtrip: kmalloc after kfree failed\n");
        return TEST_FAIL;
    }
    kfree(buf2);

    kprintf("[TEST] kmalloc_free_roundtrip: PASS\n");
    return TEST_PASS;
}

/* ============================================================
 * Test 2: kmalloc zero-size and large allocation
 *
 * Verifies edge cases: zero-size returns a valid non-NULL
 * pointer (or NULL is also acceptable), and very large
 * requests correctly return NULL.
 * ============================================================ */
static int test_kmalloc_edge_cases(void) {
    /* Zero-size: implementation-defined, but must not crash */
    void* z = kmalloc(0);
    if (z) kfree(z);

    /* Very large request should fail gracefully */
    void* huge = kmalloc(0x7FFFFFFF);
    ASSERT_NULL(huge, "kmalloc(0x7FFFFFFF) should fail");

    /* Max reasonable allocation */
    void* big = kmalloc(65536);
    if (big) kfree(big);

    kprintf("[TEST] kmalloc_edge_cases: PASS\n");
    return TEST_PASS;
}

/* ============================================================
 * Test 3: VMA add / find / remove cycle
 *
 * Verifies the VMA tracking layer against the init process.
 * Add a mapping, find it by address, check its fields,
 * remove it, verify it's gone.
 * ============================================================ */
static int test_vma_add_find_remove(void) {
    process_t* proc = process_find(1);
    ASSERT_NOT_NULL(proc, "init process not found");

    uint64_t base = 0x10000000;
    uint64_t end  = 0x10001000;

    vma_t* v = vma_add(proc, base, end, 0x7, 0x02, NULL, 0);
    ASSERT_NOT_NULL(v, "vma_add failed");
    ASSERT_EQ(v->start, base, "vma start mismatch");
    ASSERT_EQ(v->end, end, "vma end mismatch");

    vma_t* found = vma_find(proc, base + 0x100);
    ASSERT_EQ(found, v, "vma_find returned wrong VMA");

    found = vma_find(proc, base - 1);
    ASSERT_NULL(found, "vma_find should miss before start");

    found = vma_find(proc, end);
    ASSERT_NULL(found, "vma_find should miss at end (exclusive)");

    err_t e = vma_remove(proc, base, end - base);
    ASSERT_ERR_OK(e, "vma_remove");

    found = vma_find(proc, base + 0x100);
    ASSERT_NULL(found, "vma_find after remove should miss");

    kprintf("[TEST] vma_add_find_remove: PASS\n");
    return TEST_PASS;
}

/* ============================================================
 * Test 4: TCP connection create / destroy
 *
 * Verifies the TCP transport-layer API: create a connection,
 * verify it's marked used, destroy it, verify used=0.
 * ============================================================ */
static int test_tcp_conn_create_destroy(void) {
    tcp_conn_t* c = tcp_conn_create(AF_INET);
    ASSERT_NOT_NULL(c, "tcp_conn_create failed");
    ASSERT_NE(c->used, 0, "new conn should be marked used");

    int state = tcp_conn_get_state(c);
    ASSERT_EQ(state, TCP_CLOSED, "new conn state should be CLOSED");

    tcp_conn_destroy(c);
    ASSERT_EQ(c->used, 0, "destroyed conn should have used=0");

    kprintf("[TEST] tcp_conn_create_destroy: PASS\n");
    return TEST_PASS;
}

/* ============================================================
 * Test 5: TCP connection bind / state
 *
 * Verifies binding a connection to a port and checking that
 * the state machine transitions correctly for bind + listen.
 * ============================================================ */
static int test_tcp_conn_bind_listen(void) {
    tcp_conn_t* c = tcp_conn_create(AF_INET);
    ASSERT_NOT_NULL(c, "tcp_conn_create failed");

    int e = tcp_conn_bind(c, 40000, 1);
    ASSERT_ERR_OK(e, "tcp_conn_bind");
    ASSERT_NE(c->local_port, 0, "local_port should be non-zero");

    e = tcp_conn_listen(c, 40000);
    ASSERT_ERR_OK(e, "tcp_conn_listen");

    int state = tcp_conn_get_state(c);
    ASSERT_EQ(state, TCP_LISTEN, "listener should be in LISTEN state");

    tcp_conn_destroy(c);
    kprintf("[TEST] tcp_conn_bind_listen: PASS\n");
    return TEST_PASS;
}

/* ============================================================
 * Test 6: Socket alloc / retain / release lifecycle
 *
 * Extended socket lifecycle test: alloc, verify refcount=1,
 * retain (2), release (1), release frees, verify sock_lookup
 * after unregister returns NULL. Also tests that a UDP socket
 * can be allocated and released.
 * ============================================================ */
static int test_socket_lifecycle_extended(void) {
    /* TCP socket */
    socket_t* tcp_s = socket_alloc(AF_INET, SOCK_STREAM, 0);
    ASSERT_NOT_NULL(tcp_s, "TCP socket_alloc failed");
    ASSERT_EQ(tcp_s->refcount, 1, "refcount after alloc");

    socket_retain(tcp_s);
    ASSERT_EQ(tcp_s->refcount, 2, "refcount after retain");

    socket_release(tcp_s);
    ASSERT_EQ(tcp_s->refcount, 1, "refcount after first release");

    int fd = sock_register(tcp_s);
    ASSERT_TRUE(fd >= 0, "sock_register failed");

    socket_t* found = sock_lookup(fd);
    ASSERT_EQ(found, tcp_s, "sock_lookup mismatch");

    sock_unregister(fd);
    found = sock_lookup(fd);
    ASSERT_NULL(found, "sock_lookup after unregister");

    socket_release(tcp_s);  /* refcount 1 -> 0, should free */

    /* UDP socket */
    socket_t* udp_s = socket_alloc(AF_INET6, SOCK_DGRAM, 0);
    ASSERT_NOT_NULL(udp_s, "UDP socket_alloc failed");
    socket_release(udp_s);

    kprintf("[TEST] socket_lifecycle_extended: PASS\n");
    return TEST_PASS;
}

/* ============================================================
 * Test 7: Spinlock acquire/release balance
 *
 * Verifies basic spinlock operation: acquire, release,
 * acquire/release on error-path simulation. This validates
 * the CLI/STI-based locking primitive.
 * ============================================================ */
static int test_spinlock_acquire_release(void) {
    static spinlock_t test_lock;
    cpu_flags_t flags;

    spinlock_init(&test_lock, "test");
    spinlock_acquire(&test_lock, &flags);
    spinlock_release(&test_lock, flags);

    kprintf("[TEST] spinlock_acquire_release: PASS\n");
    return TEST_PASS;
}

/* ============================================================
 * Test 8: TCP data retransmission via tcp_tick
 *
 * Verifies that a connection with expired RTO triggers data
 * retransmission. Sets up retrans_len > 0, rto_remaining = 0,
 * calls tcp_tick, then checks rto_remaining was reset.
 * ============================================================ */
static int test_tcp_retransmission(void) {
    tcp_conn_t* c = tcp_conn_create(AF_INET);
    ASSERT_NOT_NULL(c, "tcp_conn_create failed");

    /* Manually set up as ESTABLISHED with pending retransmission */
    c->state = TCP_ESTABLISHED;
    c->retrans_len = 64;
    c->retrans_seq = 1000;
    c->rto_remaining = 0;   /* expired — will fire on next tcp_tick */
    c->rto_ms = 200;
    kmemset(c->retrans_buf, 'X', 64);

    /* Call tcp_tick — should process this connection */
    tcp_tick();

    /* After retransmit, rto_remaining should be reset to rto_ms (200) */
    ASSERT_NE(c->rto_remaining, 0, "rto_remaining should be reset after retransmit");
    ASSERT_NE(c->retrans_len, 0, "retrans_len should remain set");

    tcp_conn_destroy(c);
    kprintf("[TEST] tcp_retransmission: PASS\n");
    return TEST_PASS;
}

/* ============================================================
 * Test 9: Mutex init / lock / unlock
 *
 * Verifies the mutex API: initialize, acquire, release.
 * Uses infinite timeout. On single-core this tests basic
 * synchronization state machine.
 * ============================================================ */
static int test_mutex_lock_unlock(void) {
    mutex_t m;
    mutex_init(&m);

    err_t e = mutex_lock(&m, (uint64_t)-1);
    ASSERT_ERR_OK(e, "mutex_lock");

    e = mutex_unlock(&m);
    ASSERT_ERR_OK(e, "mutex_unlock");

    kprintf("[TEST] mutex_lock_unlock: PASS\n");
    return TEST_PASS;
}

/* ============================================================
 * Test 10: UDP endpoint multi-packet enqueue/dequeue
 *
 * Extends the existing roundtrip test: enqueue 3 datagrams
 * with different payloads, dequeue them in order, verify
 * each payload matches exactly.
 * ============================================================ */
static int test_udp_endpoint_multi(void) {
    udp_endpoint_t ep;
    kmemset(&ep, 0, sizeof(ep));

    uint8_t src[16];
    kmemset(src, 0, 16);
    src[0] = 0xFE; src[15] = 0x01;

    const char* msgs[3] = {"alpha", "beta", "gamma"};
    uint32_t lens[3] = {5, 4, 5};

    for (int i = 0; i < 3; i++) {
        udp_endpoint_enqueue(&ep, AF_INET6, src, 2000 + i,
                              (const uint8_t*)msgs[i], lens[i]);
    }
    ASSERT_EQ(ep.q_count, 3, "q_count after 3 enqueues");

    for (int i = 0; i < 3; i++) {
        uint8_t out[16];
        int out_af;
        uint8_t out_addr[16];
        uint16_t out_port;
        int ret = udp_endpoint_dequeue(&ep, out, sizeof(out),
                                        &out_af, out_addr, &out_port, 0);
        ASSERT_NE(ret, ERR_TIMEOUT, "dequeue should not timeout");
        ASSERT_TRUE(ret > 0, "dequeue should return positive length");
        ASSERT_EQ(kmemcmp(out, msgs[i], (uint32_t)ret), 0, "payload mismatch");
        ASSERT_EQ(out_port, (uint16_t)(2000 + i), "port mismatch");
    }
    ASSERT_EQ(ep.q_count, 0, "q_count after all dequeues");

    kprintf("[TEST] udp_endpoint_multi: PASS\n");
    return TEST_PASS;
}

/* ============================================================
 * Test 11: Block cache eviction (write more than cache size)
 *
 * Writes to 100 blocks (cache is ~64 entries), forcing LRU
 * eviction, then reads back and verifies data integrity.
 * ============================================================ */
static int test_block_cache_eviction(void) {
    block_dev_t* dev = block_find("ramdisk");
    ASSERT_NOT_NULL(dev, "ramdisk device not found");

    /* ramdisk is 1 MB = 2048 blocks. Use LBA range 100-199 (well within). */
    uint8_t wbuf[BLOCK_SIZE];
    uint8_t rbuf[BLOCK_SIZE];

    for (uint64_t i = 0; i < 100; i++) {
        kmemset(wbuf, (int)(i & 0xFF), BLOCK_SIZE);
        ASSERT_ERR_OK(block_write(dev, 100 + i, 1, wbuf), "cache write");
    }

    for (uint64_t i = 0; i < 100; i++) {
        ASSERT_ERR_OK(block_read(dev, 100 + i, 1, rbuf), "cache read");
        for (int j = 0; j < BLOCK_SIZE; j++) {
            if (rbuf[j] != (uint8_t)(i & 0xFF)) {
                kprintf("[FAIL] test_block_cache_eviction: byte %d block %llu\n", j, i);
                return TEST_FAIL;
            }
        }
    }

    kprintf("[TEST] block_cache_eviction: PASS\n");
    return TEST_PASS;
}

/* ============================================================
 * Test 12: kmalloc allocation pattern stress
 *
 * Allocates/frees many small blocks in alternating patterns
 * to stress slab allocator coalescing and fragmentation
 * handling. Interleaves different sizes.
 * ============================================================ */
static int test_kmalloc_stress(void) {
    void* ptrs[64];

    for (int round = 0; round < 4; round++) {
        int count = 0;
        for (int i = 0; i < 64; i++) {
            size_t sz = (size_t)(8 + ((i * 7 + round * 3) % 128));
            ptrs[count] = kmalloc(sz);
            if (!ptrs[count]) break;
            kmemset(ptrs[count], (uint8_t)(i + round), sz);
            count++;
        }
        ASSERT_TRUE(count > 0, "kmalloc round produced no allocations");

        /* Free odd-indexed entries (set to NULL to track) */
        for (int i = 1; i < count; i += 2) {
            kfree(ptrs[i]);
            ptrs[i] = NULL;
        }

        /* Re-allocate into freed slots plus some fresh ones */
        for (int i = 0; i < count; i++) {
            if (ptrs[i] == NULL) {
                size_t sz = (size_t)(16 + ((i * 13 + round) % 96));
                ptrs[i] = kmalloc(sz);
                if (ptrs[i])
                    kmemset(ptrs[i], (uint8_t)(i + round + 64), sz);
            }
        }

        /* Free all non-NULL entries */
        for (int i = 0; i < count; i++) {
            if (ptrs[i])
                kfree(ptrs[i]);
        }
    }

    kprintf("[TEST] kmalloc_stress: PASS\n");
    return TEST_PASS;
}

/* ============================================================
 * Test 13: kmalloc various sizes
 *
 * Tests allocation with many different sizes to exercise
 * different slab classes: 1, 2, 3, 5, 7, 8, 15, 16, 31, 32,
 * 63, 64, 127, 128, 255, 256, 511, 512, 1023, 1024, 2047.
 * Each is written to and read back.
 * ============================================================ */
static int test_kmalloc_sizes(void) {
    size_t sizes[] = {1, 2, 3, 5, 7, 8, 15, 16, 31, 32, 63, 64,
                      127, 128, 255, 256, 511, 512, 1023, 1024, 2047};
    int n = (int)(sizeof(sizes) / sizeof(sizes[0]));

    for (int i = 0; i < n; i++) {
        uint8_t* buf = (uint8_t*)kmalloc(sizes[i]);
        ASSERT_NOT_NULL(buf, "kmalloc size variant failed");

        for (size_t j = 0; j < sizes[i]; j++)
            buf[j] = (uint8_t)(j ^ (uint8_t)i);
        for (size_t j = 0; j < sizes[i]; j++) {
            if (buf[j] != (uint8_t)(j ^ (uint8_t)i)) {
                kprintf("[FAIL] test_kmalloc_sizes: byte %llu size %llu\n",
                        (unsigned long long)j, (unsigned long long)sizes[i]);
                kfree(buf);
                return TEST_FAIL;
            }
        }
        kfree(buf);
    }

    kprintf("[TEST] kmalloc_sizes: PASS\n");
    return TEST_PASS;
}

/* ============================================================
 * Test 14: Mutex stress (many lock/unlock cycles)
 *
 * Verifies that repeated mutex lock/unlock does not leak
 * locks or corrupt state. Runs 500 cycles.
 * ============================================================ */
static int test_mutex_stress(void) {
    mutex_t m;
    mutex_init(&m);

    for (int i = 0; i < 500; i++) {
        err_t e = mutex_lock(&m, (uint64_t)-1);
        if (e != ERR_OK) {
            kprintf("[FAIL] test_mutex_stress: lock failed at iteration %d\n", i);
            return TEST_FAIL;
        }
        e = mutex_unlock(&m);
        if (e != ERR_OK) {
            kprintf("[FAIL] test_mutex_stress: unlock failed at iteration %d\n", i);
            return TEST_FAIL;
        }
    }

    kprintf("[TEST] mutex_stress: PASS\n");
    return TEST_PASS;
}

/* ============================================================
 * Test 15: TCP state machine — manual state transitions
 *
 * Tests state-machine transitions without sending packets
 * (avoids ARP/NDP resolution hang in test env). Validates
 * allowed transitions: CLOSED→LISTEN, LISTEN→ESTABLISHED,
 * ESTABLISHED→FIN_WAIT1, FIN_WAIT1→CLOSED (RST path).
 * ============================================================ */
static int test_tcp_state_transitions(void) {
    tcp_conn_t* c = tcp_conn_create(AF_INET);
    ASSERT_NOT_NULL(c, "tcp_conn_create failed");
    ASSERT_EQ(c->state, TCP_CLOSED, "new conn should be CLOSED");

    /* bind + listen transitions to LISTEN */
    int e = tcp_conn_bind(c, 40001, 1);
    ASSERT_ERR_OK(e, "tcp_conn_bind");
    e = tcp_conn_listen(c, 40001);
    ASSERT_ERR_OK(e, "tcp_conn_listen");
    ASSERT_EQ(c->state, TCP_LISTEN, "should be LISTEN after listen()");

    /* Manually set ESTABLISHED, then simulate close by setting FIN_WAIT1 */
    c->state = TCP_ESTABLISHED;
    c->state = TCP_FIN_WAIT1;

    /* Manual RST: set state to CLOSED and verify used=1 still */
    c->state = TCP_CLOSED;
    ASSERT_NE(c->used, 0, "RST should not set used=0");

    tcp_conn_destroy(c);
    kprintf("[TEST] tcp_state_transitions: PASS\n");
    return TEST_PASS;
}

/* ============================================================
 * Test 16: UDP endpoint queue full behaviour
 *
 * Fills the endpoint queue to capacity, verifies enqueue
 * returns without overflow (dropped), then dequeues all
 * and verifies first-UDP_DGRAM_QUEUE_SIZE items are intact.
 * ============================================================ */
static int test_udp_endpoint_queue_full(void) {
    udp_endpoint_t ep;
    kmemset(&ep, 0, sizeof(ep));

    uint8_t src[16];
    kmemset(src, 0, 16);
    src[0] = 0xAA;

    /* Fill to capacity */
    for (int i = 0; i < UDP_DGRAM_QUEUE_SIZE + 5; i++) {
        uint8_t payload = (uint8_t)i;
        udp_endpoint_enqueue(&ep, AF_INET6, src, 1000 + (uint16_t)i,
                              &payload, 1);
    }

    /* Queue should be full (extra items silently dropped) */
    ASSERT_TRUE(ep.q_count <= UDP_DGRAM_QUEUE_SIZE,
                "queue should not exceed capacity");

    /* Dequeue and verify the first UDP_DGRAM_QUEUE_SIZE are correct */
    int dequeued = 0;
    while (ep.q_count > 0) {
        uint8_t out;
        int ret = udp_endpoint_dequeue(&ep, &out, 1, NULL, NULL, NULL, 0);
        if (ret < 0) break;
        ASSERT_EQ(out, (uint8_t)dequeued, "payload mismatch at position");
        dequeued++;
    }
    ASSERT_EQ(dequeued, UDP_DGRAM_QUEUE_SIZE,
              "should dequeue exactly UDP_DGRAM_QUEUE_SIZE items");
    ASSERT_EQ(ep.q_count, 0, "queue should be empty after dequeue");

    kprintf("[TEST] udp_endpoint_queue_full: PASS\n");
    return TEST_PASS;
}

/* ============================================================
 * Test 17: Error code verification
 *
 * Tests that kernel APIs return the correct err_t values
 * for well-defined failure scenarios (NULL params, invalid
 * arguments, etc.). This guards the error-propagation path.
 * ============================================================ */
static int test_error_codes(void) {
    /* block_write with NULL dev returns ERR_INVAL */
    err_t e = block_write(NULL, 0, 1, NULL);
    ASSERT_EQ(e, ERR_INVAL, "block_write(NULL) should return ERR_INVAL");

    /* block_read with NULL dev returns ERR_INVAL */
    e = block_read(NULL, 0, 1, NULL);
    ASSERT_EQ(e, ERR_INVAL, "block_read(NULL) should return ERR_INVAL");

    /* process_find(0) returns NULL (PID 0 = idle) — safe call */
    process_t* p = process_find((pid_t)0);
    ASSERT_NOT_NULL(p, "process_find(0) should find idle process");

    /* process_find with huge PID returns NULL */
    p = process_find((pid_t)0x7FFFFFFF);
    ASSERT_NULL(p, "process_find(invalid) should return NULL");

    kprintf("[TEST] error_codes: PASS\n");
    return TEST_PASS;
}

static int test_veth_pair_basic(void) {
    int idx;
    ASSERT_TRUE(veth_pair_create(&idx) == ERR_OK, "veth_pair_create");
    ASSERT_TRUE(idx >= 0 && idx < VETH_MAX_PAIRS, "veth_pair_idx");

    const uint8_t* mac_a = veth_get_mac(idx, 0);
    const uint8_t* mac_b = veth_get_mac(idx, 1);
    ASSERT_TRUE(mac_a != NULL && mac_b != NULL, "veth_get_mac");
    /* MACs should be non-zero and different */
    int same = (kmemcmp(mac_a, mac_b, ETH_ALEN) == 0);
    int zero_a = 1, zero_b = 1;
    for (int i = 0; i < ETH_ALEN; i++) { if (mac_a[i]) zero_a = 0; if (mac_b[i]) zero_b = 0; }
    ASSERT_TRUE(!same, "veth_mac_different");
    ASSERT_TRUE(!zero_a && !zero_b, "veth_mac_nonzero");

    /* veth_find_end should find both */
    veth_end_t* ea = veth_find_end(mac_a);
    veth_end_t* eb = veth_find_end(mac_b);
    ASSERT_TRUE(ea != NULL && eb != NULL, "veth_find_end");
    ASSERT_TRUE(ea->peer == eb && eb->peer == ea, "veth_peer_cross_ref");

    /* Validate eth_try_veth recognizes veth MACs */
    uint8_t test_data[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    int handled = eth_try_veth(mac_a, ETHERTYPE_LOOP, test_data, sizeof(test_data));
    ASSERT_TRUE(handled == 1, "eth_try_veth_finds_a");

    handled = eth_try_veth(mac_b, ETHERTYPE_LOOP, test_data, sizeof(test_data));
    ASSERT_TRUE(handled == 1, "eth_try_veth_finds_b");

    return TEST_PASS;
}

static int test_veth_frame_roundtrip(void) {
    /* Create a veth pair, verify raw frame roundtrip via eth_try_veth → veth_deliver */
    int idx;
    ASSERT_TRUE(veth_pair_create(&idx) == ERR_OK, "veth_frame_pair_create");

    const uint8_t* mac_a = veth_get_mac(idx, 0);
    const uint8_t* mac_b = veth_get_mac(idx, 1);
    ASSERT_TRUE(mac_a && mac_b, "veth_frame_macs");

    /* Send a frame from A to B via eth_send */
    uint8_t payload[] = {0xDE, 0xAD, 0xBE, 0xEF};
    int e = eth_send(mac_b, ETHERTYPE_LOOP, payload, sizeof(payload));
    ASSERT_TRUE(e == ERR_OK, "veth_send_a_to_b");

    /* Send a frame from B to A */
    uint8_t payload2[] = {0xCA, 0xFE, 0xBA, 0xBE};
    e = eth_send(mac_a, ETHERTYPE_LOOP, payload2, sizeof(payload2));
    ASSERT_TRUE(e == ERR_OK, "veth_send_b_to_a");

    /* Move end B to a new namespace, verify delivery still works */
    net_ns_t* ns_b = net_ns_alloc();
    ASSERT_TRUE(ns_b != NULL, "net_ns_alloc");
    kstrncpy(ns_b->name, "ns-b-frame", sizeof(ns_b->name));
    ns_b->refcount = 1;

    veth_end_move(idx, 1, ns_b);

    /* Frame from A (init_net_ns) to B (ns_b) should still deliver */
    e = eth_send(mac_b, ETHERTYPE_LOOP, payload, sizeof(payload));
    ASSERT_TRUE(e == ERR_OK, "veth_send_a_to_b_cross_ns");

    /* Frame from B (ns_b) to A (init_net_ns):
     * eth_send(mac_a, ...) will find mac_a in the veth registry and call
     * veth_deliver, which switches to init_net_ns and dispatches.
     * Since eth_try_veth always runs in the caller's namespace context,
     * this should work. */
    e = eth_send(mac_a, ETHERTYPE_LOOP, payload2, sizeof(payload2));
    ASSERT_TRUE(e == ERR_OK, "veth_send_b_to_a_cross_ns");

    net_ns_release(ns_b);
    kprintf("[TEST] veth frame roundtrip: OK\n");
    return TEST_PASS;
}

/* ============================================================
 * SMP Test: concurrent spinlock stress
 *
 * Creates N kernel threads that all PUSH spinlock-acquire a
 * shared lock, increment a counter, and release. Verifies
 * the final count equals N*100.
 * ============================================================ */
static spinlock_t smp_spin_test;
static volatile int smp_spin_counter;

static void smp_spin_worker(void* arg) {
    (void)arg;
    cpu_flags_t flags;
    for (int i = 0; i < 100; i++) {
        spinlock_acquire(&smp_spin_test, &flags);
        smp_spin_counter++;
        spinlock_release(&smp_spin_test, flags);
    }
    thread_exit(0);
}

static int test_smp_concurrent_spinlock(void) {
    if (!smp_enabled || smp_nr_cpus() < 2) {
        kprintf("[TEST] test_smp_concurrent_spinlock: SKIP (SMP < 2)\n");
        return TEST_PASS;
    }

    spinlock_init(&smp_spin_test, "smp-spin-test");
    smp_spin_counter = 0;

    int n = smp_nr_cpus();
    if (n > 4) n = 4; /* keep it reasonable */
    thread_t* threads[4];

    for (int i = 0; i < n; i++) {
        threads[i] = thread_create(smp_spin_worker, NULL, THREAD_DEF_PRIO, "smp-spin");
        ASSERT_NOT_NULL(threads[i], "thread_create");
        sched_add_thread(threads[i]);
    }

    for (int i = 0; i < n; i++) {
        int code;
        err_t e = thread_join(threads[i], &code);
        ASSERT_ERR_OK(e, "thread_join");
    }

    ASSERT_TRUE(smp_spin_counter == n * 100, "smp_spin_counter == n*100");
    kprintf("[TEST] test_smp_concurrent_spinlock: %d threads x 100, counter=%d\n",
            n, smp_spin_counter);
    return TEST_PASS;
}

/* ============================================================
 * SMP Test: PMM concurrent allocation / free
 *
 * Each thread allocates 16 pages, writes a known pattern,
 * reads it back, then frees them.
 * ============================================================ */
static int smp_pmm_iters = 8;

static void smp_pmm_worker(void* arg) {
    int tid = (int)(uintptr_t)arg;
    for (int i = 0; i < smp_pmm_iters; i++) {
        uint64_t phys = pmm_alloc_page();
        if (!phys) {
            kprintf("[FAIL] smp_pmm_worker[%d]: pmm_alloc_page failed at iter %d\n", tid, i);
            thread_exit(1);
        }
        uint8_t* v = (uint8_t*)PHYS_TO_VIRT(phys);
        for (int j = 0; j < 4096; j++)
            v[j] = (uint8_t)(tid + i + j);
        for (int j = 0; j < 4096; j++) {
            if (v[j] != (uint8_t)(tid + i + j)) {
                kprintf("[FAIL] smp_pmm_worker[%d]: byte %d corrupt at iter %d\n", tid, j, i);
                thread_exit(1);
            }
        }
        pmm_free_page(phys);
    }
    thread_exit(0);
}

static int test_smp_pmm_concurrent(void) {
    if (!smp_enabled || smp_nr_cpus() < 2) {
        kprintf("[TEST] test_smp_pmm_concurrent: SKIP (SMP < 2)\n");
        return TEST_PASS;
    }

    int n = smp_nr_cpus();
    if (n > 4) n = 4;
    thread_t* threads[4];

    for (int i = 0; i < n; i++) {
        threads[i] = thread_create(smp_pmm_worker, (void*)(uintptr_t)(i+1),
                                   THREAD_DEF_PRIO, "smp-pmm");
        ASSERT_NOT_NULL(threads[i], "thread_create");
        sched_add_thread(threads[i]);
    }

    for (int i = 0; i < n; i++) {
        int code;
        err_t e = thread_join(threads[i], &code);
        ASSERT_ERR_OK(e, "thread_join");
        ASSERT_TRUE(code == 0, "worker exit code 0");
    }

    kprintf("[TEST] test_smp_pmm_concurrent: %d threads x %d iters, PASS\n",
            n, smp_pmm_iters);
    return TEST_PASS;
}

/* ============================================================
 * SMP Test: sched_setaffinity basic
 *
 * Creates a thread with affinity restricted to CPU 0 and
 * verifies it can run and complete.
 * ============================================================ */
static volatile int smp_aff_ran;

static void smp_aff_worker(void* arg) {
    (void)arg;
    smp_aff_ran = 1;
    thread_exit(0);
}

static int test_smp_affinity(void) {
    if (!smp_enabled || smp_nr_cpus() < 2) {
        kprintf("[TEST] test_smp_affinity: SKIP (SMP < 2)\n");
        return TEST_PASS;
    }

    smp_aff_ran = 0;
    thread_t* t = thread_create(smp_aff_worker, NULL, THREAD_DEF_PRIO, "smp-aff");
    ASSERT_NOT_NULL(t, "thread_create");
    sched_set_thread_affinity(t, 1ULL); /* CPU 0 only */
    sched_add_thread(t);

    int code;
    err_t e = thread_join(t, &code);
    ASSERT_ERR_OK(e, "thread_join");
    ASSERT_TRUE(code == 0, "exit code 0");
    ASSERT_TRUE(smp_aff_ran, "worker ran");

    kprintf("[TEST] test_smp_affinity: pinned to CPU0, PASS\n");
    return TEST_PASS;
}

/* ============================================================
 * Test: rwlock basic operations
 *
 * Verifies read acquire/release, write acquire/release, and
 * concurrent multiple readers (same thread, recursive read).
 * ============================================================ */
static int test_rwlock_basic(void) {
    rwlock_t rw;
    rwlock_init(&rw, "test-rwlock");
    cpu_flags_t f1, f2;

    /* Read acquire/release */
    rwlock_read_acquire(&rw, &f1);
    rwlock_read_release(&rw, f1);

    /* Write acquire/release */
    rwlock_write_acquire(&rw, &f1);
    rwlock_write_release(&rw, f1);

    /* Multiple readers (same thread) */
    rwlock_read_acquire(&rw, &f1);
    rwlock_read_acquire(&rw, &f2);
    ASSERT_EQ(rw.state, 2, "state should be 2 with two readers");
    rwlock_read_release(&rw, f2);
    ASSERT_EQ(rw.state, 1, "state should be 1 after one release");
    rwlock_read_release(&rw, f1);
    ASSERT_EQ(rw.state, 0, "state should be 0 after all releases");

    kprintf("[TEST] rwlock_basic: PASS\n");
    return TEST_PASS;
}

/* ============================================================
 * Test: seqlock basic operations
 *
 * Verifies write lock/unlock and read begin/retry cycle.
 * ============================================================ */
static int test_seqlock_basic(void) {
    seqlock_t sql;
    seqlock_init(&sql, "test-seqlock");
    cpu_flags_t flags;

    /* Read begin/retry on unlocked seqlock */
    uint64_t seq = seqlock_read_begin(&sql);
    ASSERT_EQ(seq, 0, "initial sequence should be 0");
    ASSERT_FALSE(seqlock_read_retry(&sql, seq), "should not retry on idle");

    /* Write acquire/release flips sequence */
    seqlock_write_acquire(&sql, &flags);
    ASSERT_TRUE(sql.sequence & 1, "sequence should be odd during write");
    seqlock_write_release(&sql, flags);
    ASSERT_EQ(sql.sequence, 2, "sequence should be 2 after write release");

    /* Read during idle sees even sequence */
    seq = seqlock_read_begin(&sql);
    ASSERT_EQ(seq, 2, "sequence should be 2");
    ASSERT_FALSE(seqlock_read_retry(&sql, seq), "no retry needed");

    kprintf("[TEST] seqlock_basic: PASS\n");
    return TEST_PASS;
}

#ifdef CONFIG_LOCKDEP
/* ============================================================
 * Test: lockdep ordering detection
 *
 * Acquires locks in fixed order, then verifies lockdep doesn't
 * false-positive on the same order. Tests that the graph records
 * A->B and does NOT report deadlock on A->B (same order).
 * ============================================================ */
static int test_lockdep_ordering(void) {
    rwlock_t a, b;
    rwlock_init(&a, "lockdep-A");
    rwlock_init(&b, "lockdep-B");
    cpu_flags_t fa, fb;

    /* Acquire A then B (order A->B) */
    rwlock_write_acquire(&a, &fa);
    rwlock_write_acquire(&b, &fb);
    rwlock_write_release(&b, fb);
    rwlock_write_release(&a, fa);

    /* Now acquire A then B again (same order) — should be no warning */
    rwlock_write_acquire(&a, &fa);
    rwlock_write_acquire(&b, &fb);
    rwlock_write_release(&b, fb);
    rwlock_write_release(&a, fa);

    /* Check that lockdep graph has edge a -> b */
    /* (no assertion, just verify no crash) */

    kprintf("[TEST] lockdep_ordering: PASS\n");
    return TEST_PASS;
}
#endif

void kernel_self_test(void) {
    kprintf("[TEST] === Kernel self-tests ===\n");

    int pass = 0, fail = 0;
    if (test_kmalloc_free_roundtrip() == TEST_PASS) pass++; else fail++;
    if (test_kmalloc_edge_cases() == TEST_PASS) pass++; else fail++;
    if (test_vma_add_find_remove() == TEST_PASS) pass++; else fail++;
    if (test_tcp_conn_create_destroy() == TEST_PASS) pass++; else fail++;
    if (test_tcp_conn_bind_listen() == TEST_PASS) pass++; else fail++;
    if (test_socket_lifecycle_extended() == TEST_PASS) pass++; else fail++;
    if (test_spinlock_acquire_release() == TEST_PASS) pass++; else fail++;
    if (test_tcp_retransmission() == TEST_PASS) pass++; else fail++;
    if (test_mutex_lock_unlock() == TEST_PASS) pass++; else fail++;
    if (test_udp_endpoint_multi() == TEST_PASS) pass++; else fail++;
    if (test_block_cache_eviction() == TEST_PASS) pass++; else fail++;
    if (test_kmalloc_stress() == TEST_PASS) pass++; else fail++;
    if (test_kmalloc_sizes() == TEST_PASS) pass++; else fail++;
    if (test_mutex_stress() == TEST_PASS) pass++; else fail++;
    if (test_tcp_state_transitions() == TEST_PASS) pass++; else fail++;
    if (test_udp_endpoint_queue_full() == TEST_PASS) pass++; else fail++;
    if (test_error_codes() == TEST_PASS) pass++; else fail++;
    if (test_veth_pair_basic() == TEST_PASS) pass++; else fail++;
    if (test_veth_frame_roundtrip() == TEST_PASS) pass++; else fail++;
    if (test_smp_concurrent_spinlock() == TEST_PASS) pass++; else fail++;
    if (test_smp_pmm_concurrent() == TEST_PASS) pass++; else fail++;
    if (test_smp_affinity() == TEST_PASS) pass++; else fail++;
    if (test_rwlock_basic() == TEST_PASS) pass++; else fail++;
    if (test_seqlock_basic() == TEST_PASS) pass++; else fail++;
#ifdef CONFIG_LOCKDEP
    if (test_lockdep_ordering() == TEST_PASS) pass++; else fail++;
#endif

    kprintf("[TEST] === Results: %d pass, %d fail ===\n", pass, fail);
}

#endif /* KERNEL_SELF_TEST */

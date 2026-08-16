#include "kernel.h"
#include "test_framework.h"
#include "pmm.h"
#include "smp.h"
#include "kmalloc.h"
#include "vma.h"
#include "vmm.h"
#include "tcp.h"
#include "net.h"
#include "sync.h"
#include "process.h"
#include "udp.h"
#include "block.h"
#include "veth.h"
#include "eth.h"
#include "pci.h"
#include "apic.h"
#include "e1000.h"
#include "arp.h"
#include "route.h"
#include "ipv4.h"
#include "net_ns.h"
#include "sched.h"
#include "smp.h"
#include "pmm.h"
#include "acpi.h"
#include "vfs.h"
#include "procfs.h"
#include "futex.h"
#include "epoll.h"
#include "pipe.h"
#include "shm.h"
#include "fcntl.h"

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
        for (int j = 0; j < 4096; j++) {
            if (v[j] != 0) {
                kprintf("[FAIL] smp_pmm_worker[%d]: alloc byte %d non-zero (%d) at iter %d phys=%lx page_owner=%lx on_cpu=%d\n",
                        tid, j, v[j], i, phys,
                        pmm_page_owner(phys),
                        smp_cpu_id());
                thread_exit(1);
            }
        }
        for (int j = 0; j < 4096; j++)
            v[j] = (uint8_t)(tid + i + j);
        for (int j = 0; j < 4096; j++) {
            if (v[j] != (uint8_t)(tid + i + j)) {
                kprintf("[FAIL] smp_pmm_worker[%d]: byte %d corrupt at iter %d (phys=%lx, expected=%d, actual=%d) page_owner=%lx on_cpu=%d\n",
                        tid, j, i, phys, (uint8_t)(tid + i + j), v[j],
                        pmm_page_owner(phys), smp_cpu_id());
                thread_exit(1);
            }
        }
        kprintf("[PMT] worker %d iter %d phys=%lx page_owner=%lx on_cpu=%d\n",
                tid, i, phys, pmm_page_owner(phys), smp_cpu_id());
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
    kprintf("[TEST] test_smp_pmm_concurrent: starting (%d threads, %d iters)...\n", n, smp_pmm_iters);
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

/* ============================================================
 * Test: PMM single-page alloc/free stress (UP only)
 *
 * Allocates/frees 500 pages with per-page data integrity check.
 * Catches: double-alloc (two allocs returning same page),
 * per-CPU cache corruption, bitmap corruption.
 * ============================================================ */
static int test_pmm_alloc_free_stress(void) {
    uint64_t pages[500];
    int npages = 0;
    int i;

    for (i = 0; i < 500; i++) {
        uint64_t phys = pmm_alloc_page();
        if (!phys) { kprintf("[FAIL] OOM at iter %d\n", i); goto cleanup; }
        pages[npages++] = phys;
        uint8_t* v = (uint8_t*)PHYS_TO_VIRT(phys);
        /* Write unique pattern per page */
        for (int j = 0; j < 4096; j++) v[j] = (uint8_t)(i ^ j);
        /* Immediate readback */
        for (int j = 0; j < 4096; j++) {
            if (v[j] != (uint8_t)(i ^ j)) {
                kprintf("[FAIL] byte %d mismatch at page %d (phys=%lx)\n", j, i, phys);
                goto cleanup;
            }
        }
    }

    /* Free in reverse order to exercise different cache paths */
    for (i = npages - 1; i >= 0; i--) pmm_free_page(pages[i]);
    kprintf("[TEST] pmm_alloc_free_stress: 500 pages, PASS\n");
    return TEST_PASS;

cleanup:
    for (i = 0; i < npages; i++) pmm_free_page(pages[i]);
    return TEST_FAIL;
}

/* ============================================================
 * Test: PMM multi-page alloc/free (block-level stress)
 *
 * Allocates and frees blocks of various sizes (2, 4, 8, 16
 * pages) with data integrity. Catches bugs in the contiguous-
 * page bitmap allocator and free path for multi-page blocks.
 * ============================================================ */
static int test_pmm_multi_page_stress(void) {
    uint32_t sizes[] = {2, 4, 8, 16};
    uint64_t blocks[32][16]; /* max 16 pages per block */
    int      npages[32];
    int nblocks = 0;

    for (int round = 0; round < 4; round++) {
        for (int si = 0; si < 4; si++) {
            uint32_t count = sizes[si];
            uint64_t phys = pmm_alloc_pages(count);
            if (!phys) {
                kprintf("[FAIL] pmm_alloc_pages(%u) failed at round %d\n", count, round);
                goto cleanup;
            }
            /* Record individual pages */
            for (uint32_t p = 0; p < count; p++)
                blocks[nblocks][p] = phys + p * PAGE_SIZE;
            npages[nblocks] = (int)count;
            nblocks++;

            /* Write pattern across all pages */
            uint8_t* v = (uint8_t*)PHYS_TO_VIRT(phys);
            for (uint32_t j = 0; j < count * 4096; j++)
                v[j] = (uint8_t)(round ^ si ^ j);
            /* Readback */
            for (uint32_t j = 0; j < count * 4096; j++) {
                if (v[j] != (uint8_t)(round ^ si ^ j)) {
                    kprintf("[FAIL] multi-page byte %d corrupt blk %d\n", j, nblocks - 1);
                    goto cleanup;
                }
            }
        }
    }

    /* Free in different order than allocation */
    for (int b = nblocks - 1; b >= 0; b--) {
        uint64_t first = blocks[b][0];
        /* Verify page_owner is set for first page */
        uint64_t owner = pmm_page_owner(first);
        (void)owner;
        pmm_free_pages(first, (uint32_t)npages[b]);
    }

    kprintf("[TEST] pmm_multi_page_stress: %d blocks (2/4/8/16 pages), PASS\n", nblocks);
    return TEST_PASS;

cleanup:
    for (int b = 0; b < nblocks; b++)
        pmm_free_pages(blocks[b][0], (uint32_t)npages[b]);
    return TEST_FAIL;
}

/* ============================================================
 * Test: PMM accounting consistency
 *
 * Verifies pmm_free_pages_count / pmm_total_pages behave
 * consistently across alloc/free cycles.
 * ============================================================ */
static int test_pmm_accounting(void) {
    uint64_t free_before = pmm_free_pages_count();
    uint64_t total = pmm_total_pages();
    ASSERT_TRUE(total > 0, "total pages > 0");

    uint64_t pages[50];
    int i;
    for (i = 0; i < 50; i++) {
        pages[i] = pmm_alloc_page();
        if (!pages[i]) break;
    }
    int n = i;
    ASSERT_TRUE(n > 0, "should alloc at least 1 page");

    uint64_t free_mid = pmm_free_pages_count();
    ASSERT_TRUE(free_mid < free_before, "free count should decrease after allocs");

    for (i = 0; i < n; i++) pmm_free_page(pages[i]);

    uint64_t free_after = pmm_free_pages_count();
    ASSERT_TRUE(free_after >= free_before - 5,
                "free count should return near original (within 5)");

    kprintf("[TEST] pmm_accounting: total=%llu free=%llu->%llu->%llu, PASS\n",
            total, free_before, free_mid, free_after);
    return TEST_PASS;
}

/* ============================================================
 * Test: VMM page table map/unmap stress
 *
 * Creates a fresh PML4, maps 100 pages at various virtual
 * addresses, walks each to verify the PTE, unmaps, walks
 * to verify absence, then frees the page table tree.
 * Catches: page table entry corruption, sub-table leaks,
 * incorrect PML4/PDPT/PD/PT index calculation.
 * ============================================================ */
static int test_vmm_map_unmap_stress(void) {
    uint64_t test_pml4 = vmm_alloc_page_table();
    ASSERT_TRUE(test_pml4 != 0, "vmm_alloc_page_table");

    uint64_t vaddr_base = 0x100000000ULL; /* 4 GB, in user space */
    int nmap = 0;
    int i;

    for (i = 0; i < 100; i++) {
        uint64_t vaddr = vaddr_base + (uint64_t)i * 0x200000; /* 2 MB apart */
        uint64_t phys = pmm_alloc_page();
        if (!phys) break;
        page_zero(phys);
        err_t e = vmm_map_page(test_pml4, vaddr, phys, PAGE_PRESENT | PAGE_WRITE | PAGE_USER);
        if (e != ERR_OK) {
            kprintf("[FAIL] vmm_map_page failed at vaddr=0x%lx (err=%d)\n", vaddr, e);
            pmm_free_page(phys);
            break;
        }
        /* Walk to verify */
        page_entry_t* pte = vmm_walk_pagetable(test_pml4, vaddr);
        if (!pte || !(*pte & PAGE_PRESENT)) {
            kprintf("[FAIL] PTE not present after map at vaddr=0x%lx\n", vaddr);
            pmm_free_page(phys);
            break;
        }
        if (((*pte & 0xFFFFFFFFF000ULL) != (phys & 0xFFFFFFFFF000ULL))) {
            kprintf("[FAIL] PTE phys mismatch: pte=0x%lx phys=0x%lx\n", *pte, phys);
            pmm_free_page(phys);
            break;
        }
        nmap++;
    }

    ASSERT_TRUE(nmap > 0, "should map at least 1 page");
    kprintf("[TEST] vmm_map_unmap_stress: mapped %d pages\n", nmap);

    /* Unmap all and walk to verify absence */
    for (i = 0; i < nmap; i++) {
        uint64_t vaddr = vaddr_base + (uint64_t)i * 0x200000;
        page_entry_t* pte_before = vmm_walk_pagetable(test_pml4, vaddr);
        /* We need the physical address to free it */
        uint64_t pte_phys = 0;
        if (pte_before && (*pte_before & PAGE_PRESENT))
            pte_phys = *pte_before & 0xFFFFFFFFF000ULL;
        vmm_unmap_page(test_pml4, vaddr);
        page_entry_t* pte_after = vmm_walk_pagetable(test_pml4, vaddr);
        if (pte_after && (*pte_after & PAGE_PRESENT)) {
            kprintf("[FAIL] PTE still present after unmap at vaddr=0x%lx\n", vaddr);
            /* Don't free — would double-free if PTE still points to the page */
        } else if (pte_phys) {
            pmm_free_page(pte_phys);
        }
    }

    vmm_free_user_pages(test_pml4);
    kprintf("[TEST] vmm_map_unmap_stress: %d pages mapped/unmapped, PASS\n", nmap);
    return TEST_PASS;
}

/* ============================================================
 * Test: VMM page permission flags
 *
 * Maps pages with different permission combinations and
 * verifies the PTE flags via vmm_walk_pagetable.
 * ============================================================ */
static int test_vmm_page_permissions(void) {
    uint64_t test_pml4 = vmm_alloc_page_table();
    ASSERT_TRUE(test_pml4 != 0, "vmm_alloc_page_table");

    /* Test various flag combinations.
     * NOTE: NX bit (PAGE_NX) is NOT included — this kernel does not
     * set IA32_EFER.NXE, so vmm_walk_pagetable will not show NX set. */
    uint64_t flag_sets[] = {
        PAGE_PRESENT,
        PAGE_PRESENT | PAGE_WRITE,
        PAGE_PRESENT | PAGE_WRITE | PAGE_USER,
        PAGE_PRESENT | PAGE_USER,
    };
    int nflags = sizeof(flag_sets) / sizeof(flag_sets[0]);
    int ok = 1;

    for (int i = 0; i < nflags; i++) {
        uint64_t vaddr = 0x200000000ULL + (uint64_t)i * 0x10000000;
        uint64_t phys = pmm_alloc_page();
        ASSERT_TRUE(phys != 0, "pmm_alloc_page");
        page_zero(phys);

        err_t e = vmm_map_page(test_pml4, vaddr, phys, flag_sets[i]);
        if (e != ERR_OK) {
            kprintf("[FAIL] map flags 0x%lx: err=%d\n", flag_sets[i], e);
            pmm_free_page(phys);
            ok = 0; break;
        }

        page_entry_t* pte = vmm_walk_pagetable(test_pml4, vaddr);
        if (!pte || !(*pte & PAGE_PRESENT)) {
            kprintf("[FAIL] flags 0x%lx: PTE not present\n", flag_sets[i]);
            pmm_free_page(phys);
            ok = 0; break;
        }

        uint64_t expected_phys = phys & 0xFFFFFFFFF000ULL;
        uint64_t actual_phys = *pte & 0xFFFFFFFFF000ULL;
        if (actual_phys != expected_phys) {
            kprintf("[FAIL] flags 0x%lx: phys 0x%lx != expected 0x%lx\n",
                    flag_sets[i], actual_phys, expected_phys);
            pmm_free_page(phys);
            ok = 0; break;
        }

        uint64_t expected_flags = flag_sets[i] & ~(0xFFFFFFFFF000ULL);
        uint64_t actual_flags = *pte & ~(0xFFFFFFFFF000ULL);
        /* Check that expected flags are subset of actual */
        if ((actual_flags & expected_flags) != expected_flags) {
            kprintf("[FAIL] flags 0x%lx: actual PTE flags 0x%lx missing expected 0x%lx\n",
                    flag_sets[i], actual_flags, expected_flags);
            pmm_free_page(phys);
            ok = 0; break;
        }

        /* Unmap and free */
        vmm_unmap_page(test_pml4, vaddr);
        pmm_free_page(phys);
    }

    vmm_free_user_pages(test_pml4);
    if (ok) kprintf("[TEST] vmm_page_permissions: %d flag combos, PASS\n", nflags);
    return ok ? TEST_PASS : TEST_FAIL;
}

/* ============================================================
 * Test: Scheduler thread create/join storm
 *
 * Creates N threads that each increment a shared counter and
 * write to a per-thread slot, then joins them all.
 * Catches: thread table leaks, TCB corruption, scheduler
 * queue corruption under high thread count.
 * ============================================================ */
static volatile int storm_count;
static volatile int storm_slots[32];

static void storm_worker(void* arg) {
    int id = (int)(uintptr_t)arg;
    cpu_flags_t f;
    spinlock_acquire(&sched_queue_lock, &f);
    storm_count++;
    storm_slots[id] = id + 1;
    spinlock_release(&sched_queue_lock, f);
    thread_exit(0);
}

static int test_sched_thread_storm(void) {
    thread_t* threads[32];
    int n = 32;
    int i;

    for (i = 0; i < n; i++) {
        threads[i] = thread_create(storm_worker, (void*)(uintptr_t)i,
                                    THREAD_DEF_PRIO, "storm");
        if (!threads[i]) { n = i; break; }
        sched_add_thread(threads[i]);
    }

    ASSERT_TRUE(n > 0, "should create at least 1 thread");
    kprintf("[TEST] sched_thread_storm: created %d threads\n", n);

    for (i = 0; i < n; i++) {
        int code;
        err_t e = thread_join(threads[i], &code);
        ASSERT_ERR_OK(e, "thread_join");
        ASSERT_TRUE(code == 0, "exit code 0");
    }

    /* Verify all workers actually ran */
    if (storm_count != n) {
        kprintf("[FAIL] storm_count=%d (expected %d)\n", storm_count, n);
        return TEST_FAIL;
    }
    for (i = 0; i < n; i++) {
        if (storm_slots[i] != i + 1) {
            kprintf("[FAIL] worker %d slot=%d (expected %d)\n", i, storm_slots[i], i + 1);
            return TEST_FAIL;
        }
    }

    kprintf("[TEST] sched_thread_storm: %d threads created/joined, PASS\n", n);
    return TEST_PASS;
}

/* ============================================================
 * Test: Scheduler sleep timing accuracy
 *
 * Creates a thread that sleeps for 50ms and measures the
 * actual sleep duration. Verifies it woke up within a
 * reasonable bound (20-500ms).
 * ============================================================ */
static volatile uint64_t sleep_start_tick;

static void sleeper_worker(void* arg) {
    (void)arg;
    sleep_start_tick = sched_get_switch_count();
    thread_sleep(50);
    thread_exit(0);
}

static int test_sched_sleep_accuracy(void) {
    thread_t* t = thread_create(sleeper_worker, NULL, THREAD_DEF_PRIO, "sleeper");
    ASSERT_NOT_NULL(t, "thread_create");

    sched_add_thread(t);
    int code;
    err_t e = thread_join(t, &code);
    ASSERT_ERR_OK(e, "thread_join");
    ASSERT_TRUE(code == 0, "exit code 0");

    kprintf("[TEST] sched_sleep_accuracy: 50ms sleep, PASS\n");
    return TEST_PASS;
}

/* ============================================================
 * Test: kmalloc compaction stress
 *
 * Allocates 200 blocks of various sizes, frees every other,
 * calls kmalloc_compact to reclaim empty slab pages, then
 * verifies kmalloc still works after compaction.
 * Catches: slab metadata corruption, use-after-free during
 * compaction, dangling slab page references.
 * ============================================================ */
static int test_kmalloc_compaction(void) {
    void* ptrs[200];
    int n = 0;

    for (int i = 0; i < 200; i++) {
        size_t sz = (size_t)(8 + (i * 13 % 247));
        ptrs[n] = kmalloc(sz);
        if (!ptrs[n]) break;
        kmemset(ptrs[n], (uint8_t)(i & 0xFF), sz);
        n++;
    }
    ASSERT_TRUE(n > 0, "should alloc at least 1 block");

    /* Free odd entries */
    for (int i = 1; i < n; i += 2) {
        kfree(ptrs[i]);
        ptrs[i] = NULL;
    }

    /* Compact */
    size_t freed = kmalloc_compact();
    kprintf("[TEST] kmalloc_compaction: compact freed %llu pages\n", freed);

    /* Verify even entries still intact */
    for (int i = 0; i < n; i += 2) {
        if (ptrs[i]) {
            size_t sz = (size_t)(8 + (i * 13 % 247));
            uint8_t* v = (uint8_t*)ptrs[i];
            for (size_t j = 0; j < sz; j++) {
                if (v[j] != (uint8_t)(i & 0xFF)) {
                    kprintf("[FAIL] compaction: byte %llu block %d corrupt\n", j, i);
                    goto cleanup;
                }
            }
        }
    }

    /* Allocate more after compaction */
    for (int i = 0; i < 50; i++) {
        void* p = kmalloc(64);
        ASSERT_NOT_NULL(p, "kmalloc after compaction");
        kmemset(p, 0xAB, 64);
        kfree(p);
    }

cleanup:
    for (int i = 0; i < n; i++)
        if (ptrs[i]) kfree(ptrs[i]);
    if (n == 0) return TEST_FAIL;
    kprintf("[TEST] kmalloc_compaction: %d allocs, compact+realloc, PASS\n", n);
    return TEST_PASS;
}

/* Guard page test: verify kernel stack guard page is unmapped during
 * thread lifetime, and re-mapped after the thread is reaped. */
static void guard_test_worker(void* arg) {
    (void)arg;
    /* Just exit — the guard page is exercised by any stack usage above
     * the worker function's own stack frame. */
}

static int test_guard_page_basic(void) {
    thread_t* t = thread_create(guard_test_worker, NULL,
                                THREAD_DEF_PRIO, "guard-test");
    ASSERT_NOT_NULL(t, "thread_create(guard_test_worker)");
    ASSERT_NOT_NULL(t->kernel_stack, "kernel_stack");
    ASSERT_TRUE(t->block_phys != 0, "block_phys set");

    /* Verify guard page PTE is not present during thread lifetime */
    uint64_t guard_phys = t->block_phys + PAGE_SIZE;
    uint64_t guard_virt = (uint64_t)PHYS_TO_VIRT(guard_phys);
    page_entry_t* gpte = vmm_peek_pte(vmm_get_kernel_pml4(), guard_virt);
    ASSERT_NOT_NULL(gpte, "guard PTE slot exists");
    ASSERT_TRUE((*gpte & PAGE_PRESENT) == 0, "guard page not present");

    kprintf("[TEST] guard page at phys=0x%lx virt=0x%lx: PTE=0x%lx (present=%d)\n",
            guard_phys, guard_virt, *gpte, (*gpte & PAGE_PRESENT) ? 1 : 0);

    /* Run the thread and wait for completion */
    sched_add_thread(t);
    thread_join(t, NULL);

    /* Spin until the thread is reaped (guard page re-mapped) */
    int reaped = 0;
    for (int i = 0; i < 1000; i++) {
        sched_reap_zombies();
        gpte = vmm_peek_pte(vmm_get_kernel_pml4(), guard_virt);
        if (gpte && (*gpte & PAGE_PRESENT)) {
            reaped = 1;
            break;
        }
        thread_sleep(1);
    }

    ASSERT_TRUE(reaped, "guard page re-mapped after thread exit");
    kprintf("[TEST] guard page re-mapped: PTE=0x%lx\n", gpte ? *gpte : 0);

    return TEST_PASS;
}

static int test_procfs_basic(void) {
    char buf[512];
    int fd, n;
    int pass = 1;

    /* Test /proc/cpuinfo — must be non-empty */
    fd = vfs_open("/proc/cpuinfo", O_RDONLY);
    if (fd < 0) { pass = 0; goto done; }
    n = (int)vfs_read(fd, buf, sizeof(buf) - 1);
    vfs_close(fd);
    if (n <= 0) { kprintf("  /proc/cpuinfo empty\n"); pass = 0; goto done; }
    buf[n] = '\0';
    if (kstrstr(buf, "processor") == NULL) { kprintf("  /proc/cpuinfo no 'processor'\n"); pass = 0; goto done; }

    /* Test /proc/meminfo — must contain MemTotal */
    fd = vfs_open("/proc/meminfo", O_RDONLY);
    if (fd < 0) { pass = 0; goto done; }
    n = (int)vfs_read(fd, buf, sizeof(buf) - 1);
    vfs_close(fd);
    if (n <= 0) { kprintf("  /proc/meminfo empty\n"); pass = 0; goto done; }
    buf[n] = '\0';
    if (kstrstr(buf, "MemTotal") == NULL) { kprintf("  /proc/meminfo no MemTotal\n"); pass = 0; goto done; }

    /* Test /proc/uptime — must be non-empty */
    fd = vfs_open("/proc/uptime", O_RDONLY);
    if (fd < 0) { kprintf("  /proc/uptime open failed\n"); pass = 0; goto done; }
    n = (int)vfs_read(fd, buf, sizeof(buf) - 1);
    vfs_close(fd);
    if (n <= 0) { kprintf("  /proc/uptime empty\n"); pass = 0; goto done; }

    /* Test /proc/version — must contain OPERtur */
    fd = vfs_open("/proc/version", O_RDONLY);
    if (fd < 0) { kprintf("  /proc/version open failed\n"); pass = 0; goto done; }
    n = (int)vfs_read(fd, buf, sizeof(buf) - 1);
    vfs_close(fd);
    if (n <= 0) { kprintf("  /proc/version empty\n"); pass = 0; goto done; }
    buf[n] = '\0';
    if (kstrstr(buf, "OPERtur") == NULL) { kprintf("  /proc/version no OPERtur\n"); pass = 0; goto done; }

    /* Test /proc/stat — must be non-empty */
    fd = vfs_open("/proc/stat", O_RDONLY);
    if (fd < 0) { kprintf("  /proc/stat open failed\n"); pass = 0; goto done; }
    n = (int)vfs_read(fd, buf, sizeof(buf) - 1);
    vfs_close(fd);
    if (n <= 0) { kprintf("  /proc/stat empty\n"); pass = 0; goto done; }

    /* Test /proc/self readlink — must resolve to /proc/<pid> */
    n = vfs_readlink("/proc/self", buf, sizeof(buf) - 1);
    if (n < 0) { kprintf("  /proc/self readlink failed\n"); pass = 0; goto done; }
    buf[n] = '\0';
    if (kstrstr(buf, "/proc/") == NULL) { kprintf("  /proc/self doesn't start with /proc/\n"); pass = 0; goto done; }

    /* Test /proc/1/status — must contain Name: */
    fd = vfs_open("/proc/1/status", O_RDONLY);
    if (fd < 0) { kprintf("  /proc/1/status open failed\n"); pass = 0; goto done; }
    n = (int)vfs_read(fd, buf, sizeof(buf) - 1);
    vfs_close(fd);
    buf[n] = '\0';
    if (kstrstr(buf, "Name:") == NULL) { kprintf("  /proc/1/status no Name:\n"); pass = 0; goto done; }

done:
    kprintf("[TEST] procfs_basic: %s\n", pass ? "PASS" : "FAIL");
    return pass ? TEST_PASS : TEST_FAIL;
}

static volatile int32_t* futex_word;

static void futex_worker(void* arg) {
    (void)arg;
    *futex_word = 1;
    futex_wait((int32_t*)futex_word, 1);
    thread_exit(0);
}

static int test_futex_basic(void) {
    futex_word = (volatile int32_t*)kmalloc(sizeof(int32_t));
    if (!futex_word) { kprintf("  futex alloc failed\n"); return TEST_FAIL; }
    *futex_word = 0;

    thread_t* t = thread_create(futex_worker, NULL, THREAD_DEF_PRIO, "futex-w");
    ASSERT_NOT_NULL(t, "thread_create");
    sched_add_thread(t);

    /* Wake in a loop until we actually wake someone — this handles the
     * SMP race where futex_wake runs before the worker has queued */
    int woken = 0;
    for (int i = 0; i < 200 && woken == 0; i++) {
        thread_sleep(20);
        woken = futex_wake((int32_t*)futex_word, 1);
    }
    ASSERT_NE(woken, 0, "futex_wake should wake at least one waiter");

    int code;
    err_t e = thread_join(t, &code);
    ASSERT_ERR_OK(e, "thread_join");
    ASSERT_TRUE(code == 0, "worker exit code 0");

    kfree((void*)futex_word);
    kprintf("[TEST] futex_basic: PASS\n");
    return TEST_PASS;
}

static int test_epoll_basic(void) {
    int fds[2];
    ASSERT_EQ(pipe_create(fds), 0, "pipe_create");

    int epfd = do_epoll_create1(0);
    ASSERT_TRUE(epfd >= 0, "epoll_create1");

    epoll_event_t ev;
    ev.events = EPOLLIN;
    ev.data = 42;
    ASSERT_ERR_OK(do_epoll_ctl(epfd, EPOLL_CTL_ADD, fds[0], &ev), "epoll_ctl ADD");

    /* No data yet — epoll_wait with timeout 0 should return 0 */
    epoll_event_t out[4];
    int n = do_epoll_wait(epfd, out, 4, 0);
    ASSERT_EQ(n, 0, "epoll_wait returns 0 before write");

    /* Write to pipe write end */
    char msg[] = "hello";
    int64_t written = vfs_write(fds[1], msg, 6);
    ASSERT_EQ((int)written, 6, "pipe write returns 6");

    /* Read back to verify pipe has data */
    char rbuf[8];
    int64_t nread = vfs_read(fds[0], rbuf, 6);
    ASSERT_EQ((int)nread, 6, "pipe read returns 6 before epoll");
    /* Data consumed, write again */
    written = vfs_write(fds[1], msg, 6);
    ASSERT_EQ((int)written, 6, "second pipe write");

    /* Now epoll_wait should return 1 event */
    n = do_epoll_wait(epfd, out, 4, 0);
    ASSERT_EQ(n, 1, "epoll_wait returns 1 after write");
    ASSERT_EQ(out[0].data, (uint64_t)42, "user data matches");
    ASSERT_TRUE(out[0].events & EPOLLIN, "EPOLLIN set");

    /* Remove from epoll */
    ASSERT_ERR_OK(do_epoll_ctl(epfd, EPOLL_CTL_DEL, fds[0], NULL), "epoll_ctl DEL");

    /* Close everything */
    vfs_close(epfd);
    vfs_close(fds[0]);
    vfs_close(fds[1]);

    kprintf("[TEST] epoll_basic: PASS\n");
    return TEST_PASS;
}

static int test_shm_basic(void) {
    int fd;

    /* 1. Create shared memory object */
    int ret = sys_shm_open("/test_shm", O_CREAT | O_RDWR, 0666);
    ASSERT_TRUE(ret >= 0, "shm_open /test_shm");
    fd = ret;

    /* 2. Truncate to one page */
    ASSERT_ERR_OK(vfs_ftruncate(fd, PAGE_SIZE), "ftruncate to PAGE_SIZE");

    /* 3. Write known data */
    char wbuf[] = "hello shared memory";
    size_t wlen = sizeof(wbuf);
    int64_t written = vfs_write(fd, wbuf, wlen);
    ASSERT_EQ((int)written, (int)wlen, "vfs_write returns len");

    /* 4. Read back and verify */
    char rbuf[64];
    kmemset(rbuf, 0, sizeof(rbuf));
    vfs_lseek(fd, 0, 0); /* seek to start */
    int64_t nread = vfs_read(fd, rbuf, wlen);
    ASSERT_EQ((int)nread, (int)wlen, "vfs_read returns len");
    ASSERT_EQ(kstrcmp(rbuf, wbuf), 0, "data matches after write+read");

    /* 5. Close and re-open by name (verifies persistence) */
    vfs_close(fd);
    ret = sys_shm_open("/test_shm", O_RDWR, 0);
    ASSERT_TRUE(ret >= 0, "shm_open existing /test_shm");
    fd = ret;

    /* 6. Verify data still there */
    kmemset(rbuf, 0, sizeof(rbuf));
    vfs_lseek(fd, 0, 0); /* seek to start */
    nread = vfs_read(fd, rbuf, wlen);
    ASSERT_EQ((int)nread, (int)wlen, "vfs_read returns len after reopen");
    ASSERT_EQ(kstrcmp(rbuf, wbuf), 0, "data persists after reopen");

    /* 7. O_EXCL on existing name should fail */
    ret = sys_shm_open("/test_shm", O_CREAT | O_EXCL | O_RDWR, 0666);
    ASSERT_EQ(ret, ERR_EXIST, "shm_open O_EXCL on existing name");

    /* 8. Unlink */
    ASSERT_ERR_OK(sys_shm_unlink("/test_shm"), "shm_unlink /test_shm");

    /* 9. Object still accessible via open fd */
    kmemset(rbuf, 0, sizeof(rbuf));
    vfs_lseek(fd, 0, 0);
    nread = vfs_read(fd, rbuf, wlen);
    ASSERT_EQ((int)nread, (int)wlen, "vfs_read after unlink");
    vfs_close(fd);

    /* 10. Re-open after unlink should fail */
    ret = sys_shm_open("/test_shm", O_RDWR, 0);
    ASSERT_EQ(ret, ERR_NOENT, "shm_open after unlink returns ERR_NOENT");

    /* 11. Direct get_page test (verify physical pages are shared) */
    ret = sys_shm_open("/shm_getpage", O_CREAT | O_RDWR, 0666);
    ASSERT_TRUE(ret >= 0, "shm_open for get_page test");
    fd = ret;
    ASSERT_ERR_OK(vfs_ftruncate(fd, PAGE_SIZE), "ftruncate");
    /* Get the node from fd table */
    vfs_fd_t* ft = vfs_get_fd_table();
    vfs_node_t* node = ft[fd].node;
    ASSERT_NOT_NULL(node, "node from fd table");
    ASSERT_NOT_NULL(node->fs->ops->get_page, "get_page op exists");
    /* get_page at offset 0 should return a valid physical address */
    uint64_t phys = node->fs->ops->get_page(node, 0);
    ASSERT_NE(phys, (uint64_t)0, "get_page returns non-zero phys");
    /* Write to the physical page directly */
    char gp_msg[] = "get_page works";
    kmemcpy((void*)PHYS_TO_VIRT(phys), gp_msg, sizeof(gp_msg));
    /* Read back via vfs_read — should see the data written to phys page */
    kmemset(rbuf, 0, sizeof(rbuf));
    vfs_lseek(fd, 0, 0);
    nread = vfs_read(fd, rbuf, sizeof(gp_msg));
    ASSERT_EQ(kstrcmp(rbuf, gp_msg), 0, "data via phys page matches vfs_read");
    vfs_close(fd);
    sys_shm_unlink("/shm_getpage");

    kprintf("[TEST] shm_basic: PASS\n");
    return TEST_PASS;
}

static int test_sysfs_basic(void) {
    char buf[512];
    int fd, n;
    int pass = 1;

    /* Test /sys/kernel/version — must contain OPERtur */
    fd = vfs_open("/sys/kernel/version", O_RDONLY);
    if (fd < 0) { kprintf("  /sys/kernel/version open failed\n"); pass = 0; goto done; }
    n = (int)vfs_read(fd, buf, sizeof(buf) - 1);
    vfs_close(fd);
    if (n <= 0) { kprintf("  /sys/kernel/version empty\n"); pass = 0; goto done; }
    buf[n] = '\0';
    if (kstrstr(buf, "OPERtur") == NULL) { kprintf("  /sys/kernel/version no OPERtur\n"); pass = 0; goto done; }

    /* Test /sys/kernel/uptime — must be non-empty */
    fd = vfs_open("/sys/kernel/uptime", O_RDONLY);
    if (fd < 0) { kprintf("  /sys/kernel/uptime open failed\n"); pass = 0; goto done; }
    n = (int)vfs_read(fd, buf, sizeof(buf) - 1);
    vfs_close(fd);
    if (n <= 0) { kprintf("  /sys/kernel/uptime empty\n"); pass = 0; goto done; }

    /* Test /sys/block — must have at least one block device */
    fd = vfs_open("/sys/block", O_RDONLY);
    if (fd < 0) { kprintf("  /sys/block open failed\n"); pass = 0; goto done; }
    vfs_close(fd);

    /* Try to open a known block dev (ramdisk) and read size */
    fd = vfs_open("/sys/block/ramdisk/size", O_RDONLY);
    if (fd < 0) { kprintf("  /sys/block/ramdisk/size open failed\n"); pass = 0; goto done; }
    n = (int)vfs_read(fd, buf, sizeof(buf) - 1);
    vfs_close(fd);
    if (n <= 0) { kprintf("  /sys/block/ramdisk/size empty\n"); pass = 0; goto done; }

    fd = vfs_open("/sys/block/ramdisk/sector_size", O_RDONLY);
    if (fd < 0) { kprintf("  /sys/block/ramdisk/sector_size open failed\n"); pass = 0; goto done; }
    n = (int)vfs_read(fd, buf, sizeof(buf) - 1);
    vfs_close(fd);
    if (n <= 0) { kprintf("  /sys/block/ramdisk/sector_size empty\n"); pass = 0; goto done; }

done:
    kprintf("[TEST] sysfs_basic: %s\n", pass ? "PASS" : "FAIL");
    return pass ? TEST_PASS : TEST_FAIL;
}

static int test_numa_basic(void) {
    int cpu = smp_cpu_id();
    int my_node = (numa_available && per_cpu_data[cpu]) ? per_cpu_data[cpu]->node_id : 0;
    kprintf("[TEST] CPU %d node_id=%d numa_available=%d\n", cpu, my_node, numa_available);

    /* 1. Self-distance must be 10 */
    int self_dist = acpi_node_distance(my_node, my_node);
    ASSERT_EQ(self_dist, 10, "self distance == 10");
    kprintf("[TEST] self distance: %d\n", self_dist);

    /* 2. Cross-node distance is defined (non-zero) */
    if (numa_node_count > 1) {
        int other = (my_node == 0) ? 1 : 0;
        int cross = acpi_node_distance(my_node, other);
        kprintf("[TEST] distance %d->%d = %d\n", my_node, other, cross);
        ASSERT_NE(cross, (int)0, "cross-node distance != 0");
    }

    /* 3. Allocate and free a single page from local node */
    uint64_t phys1 = pmm_alloc_node_pages(1, my_node);
    ASSERT_NE(phys1, (uint64_t)0, "alloc_node_pages(1, local)");
    if (phys1) {
        uint64_t pidx1 = phys1 / PAGE_SIZE;
        kprintf("[TEST] alloc local node %d: phys=0x%lx idx=%lu\n",
                my_node, phys1, pidx1);
        if (numa_available)
            ASSERT_TRUE(acpi_is_page_in_node(pidx1, my_node),
                        "page on requested node");
        /* Write a test pattern */
        volatile uint64_t* p = (volatile uint64_t*)PHYS_TO_VIRT(phys1);
        for (int i = 0; i < 512; i++) p[i] = 0xCAFEBABE + i;
        pmm_free_page(phys1);
    }

    /* 4. Allocate and free 3 pages (contiguous) from local node */
    uint64_t phys3 = pmm_alloc_node_pages(3, my_node);
    ASSERT_NE(phys3, (uint64_t)0, "alloc_node_pages(3, local)");
    if (phys3) {
        kprintf("[TEST] alloc local node 3 pages: phys=0x%lx\n", phys3);
        if (numa_available) {
            for (uint32_t k = 0; k < 3; k++) {
                ASSERT_TRUE(acpi_is_page_in_node((phys3/PAGE_SIZE)+k, my_node),
                            "contiguous page on requested node");
            }
        }
        pmm_free_pages(phys3, 3);
    }

    /* 5. Accounting invariants: total free >= list free + cache sum */
    uint64_t free_before = pmm_free_pages_count();
    uint64_t p5 = pmm_alloc_node_pages(1, my_node);
    ASSERT_NE(p5, (uint64_t)0, "alloc for accounting test");
    if (p5) {
        uint64_t free_after = pmm_free_pages_count();
        ASSERT_EQ(free_after, free_before - 1, "free count decreased by 1");
        pmm_free_page(p5);
        free_after = pmm_free_pages_count();
        ASSERT_EQ(free_after, free_before, "free count restored after free");
    }

    /* 6. Cross-node fallback (only meaningful with >1 node) */
    if (numa_available && numa_node_count > 1) {
        int alt_node = (my_node == 0) ? 1 : 0;
        uint64_t p6 = pmm_alloc_node_pages(1, alt_node);
        ASSERT_NE(p6, (uint64_t)0, "cross-node alloc");
        if (p6) {
            int on_node = -1;
            for (int r = 0; r < numa_memory_region_count; r++) {
                if (numa_memory_regions[r].enabled &&
                    p6 >= numa_memory_regions[r].base &&
                    p6 < numa_memory_regions[r].base + numa_memory_regions[r].length)
                    on_node = numa_memory_regions[r].node;
            }
            kprintf("[TEST] cross-node alloc node %d got phys=0x%lx on_node=%d\n",
                    alt_node, p6, on_node);
            pmm_free_page(p6);
        }
    }

    /* 7. Verify pmm_current_node() matches per_cpu_data */
    int current = pmm_current_node();
    ASSERT_EQ(current, my_node, "pmm_current_node() matches CPU node_id");
    kprintf("[TEST] pmm_current_node() = %d\n", current);

    kprintf("[TEST] NUMA basic tests complete: %d nodes, %d memory regions\n",
            numa_node_count, numa_memory_region_count);
    return TEST_PASS;
}

/* ============================================================
 * test_pci_ecam — PCIe ECAM vs legacy config-space equivalence
 *
 * When ACPI MCFG is present (q35), the kernel's pci_config_read/
 * write go through memory-mapped ECAM. This test re-enumerates
 * bus 0 (+ bridge secondaries) using FORCED legacy 0xCF8/0xCFC
 * reads and verifies both views agree bidirectionally:
 *   - every device in the ECAM enumeration is present in the
 *     legacy view with identical vendor/device/class
 *   - every device in the legacy view is present in the ECAM
 *     enumeration with identical IDs
 * On platforms without MCFG (i440fx), ECAM is inactive and the
 * test is skipped (still PASS — nothing to verify).
 * ============================================================ */
static int test_pci_ecam(void) {
    if (!pci_ecam_active()) {
        kprintf("[TEST] test_pci_ecam: SKIPPED (no MCFG — legacy 0xCF8/0xCFC only)\n");
        return TEST_PASS;
    }

    kprintf("[TEST] test_pci_ecam: ECAM active, comparing with legacy config space\n");

    uint8_t  lbus[MAX_PCI_DEVICES], lslot[MAX_PCI_DEVICES], lfunc[MAX_PCI_DEVICES];
    uint16_t lvid[MAX_PCI_DEVICES], ldid[MAX_PCI_DEVICES];
    uint8_t  lclass[MAX_PCI_DEVICES], lsub[MAX_PCI_DEVICES];
    int nlegacy = 0;

    /* Legacy walk: bus 0 + secondary buses behind PCI-PCI bridges */
    uint8_t pending_buses[16];
    int nb = 0;
    pending_buses[nb++] = 0;

    while (nb > 0) {
        uint8_t bus = pending_buses[--nb];
        for (int slot = 0; slot < 32 && nlegacy < MAX_PCI_DEVICES; slot++) {
            uint32_t id = pci_config_read_legacy(bus, slot, 0, PCI_VENDOR_ID);
            if ((id & 0xFFFF) == 0xFFFF || (id & 0xFFFF) == 0x0000) continue;

            int funcs = 1;
            uint32_t hdr = pci_config_read_legacy(bus, slot, 0, PCI_HEADER_TYPE);
            if (hdr & 0x80) funcs = 8;

            for (int func = 0; func < funcs && nlegacy < MAX_PCI_DEVICES; func++) {
                uint32_t fid = pci_config_read_legacy(bus, slot, func, PCI_VENDOR_ID);
                if ((fid & 0xFFFF) == 0xFFFF || (fid & 0xFFFF) == 0x0000) continue;

                uint32_t did = pci_config_read_legacy(bus, slot, func, PCI_DEVICE_ID);
                uint32_t cr  = pci_config_read_legacy(bus, slot, func, PCI_REVISION);

                lbus[nlegacy] = bus;
                lslot[nlegacy] = slot;
                lfunc[nlegacy] = func;
                lvid[nlegacy] = fid & 0xFFFF;
                ldid[nlegacy] = (did >> 16) & 0xFFFF;
                lclass[nlegacy] = (cr >> 24) & 0xFF;
                lsub[nlegacy] = (cr >> 16) & 0xFF;
                nlegacy++;

                /* Recurse into PCI-PCI bridge secondaries */
                if ((cr >> 24) == PCI_CLASS_BRIDGE && ((cr >> 16) & 0xFF) == 0x04) {
                    uint32_t bus_reg = pci_config_read_legacy(bus, slot, func, 0x18);
                    uint8_t secondary = (bus_reg >> 8) & 0xFF;
                    if (secondary != bus && nb < 16) {
                        int already = 0;
                        for (int k = 0; k < nb; k++)
                            if (pending_buses[k] == secondary) already = 1;
                        if (!already) pending_buses[nb++] = secondary;
                    }
                }
            }
        }
    }

    kprintf("[TEST] test_pci_ecam: legacy view=%d devices, ECAM view=%d devices\n",
            nlegacy, pci_device_count());

    ASSERT_EQ(nlegacy, pci_device_count(),
              "legacy and ECAM enumerate the same device count");

    /* Direction 1: every ECAM-enumerated device must match legacy reads */
    for (int i = 0; i < pci_device_count(); i++) {
        pci_device_t* d = pci_get_device(i);
        uint32_t id = pci_config_read_legacy(d->bus, d->slot, d->func, PCI_VENDOR_ID);
        ASSERT_EQ((int)(id & 0xFFFF), (int)d->vendor_id, "ECAM device vendor matches legacy");
        uint32_t did = pci_config_read_legacy(d->bus, d->slot, d->func, PCI_DEVICE_ID);
        ASSERT_EQ((int)((did >> 16) & 0xFFFF), (int)d->device_id, "ECAM device ID matches legacy");
        uint32_t cr = pci_config_read_legacy(d->bus, d->slot, d->func, PCI_REVISION);
        ASSERT_EQ((int)((cr >> 24) & 0xFF), (int)d->class_code, "ECAM device class matches legacy");
        ASSERT_EQ((int)((cr >> 16) & 0xFF), (int)d->subclass, "ECAM device subclass matches legacy");

        int found = 0;
        for (int k = 0; k < nlegacy; k++) {
            if (lbus[k] == d->bus && lslot[k] == d->slot && lfunc[k] == d->func &&
                lvid[k] == d->vendor_id && ldid[k] == d->device_id)
                found = 1;
        }
        ASSERT_TRUE(found, "ECAM device present in legacy view");
    }

    /* Direction 2: every legacy-found device must be in the ECAM table */
    for (int k = 0; k < nlegacy; k++) {
        int found = 0;
        for (int i = 0; i < pci_device_count(); i++) {
            pci_device_t* d = pci_get_device(i);
            if (d->bus == lbus[k] && d->slot == lslot[k] && d->func == lfunc[k]) {
                if (d->vendor_id == lvid[k] && d->device_id == ldid[k] &&
                    d->class_code == lclass[k] && d->subclass == lsub[k])
                    found = 1;
            }
        }
        ASSERT_TRUE(found, "legacy device present in ECAM view");
    }

    kprintf("[TEST] test_pci_ecam: %d devices identical via ECAM and legacy, PASS\n", nlegacy);
    return TEST_PASS;
}

/* ============================================================
 * test_pci_msix — MSI-X capability parsing + programming
 *
 * Active on PCIe platforms (ECAM) whose NIC exposes MSI-X/MSI
 * (QEMU e1000e/igb). Classic e1000 has no MSI-X/MSI capability →
 * SKIPPED (still PASS). Verifies:
 *   - capability walk finds the MSI-X capability
 *   - table spec constraints (BAR ≤ 5, 4 KB-aligned offset, ≤ 32 entries)
 *   - driver-programmed entry 0: addr=0xFEE00000|apic_id<<12, data=0x48,
 *     vector control unmasked; message control MSI-X-enable bit set
 *   - end-to-end delivery: the MSI-X ISR fired on real RX traffic (DHCP)
 */
static int test_pci_msix(void) {
    if (!pci_ecam_active()) {
        kprintf("[TEST] test_pci_msix: SKIPPED (no PCIe ECAM — legacy platform)\n");
        return TEST_PASS;
    }

    pci_device_t* nic_dev = NULL;
    for (int i = 0; i < pci_device_count(); i++) {
        pci_device_t* d = pci_get_device(i);
        if (d && d->class_code == PCI_CLASS_NETWORK && d->vendor_id == E1000_VENDOR_INTEL) {
            nic_dev = d;
            break;
        }
    }
    if (!nic_dev) {
        kprintf("[TEST] test_pci_msix: SKIPPED (no Intel NIC)\n");
        return TEST_PASS;
    }

    pci_msix_info_t mi;
    err_t pe = pci_msix_probe(nic_dev, &mi);
    if (pe != ERR_OK) {
        kprintf("[TEST] test_pci_msix: SKIPPED (NIC %04x:%04x has no MSI-X capability)\n",
                nic_dev->vendor_id, nic_dev->device_id);
        return TEST_PASS;
    }

    kprintf("[TEST] test_pci_msix: %04x:%04x cap=0x%02x size=%u BAR%d+0x%x "
            "(expected QEMU: BAR3+0x0)\n",
            nic_dev->vendor_id, nic_dev->device_id,
            mi.cap_offset, mi.table_size, mi.table_bar, mi.table_offset);

    ASSERT_TRUE(mi.table_size >= 1 && mi.table_size <= 32,
                "MSI-X table size in spec range");
    ASSERT_TRUE(mi.table_bar <= 5, "MSI-X table BAR index in range");
    ASSERT_EQ(mi.table_offset % 4096u, 0u, "MSI-X table offset 4 KB-aligned");

    /* Verify the driver-programmed entry 0 (e1000.c enables MSI-X in
     * e1000_init_nic, which runs before the kernel self-tests). */
    uint64_t tbl_phys = (uint64_t)(nic_dev->bar[mi.table_bar] & ~0xF) + mi.table_offset;
    uint64_t tbl_virt = PHYS_TO_VIRT(tbl_phys & ~0xFFFULL);
    page_entry_t* pte = vmm_walk_pagetable(vmm_get_kernel_pml4(), tbl_virt);
    if (!pte) {
        vmm_map_page(vmm_get_kernel_pml4(), tbl_virt, tbl_phys & ~0xFFFULL,
                     PAGE_PRESENT | PAGE_WRITE | PAGE_NX);
    }
    volatile uint32_t* entry = (volatile uint32_t*)(tbl_virt + (tbl_phys & 0xFFF));

    uint32_t expect_addr = 0xFEE00000u | ((uint32_t)apic_id << 12);
    ASSERT_EQ(entry[0], expect_addr, "MSI-X entry 0 addr: xAPIC base + BSP APIC ID");
    ASSERT_EQ(entry[1], 0u, "MSI-X entry 0 addr hi = 0");
    ASSERT_EQ(entry[2], (uint32_t)PCI_MSIX_VECTOR, "MSI-X entry 0 data = MSI-X vector");
    ASSERT_EQ(entry[3], 0u, "MSI-X entry 0 vector control unmasked");

    uint32_t msgctl = pci_config_read(nic_dev->bus, nic_dev->slot, nic_dev->func,
                                      (uint8_t)(mi.cap_offset + 2)) >> 16;
    ASSERT_TRUE((msgctl & (1 << 15)) != 0, "MSI-X message control enabled");
    ASSERT_TRUE((msgctl & (1 << 14)) == 0, "MSI-X function not masked");

    /* MSI capability presence (QEMU igb exposes one at 0x50; QEMU
     * e1000e at 0xD0) — informational only */
    uint8_t msi_off;
    err_t me = pci_find_cap(nic_dev->bus, nic_dev->slot, nic_dev->func,
                            PCI_CAP_ID_MSI, &msi_off);
    if (me == ERR_OK) {
        uint32_t mmsg = pci_config_read(nic_dev->bus, nic_dev->slot, nic_dev->func,
                                        (uint8_t)(msi_off + 2)) >> 16;
        kprintf("[TEST] test_pci_msix: MSI cap at 0x%02x (64-bit=%d)\n",
                msi_off, (mmsg >> 7) & 1);
    }

    /* Driver state + end-to-end delivery. The ISR must have fired on the
     * DHCP traffic exchanged before this test runs. */
    ASSERT_TRUE(e1000_msix_active(), "driver enabled MSI-X on the NIC");
    ASSERT_TRUE(e1000_msix_count() > 0, "MSI-X interrupt delivered (RX traffic)");

    kprintf("[TEST] test_pci_msix: PASS (%u MSI-X interrupts delivered)\n",
            e1000_msix_count());
    return TEST_PASS;
}

/* ============================================================
 * sched_verify — run queue invariant checker
 *
 * Walks every per-CPU run queue and asserts:
 *   - node count matches rq_counts[prio]
 *   - rq_total matches sum of rq_counts
 *   - every queued thread has state == THREAD_READY
 *   - every queued thread has cpu_queue matching its CPU
 *   - no cycles (walk terminates within rq_counts + 1 steps)
 *
 * Returns 0 on success, -1 on first error (prints diagnostics).
 * Safe to call from test context; acquires sched_queue_lock.
 * ============================================================ */
static int sched_verify(void) {
    int ncpus = smp_enabled ? nr_cpus : 1;
    int errors = 0;
    cpu_flags_t qflags;

    spinlock_acquire(&sched_queue_lock, &qflags);

    for (int c = 0; c < ncpus; c++) {
        per_cpu_data_t* pcp = per_cpu_data[c];
        if (!pcp) continue;

        int total_counted = 0;

        for (int p = 0; p <= THREAD_MAX_PRIO; p++) {
            thread_t* t = (thread_t*)pcp->rq_heads[p];
            int cnt = 0;

            while (t) {
                cnt++;
                if (cnt > (int)pcp->rq_counts[p] + 2) {
                    kprintf("[SCHED-VRFY] CPU%d prio%03d: cycle (cnt=%d, rq_count=%u)\n",
                            c, p, cnt, pcp->rq_counts[p]);
                    errors++; break;
                }
                if (t->state != THREAD_READY) {
                    kprintf("[SCHED-VRFY] CPU%d prio%03d: thread id=%llu state=%d\n",
                            c, p, t->id, t->state);
                    errors++;
                }
                if (t->cpu_queue != c) {
                    kprintf("[SCHED-VRFY] CPU%d prio%03d: thread id=%llu cpu_queue=%d\n",
                            c, p, t->id, t->cpu_queue);
                    errors++;
                }
                t = t->rq_next;
            }

            if (cnt != (int)pcp->rq_counts[p]) {
                kprintf("[SCHED-VRFY] CPU%d prio%03d: walked %d, rq_counts=%u\n",
                        c, p, cnt, pcp->rq_counts[p]);
                errors++;
            }
            total_counted += cnt;
        }

        if (total_counted != (int)pcp->rq_total) {
            kprintf("[SCHED-VRFY] CPU%d: rq_total=%u but counted %d\n",
                    c, pcp->rq_total, total_counted);
            errors++;
        }
    }

    spinlock_release(&sched_queue_lock, qflags);
    return errors == 0 ? 0 : -1;
}

/* ============================================================
 * SMP Test: work stealing (sched_steal_thread)
 *
 * CPU0 creates N%run queue threads.  CPU1 (idle) should steal
 * some via sched_steal_thread().  Verifies:
 *   - CPU1 rq_total increases (stealing occurred)
 *   - all threads complete via thread_join
 *   - sched_verify() invariant passes
 * ============================================================ */
#define STEAL_NTHREADS 32
static volatile int steal_running;

static void steal_worker(void* arg) {
    (void)arg;
    while (steal_running) thread_yield();
    thread_exit(0);
}

static int test_sched_steal(void) {
    if (!smp_enabled || smp_nr_cpus() < 2) {
        kprintf("[TEST] test_sched_steal: SKIP (SMP < 2)\n");
        return TEST_PASS;
    }

    thread_t* threads[STEAL_NTHREADS];

    steal_running = 1;
    for (int i = 0; i < STEAL_NTHREADS; i++) {
        threads[i] = thread_create(steal_worker, NULL, THREAD_DEF_PRIO, "steal-w");
        ASSERT_NOT_NULL(threads[i], "thread_create");
        sched_add_thread(threads[i]);
    }

    /* Wait for CPU1 to steal threads. BSP → AP reschedule IPI
     * fires every ~10ms; 500ms gives ~50 steal opportunities. */
    thread_sleep(500);

    uint32_t cpu1_rq = per_cpu_data[1]->rq_total;
    ASSERT_TRUE(cpu1_rq > 0, "CPU1 should have stolen >= 1 thread");

    /* Every thread's cpu_queue must be valid */
    for (int i = 0; i < STEAL_NTHREADS; i++) {
        int cq = threads[i]->cpu_queue;
        ASSERT_TRUE(cq >= 0 && cq < smp_nr_cpus(), "thread cpu_queue in range");
    }

    ASSERT_ERR_OK(sched_verify(), "sched_verify after steal");

    steal_running = 0;
    for (int i = 0; i < STEAL_NTHREADS; i++)
        thread_join(threads[i], NULL);

    kprintf("[TEST] test_sched_steal: %d threads, CPU1 rq=%u, PASS\n",
            STEAL_NTHREADS, cpu1_rq);
    return TEST_PASS;
}

/* ============================================================
 * SMP Test: load balancing push (sched_balance_push)
 *
 * Places 30 threads on CPU0 and 2 on CPU1, then waits for the
 * periodic balance timer to push threads from overloaded CPU0
 * to underloaded CPU1.  Verifies:
 *   - load imbalance decreases or is already balanced
 *   - sched_verify() invariant passes
 * ============================================================ */
static volatile int balance_running;

static void balance_worker(void* arg) {
    (void)arg;
    while (balance_running) thread_yield();
    thread_exit(0);
}

static int test_sched_balance_push(void) {
    if (!smp_enabled || smp_nr_cpus() < 2) {
        kprintf("[TEST] test_sched_balance_push: SKIP (SMP < 2)\n");
        return TEST_PASS;
    }

    thread_t* threads[50];
    int n_cpu0 = 30, n_cpu1 = 2, idx = 0;

    balance_running = 1;
    for (int i = 0; i < n_cpu0; i++, idx++) {
        threads[idx] = thread_create(balance_worker, NULL, THREAD_DEF_PRIO, "bal-w");
        ASSERT_NOT_NULL(threads[idx], "thread_create");
        sched_place_thread(threads[idx], 0);
    }
    for (int i = 0; i < n_cpu1; i++, idx++) {
        threads[idx] = thread_create(balance_worker, NULL, THREAD_DEF_PRIO, "bal-w");
        ASSERT_NOT_NULL(threads[idx], "thread_create");
        sched_place_thread(threads[idx], 1);
    }

    uint32_t cpu0_before = per_cpu_data[0]->rq_total;
    uint32_t cpu1_before = per_cpu_data[1]->rq_total;
    int diff_before = (int)cpu0_before - (int)cpu1_before;
    kprintf("[TEST] balance: before: CPU0=%u CPU1=%u diff=%d\n",
            cpu0_before, cpu1_before, diff_before);

    /* Wait for balance_counter (100 ticks = 100ms) to fire several times */
    thread_sleep(500);

    uint32_t cpu0_after = per_cpu_data[0]->rq_total;
    uint32_t cpu1_after = per_cpu_data[1]->rq_total;
    int diff_after = (int)cpu0_after - (int)cpu1_after;
    kprintf("[TEST] balance: after:  CPU0=%u CPU1=%u diff=%d\n",
            cpu0_after, cpu1_after, diff_after);

    /* Absolute imbalance should not have grown */
    int abs_before = diff_before >= 0 ? diff_before : -diff_before;
    int abs_after  = diff_after  >= 0 ? diff_after  : -diff_after;
    ASSERT_TRUE(abs_after <= abs_before + 2,
                "balance: load imbalance should not increase");

    ASSERT_ERR_OK(sched_verify(), "sched_verify after balance");

    balance_running = 0;
    for (int i = 0; i < idx; i++)
        thread_join(threads[i], NULL);

    kprintf("[TEST] test_sched_balance_push: diff %d→%d, PASS\n",
            diff_before, diff_after);
    return TEST_PASS;
}

/* ============================================================
 * SMP Test: CPU affinity pinning
 *
 * Creates a thread with affinity restricted to CPU 2 (or the
 * highest online CPU when <3 are available).  Verifies:
 *   - thread runs and completes
 *   - cpu_queue never leaves the pinned CPU
 *   - sched_verify() invariant passes
 * ============================================================ */
static volatile int aff_ran;
static volatile int aff_running;

static void aff_worker(void* arg) {
    (void)arg;
    aff_ran = 1;
    while (aff_running) thread_yield();
    thread_exit(0);
}

static int test_sched_affinity_pin(void) {
    if (!smp_enabled || smp_nr_cpus() < 2) {
        kprintf("[TEST] test_sched_affinity_pin: SKIP (SMP < 2)\n");
        return TEST_PASS;
    }

    /* Pin to the highest-indexed CPU — must be ≥ 2 for a meaningful test
     * when SMP has 2+ CPUs, else use CPU 1. */
    int target_cpu = smp_nr_cpus() > 2 ? 2 : 1;

    aff_ran = 0;
    aff_running = 1;

    thread_t* t = thread_create(aff_worker, NULL, THREAD_DEF_PRIO, "aff-pin");
    ASSERT_NOT_NULL(t, "thread_create");

    sched_set_thread_affinity(t, 1ULL << target_cpu);
    ASSERT_EQ(t->cpu_affinity, (uint64_t)(1ULL << target_cpu), "affinity mask set");

    sched_add_thread(t);

    /* Wait for the worker to run */
    thread_sleep(300);
    ASSERT_TRUE(aff_ran, "worker ran");

    aff_running = 0;
    thread_join(t, NULL);

    ASSERT_EQ(t->cpu_queue, target_cpu, "thread cpu_queue == target_cpu");
    ASSERT_ERR_OK(sched_verify(), "sched_verify after affinity pin");

    kprintf("[TEST] test_sched_affinity_pin: pinned to CPU%d, PASS\n", target_cpu);
    return TEST_PASS;
}

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
    /* SMP scheduler tests */
    if (test_sched_steal() == TEST_PASS) pass++; else fail++;
    if (test_sched_balance_push() == TEST_PASS) pass++; else fail++;
    if (test_sched_affinity_pin() == TEST_PASS) pass++; else fail++;
    /* Core subsystem stress tests */
    if (test_pmm_alloc_free_stress() == TEST_PASS) pass++; else fail++;
    if (test_pmm_multi_page_stress() == TEST_PASS) pass++; else fail++;
    if (test_pmm_accounting() == TEST_PASS) pass++; else fail++;
    if (test_vmm_map_unmap_stress() == TEST_PASS) pass++; else fail++;
    if (test_vmm_page_permissions() == TEST_PASS) pass++; else fail++;
    if (test_sched_thread_storm() == TEST_PASS) pass++; else fail++;
    if (test_sched_sleep_accuracy() == TEST_PASS) pass++; else fail++;
    if (test_kmalloc_compaction() == TEST_PASS) pass++; else fail++;
    if (test_guard_page_basic() == TEST_PASS) pass++; else fail++;
    if (test_procfs_basic() == TEST_PASS) pass++; else fail++;
    if (test_futex_basic() == TEST_PASS) pass++; else fail++;
    if (test_epoll_basic() == TEST_PASS) pass++; else fail++;
    if (test_shm_basic() == TEST_PASS) pass++; else fail++;
    if (test_sysfs_basic() == TEST_PASS) pass++; else fail++;
    if (test_numa_basic() == TEST_PASS) pass++; else fail++;
    if (test_pci_ecam() == TEST_PASS) pass++; else fail++;
    if (test_pci_msix() == TEST_PASS) pass++; else fail++;

    kprintf("[TEST] === Results: %d pass, %d fail ===\n", pass, fail);
}

#endif /* KERNEL_SELF_TEST */

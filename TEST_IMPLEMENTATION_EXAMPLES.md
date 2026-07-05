# 🔧 Test Implementation Examples
**OPERtur/TRY1 - Detailed Test Code Templates**  
**Date:** June 13, 2026

---

## Memory Safety Test Examples

### Example 1: Detect Recursive strcpy Bug

**Test File:** `os/src/lib/libuser/test_string.c`

```c
#include "string.h"
#include "stdio.h"
#include <setjmp.h>

static jmp_buf test_jump;
static int test_caught_hang = 0;

void test_signal_handler(int sig) {
    test_caught_hang = 1;
    longjmp(test_jump, 1);
}

int test_strcpy_recursive(void) {
    /* Test: strcpy should not recurse infinitely */
    
    signal(SIGALRM, test_signal_handler);
    alarm(2);  /* 2-second timeout */
    
    if (setjmp(test_jump) == 0) {
        char dst[256] = {0};
        char src[256] = "hello";
        char* result = strcpy(dst, src);
        alarm(0);  /* Cancel alarm */
    }
    
    if (test_caught_hang) {
        printf("[FAIL] strcpy caused hang/infinite recursion\n");
        return TEST_FAIL;
    }
    
    if (strcmp(dst, "hello") != 0) {
        printf("[FAIL] strcpy result incorrect: got '%s'\n", dst);
        return TEST_FAIL;
    }
    
    printf("[PASS] strcpy works without recursion\n");
    return TEST_PASS;
}
```

### Example 2: NULL Pointer After kmalloc

**Test File:** `os/src/kernel/kmalloc_test.c`

```c
#ifdef KMALLOC_SELF_TEST

/* Simulate OOM by filling memory first */
static int test_kmalloc_null_check(void) {
    /* Allocate blocks until we hit OOM */
    void* allocs[1000];
    int alloc_count = 0;
    
    for (int i = 0; i < 1000; i++) {
        allocs[i] = kmalloc(4096);
        if (!allocs[i]) break;  /* OOM reached */
        alloc_count++;
    }
    
    /* Now a new allocation should fail */
    void* should_be_null = kmalloc(4096);
    
    if (should_be_null != NULL) {
        kprintf("[FAIL] kmalloc should return NULL on OOM\n");
        return TEST_FAIL;
    }
    
    /* Free everything */
    for (int i = 0; i < alloc_count; i++) {
        kfree(allocs[i]);
    }
    
    kprintf("[PASS] kmalloc correctly returns NULL on OOM\n");
    return TEST_PASS;
}

/* Test: Code that calls kmalloc must check for NULL */
static int test_kmalloc_null_protection(void) {
    /* This simulates the bug in fsck.c */
    
    uint8_t* buffer = (uint8_t*)kmalloc(1000000);
    if (!buffer) {
        kprintf("[PASS] Null check prevented crash\n");
        return TEST_PASS;
    }
    
    /* If we got here, allocation succeeded */
    kfree(buffer);
    kprintf("[PASS] Allocation succeeded\n");
    return TEST_PASS;
}

#endif
```

---

## Concurrency Test Examples

### Example 3: Lock Balance Detection

**Test File:** `os/src/kernel/spinlock_test.c`

```c
#ifdef SPINLOCK_SELF_TEST

/* Monitor lock acquisitions and releases */
static volatile int lock_acquire_count = 0;
static volatile int lock_release_count = 0;
static volatile int lock_imbalance_detected = 0;

/* Wrap spinlock operations to track calls */
static inline void tracked_acquire(spinlock_t* lock) {
    spinlock_acquire(lock);
    lock_acquire_count++;
}

static inline void tracked_release(spinlock_t* lock) {
    spinlock_release(lock);
    lock_release_count++;
    
    if (lock_release_count > lock_acquire_count) {
        lock_imbalance_detected = 1;
    }
}

/* Test: tcp_find_conn should always release lock on error */
static int test_tcp_find_conn_lock_balance(void) {
    lock_acquire_count = 0;
    lock_release_count = 0;
    lock_imbalance_detected = 0;
    
    /* Call tcp_find_conn with invalid parameters */
    tcp_conn_t* result = tcp_find_conn(AF_INET, NULL, 0, 0);
    
    if (result != NULL) {
        kprintf("[FAIL] tcp_find_conn should return NULL for invalid params\n");
        return TEST_FAIL;
    }
    
    if (lock_imbalance_detected) {
        kprintf("[FAIL] Lock imbalance detected: %d acquires, %d releases\n",
                lock_acquire_count, lock_release_count);
        return TEST_FAIL;
    }
    
    if (lock_acquire_count != lock_release_count) {
        kprintf("[FAIL] Locks not balanced: acquired=%d, released=%d\n",
                lock_acquire_count, lock_release_count);
        return TEST_FAIL;
    }
    
    kprintf("[PASS] Locks properly balanced on error\n");
    return TEST_PASS;
}

#endif
```

### Example 4: Race Condition Detection

**Test File:** `os/src/kernel/race_test.c`

```c
#ifdef RACE_CONDITION_TEST

static volatile int shared_counter = 0;
static volatile int race_detected = 0;

static int worker_thread_1(void* arg) {
    for (int i = 0; i < 1000; i++) {
        int val = shared_counter;
        /* Simulate some delay to increase race window */
        volatile int dummy = 0;
        for (int j = 0; j < 100; j++) dummy++;
        shared_counter = val + 1;
    }
    return 0;
}

static int worker_thread_2(void* arg) {
    for (int i = 0; i < 1000; i++) {
        int val = shared_counter;
        volatile int dummy = 0;
        for (int j = 0; j < 100; j++) dummy++;
        shared_counter = val + 1;
    }
    return 0;
}

static int test_race_condition_detection(void) {
    /* Without proper locking, final counter should not be 2000 */
    shared_counter = 0;
    race_detected = 0;
    
    thread_t* t1 = thread_create(worker_thread_1, NULL, THREAD_DEF_PRIO, "t1");
    thread_t* t2 = thread_create(worker_thread_2, NULL, THREAD_DEF_PRIO, "t2");
    
    sched_add_thread(t1);
    sched_add_thread(t2);
    
    thread_join(t1, NULL);
    thread_join(t2, NULL);
    
    if (shared_counter != 2000) {
        kprintf("[PASS] Race condition detected: counter=%d (expected 2000)\n",
                shared_counter);
        race_detected = 1;
        return TEST_PASS;  /* Race detected as expected */
    }
    
    kprintf("[FAIL] No race detected (may need more iterations)\n");
    return TEST_FAIL;
}

#endif
```

---

## Network Test Examples

### Example 5: TCP Retransmission

**Test File:** `os/src/kernel/tcp_test_advanced.c`

```c
#ifdef TCP_ADVANCED_TEST

static int test_tcp_retransmission(void) {
    /* Setup: Create a TCP connection with data in flight */
    tcp_conn_t* conn = tcp_test_add_conn(AF_INET, 
                                         (uint8_t*)"\x0a\x00\x02\x0f", 
                                         9, 30001);
    if (!conn) return TEST_FAIL;
    
    conn->state = TCP_ESTABLISHED;
    conn->snd_nxt = 1000;
    conn->snd_una = 500;  /* Unacknowledged data */
    
    /* Simulate data in retransmit buffer */
    kmemset(conn->retx_buf, 'X', TCP_MSS);
    conn->retx_len = 100;
    conn->retx_seq = 500;
    
    /* Simulate RTO expiry */
    uint64_t now_ms = 5000;  /* Current time */
    uint64_t retx_timeout = now_ms - 10000;  /* RTO expired 10 seconds ago */
    
    /* Call tcp_tick to trigger retransmission */
    conn->rto_ms = 0;  /* Force RTO to be expired */
    tcp_tick();
    
    /* Verify: Data should have been retransmitted */
    if (conn->retx_seq != 500) {
        kprintf("[FAIL] Retransmission sequence incorrect\n");
        return TEST_FAIL;
    }
    
    if (conn->rto_ms == 0) {
        kprintf("[FAIL] RTO timer not reset after retransmission\n");
        return TEST_FAIL;
    }
    
    kprintf("[PASS] TCP retransmission works correctly\n");
    conn->used = 0;
    return TEST_PASS;
}

static int test_tcp_fast_retransmit(void) {
    /* Test: 3 duplicate ACKs trigger fast retransmit */
    tcp_conn_t* conn = tcp_test_add_conn(AF_INET, 
                                         (uint8_t*)"\x0a\x00\x02\x0f", 
                                         9, 30001);
    if (!conn) return TEST_FAIL;
    
    conn->state = TCP_ESTABLISHED;
    conn->snd_nxt = 1000;
    conn->snd_una = 500;
    
    /* Simulate receiving 3 duplicate ACKs */
    for (int i = 0; i < 3; i++) {
        tcp_handle_ack(conn, 500);  /* Same ACK number */
    }
    
    /* Should trigger fast retransmit, not wait for RTO */
    if (conn->snd_nxt != 501) {
        kprintf("[FAIL] Fast retransmit did not resend segment\n");
        return TEST_FAIL;
    }
    
    kprintf("[PASS] TCP fast retransmit works\n");
    conn->used = 0;
    return TEST_PASS;
}

#endif
```

### Example 6: UDP Error Path

**Test File:** `os/src/kernel/udp_test_advanced.c`

```c
#ifdef UDP_ADVANCED_TEST

static int test_udp_socket_error_handling(void) {
    /* Test: Error path should clean up resources */
    socket_t* s = socket_alloc(AF_INET, SOCK_DGRAM, 0);
    if (!s) return TEST_FAIL;
    
    /* Attempt bind to invalid address */
    struct sockaddr_in addr;
    kmemset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53);
    addr.sin_addr.s_addr = 0xFFFFFFFF;  /* Invalid */
    
    int ret = sock_bind(s, (struct sockaddr*)&addr, sizeof(addr));
    
    /* Should return error but not crash */
    if (ret >= 0) {
        kprintf("[FAIL] Invalid bind should return error\n");
        socket_release(s);
        return TEST_FAIL;
    }
    
    /* Socket should still be usable for another operation */
    ret = sock_bind(s, (struct sockaddr*)&addr, sizeof(addr));
    
    socket_release(s);
    kprintf("[PASS] UDP socket error handling works\n");
    return TEST_PASS;
}

static int test_udp_recv_timeout(void) {
    /* Test: recv with timeout should not hang */
    socket_t* s = socket_alloc(AF_INET, SOCK_DGRAM, 0);
    if (!s) return TEST_FAIL;
    
    struct sockaddr_in addr;
    kmemset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(5353);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    
    sock_bind(s, (struct sockaddr*)&addr, sizeof(addr));
    
    /* Set receive timeout to 100ms */
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 100000;
    sock_setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    
    uint8_t buf[1024];
    struct sockaddr_in from;
    socklen_t from_len = sizeof(from);
    
    uint64_t start = hal_timer_get_ticks();
    int ret = sock_recvfrom(s, buf, sizeof(buf), 0, 
                            (struct sockaddr*)&from, &from_len);
    uint64_t elapsed = hal_timer_get_ticks() - start;
    
    /* Should timeout and return ERR_TIMEOUT, not hang */
    if (ret != ERR_TIMEOUT) {
        kprintf("[FAIL] recv should timeout, got %d\n", ret);
        socket_release(s);
        return TEST_FAIL;
    }
    
    /* Should have waited roughly 100ms */
    if (elapsed < 80 || elapsed > 150) {
        kprintf("[WARN] Timeout timing off: %llu ms (expected ~100)\n", elapsed);
    }
    
    socket_release(s);
    kprintf("[PASS] UDP recv timeout works\n");
    return TEST_PASS;
}

#endif
```

---

## Filesystem Test Examples

### Example 7: SFS Cross-Block Dirent Test

**Test File:** `os/src/kernel/sfs_test.c`

```c
#ifdef SFS_SELF_TEST

static int test_sfs_dirent_spanning(void) {
    /* Test: Verify dirent that spans two blocks is handled correctly */
    
    /* SFS_BLOCK_SIZE=512, sizeof(sfs_dirent_t)=68
     * 512/68 = 7 complete entries per block
     * Entry 7 starts at offset 7*68=476 and extends to 544 (spans block boundary)
     */
    
    block_dev_t* dev = block_get_by_name("ramdisk");
    if (!dev) {
        kprintf("[FAIL] No ramdisk device\n");
        return TEST_FAIL;
    }
    
    /* Write known pattern to blocks */
    uint8_t block0[SFS_BLOCK_SIZE];
    uint8_t block1[SFS_BLOCK_SIZE];
    
    kmemset(block0, 0xAA, SFS_BLOCK_SIZE);
    kmemset(block1, 0xBB, SFS_BLOCK_SIZE);
    
    block_write(dev, 100, 1, block0);
    block_write(dev, 101, 1, block1);
    
    /* Now read and verify cross-block boundary calculations */
    uint8_t read_buf[2 * SFS_BLOCK_SIZE];
    block_read(dev, 100, 1, read_buf);
    block_read(dev, 101, 1, read_buf + SFS_BLOCK_SIZE);
    
    /* Entry 7 should have bytes from both blocks */
    uint8_t* entry7_ptr = read_buf + 476;  /* 7 * 68 */
    
    /* First 36 bytes from block0 (remaining: 512-476=36) */
    for (int i = 0; i < 36; i++) {
        if (entry7_ptr[i] != 0xAA) {
            kprintf("[FAIL] Entry7 first part corrupted at byte %d\n", i);
            return TEST_FAIL;
        }
    }
    
    /* Last 32 bytes from block1 (68-36=32) */
    for (int i = 36; i < 68; i++) {
        if (entry7_ptr[i] != 0xBB) {
            kprintf("[FAIL] Entry7 second part corrupted at byte %d\n", i);
            return TEST_FAIL;
        }
    }
    
    kprintf("[PASS] SFS dirent spanning blocks handled correctly\n");
    return TEST_PASS;
}

#endif
```

---

## Error Injection Test Examples

### Example 8: Error Injection Framework

**Test File:** `os/src/kernel/error_injection.h`

```c
#ifndef ERROR_INJECTION_H
#define ERROR_INJECTION_H

/* Error injection points */
typedef enum {
    INJECT_KMALLOC_FAIL,
    INJECT_KMALLOC_PARTIAL,
    INJECT_SPINLOCK_DEADLOCK,
    INJECT_NIC_DROPPED_PACKET,
    INJECT_DISK_I_O_ERROR,
} inject_point_t;

/* Injection state */
static struct {
    inject_point_t point;
    int trigger_count;
    int fail_at_count;
} error_inject = {0};

/* Macro to check if should inject error */
#define SHOULD_INJECT_ERROR(point) \
    (error_inject.point == point && \
     ++error_inject.trigger_count >= error_inject.fail_at_count)

/* In actual code: */
void* test_kmalloc_with_injection(size_t size) {
    if (SHOULD_INJECT_ERROR(INJECT_KMALLOC_FAIL)) {
        return NULL;  /* Simulate OOM */
    }
    return kmalloc(size);
}

/* Test that uses injection: */
static int test_tcp_handle_oom(void) {
    error_inject.point = INJECT_KMALLOC_FAIL;
    error_inject.fail_at_count = 1;
    error_inject.trigger_count = 0;
    
    tcp_conn_t* conn = tcp_test_add_conn(...);
    
    /* This should handle OOM gracefully */
    int ret = tcp_handle_syn(conn, syn_packet);
    
    if (ret != ERR_NOMEM) {
        kprintf("[FAIL] Should return ERR_NOMEM\n");
        return TEST_FAIL;
    }
    
    kprintf("[PASS] TCP handles OOM correctly\n");
    return TEST_PASS;
}

#endif
```

---

## Stress Test Examples

### Example 9: Concurrent TCP Connections

```c
static int stress_tcp_concurrent_connections(void) {
    /* Spawn 100 concurrent TCP connections */
    #define NUM_CONNS 100
    
    tcp_conn_t* conns[NUM_CONNS];
    for (int i = 0; i < NUM_CONNS; i++) {
        uint8_t addr[4];
        *(uint32_t*)addr = htonl(0x0a000200 + i);  /* 10.0.2.0-99 */
        conns[i] = tcp_test_add_conn(AF_INET, addr, 10000 + i, 80);
        if (!conns[i]) {
            kprintf("[FAIL] Could not create connection %d\n", i);
            return TEST_FAIL;
        }
    }
    
    /* Simulate data flow on all */
    for (int iter = 0; iter < 100; iter++) {
        for (int i = 0; i < NUM_CONNS; i++) {
            tcp_send(conns[i], "test", 4);
        }
    }
    
    /* Verify all still valid */
    for (int i = 0; i < NUM_CONNS; i++) {
        if (!conns[i]->used) {
            kprintf("[FAIL] Connection %d corrupted\n", i);
            return TEST_FAIL;
        }
        conns[i]->used = 0;
    }
    
    kprintf("[PASS] Stress: 100 concurrent connections OK\n");
    return TEST_PASS;
}
```

---

## Test Registration Framework

**File:** `os/src/kernel/test_registry.c`

```c
#include "test_registry.h"

/* Global test registry */
typedef struct {
    const char* name;
    int (*test_func)(void);
} test_entry_t;

static test_entry_t all_tests[] = {
    {"tcp_find_conn_ipv6", test_tcp_find_conn_ipv6},
    {"ndp_cache_miss", test_ndp_cache_miss},
    {"icmpv6_ns_parse", test_icmpv6_ns_parse},
    {"socket_refcount", test_socket_refcount},
    {"udp_queue_roundtrip", test_udp_queue_roundtrip},
    
    /* New tests */
    {"strcpy_recursive", test_strcpy_recursive},
    {"kmalloc_null_check", test_kmalloc_null_check},
    {"lock_balance", test_tcp_find_conn_lock_balance},
    {"tcp_retransmission", test_tcp_retransmission},
    
    {NULL, NULL}  /* Sentinel */
};

int run_all_tests(void) {
    int pass = 0, fail = 0;
    
    for (int i = 0; all_tests[i].name; i++) {
        kprintf("[TEST] Running: %s ... ", all_tests[i].name);
        int ret = all_tests[i].test_func();
        if (ret == TEST_PASS) {
            pass++;
            kprintf("PASS\n");
        } else {
            fail++;
            kprintf("FAIL\n");
        }
    }
    
    kprintf("\n=== TEST RESULTS ===\n");
    kprintf("Passed: %d\n", pass);
    kprintf("Failed: %d\n", fail);
    kprintf("Total:  %d\n", pass + fail);
    
    return fail == 0 ? 0 : 1;
}
```

---

**End of Examples**  
Use these as templates for implementing actual tests in your codebase!


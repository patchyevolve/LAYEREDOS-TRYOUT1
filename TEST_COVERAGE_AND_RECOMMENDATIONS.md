# 🧪 Comprehensive Testing Analysis & Recommendations
**OPERtur/TRY1 - OS/Kernel Implementation**  
**Date:** June 13, 2026 *(fully updated 2026-07-10 — comprehensive test suite sync)*  
**Status:** Synchronised with current codebase (81 tests across 6 suites)

---

## Executive Summary

The codebase contains **81 existing unit tests** organized into 6 test suites, plus integration test scripts. This report:

1. **Catalogs all existing tests** with descriptions
2. **Identifies coverage gaps** across major subsystems
3. **Proposes 50+ new tests** in priority order
4. **Suggests better test practices** for kernel code
5. **Outlines testing infrastructure improvements**

---

## 📊 Current Test Coverage *(as of 2026-07-10: 81 total tests)*

> **Note:** This document was written on 2026-06-13 when 25 tests existed. The test count has since grown to **81 tests** across 6 suites:
> - **5** network tests (`net_test.c`)
> - **10** storage tests (`storage_test.c`)
> - **37** kernel tests (`kernel_test.c`) — added SMP, NUMA, rwlock, seqlock, lockdep, concurrent alloc, panic recovery, CPU hotplug, RCU, watchdog, scheduler balance/steal/affinity, PMM concurrent, ATA concurrent
> - **6** SFS tests (`sfs_test.c`)
> - **4** process tests (`process_test.c`)
> - **19** security tests (`security_test.c`)
>
> All 81 pass in a single `make test-all` build. See `AGENTS.md` for full history.

### Current Test Suites (as of 2026-07-10: 81 total)

#### 1. **Network Tests** (`net_test.c`) — 5 Tests
| Test ID | Name | Coverage | Status |
|---------|------|----------|--------|
| N1 | `test_tcp_find_conn_ipv6` | TCP IPv6 5-tuple matching | ✅ Passing |
| N2 | `test_ndp_cache_miss` | NDP cache lookup/update | ✅ Passing |
| N3 | `test_icmpv6_ns_parse` | ICMPv6 NS packet parsing | ✅ Passing |
| N4 | `test_socket_refcount` | Socket lifecycle management | ✅ Passing |
| N5 | `test_udp_queue_roundtrip` | UDP datagram queue I/O | ✅ Passing |

**Invocation:** `make test-net` (runs 5/5 passing)

---

#### 2. **Storage Tests** (`storage_test.c`) — 10 Tests
| Test ID | Name | Coverage | Status |
|---------|------|----------|--------|
| S1 | `test_block_cache_basic` | Block cache read/write | ✅ Passing |
| S2 | `test_block_cache_writeback` | Block cache sync | ✅ Passing |
| S3 | `test_block_register_reject` | Invalid block device rejection | ✅ Passing |
| S4 | `test_swap_alloc_free` | Swap slot allocation | ✅ Passing |
| S5 | `test_swap_out_in` | Swap page write/read | ✅ Passing |
| S6 | `test_journal_init` | Journal initialization | ✅ Passing |
| S7 | `test_journal_log_commit` | Journal transaction | ✅ Passing |
| S8 | `test_journal_multi_txn` | Multi-transaction journal | ✅ Passing |
| S9 | `test_journal_full` | Journal capacity + checkpoint | ✅ Passing |
| S10 | `test_snapshot_take_info` | Snapshot creation | ✅ Passing |

**Invocation:** `make test-storage` (runs 10/10 passing)

---

#### 3. **Kernel Tests** (`kernel_test.c`) — 37 Tests (38 with lockdep)
| Test ID | Name | Coverage |
|---------|------|----------|
| K1–K5 | kmalloc (roundtrip, edge, stress, sizes, compaction) | Slab allocator correctness |
| K6 | `test_vma_add_find_remove` | VMA tracking |
| K7–K8 | `test_tcp_conn_create_destroy`, `test_tcp_conn_bind_listen` | TCP connection management |
| K9 | `test_socket_lifecycle_extended` | Socket refcount/ops |
| K10 | `test_spinlock_acquire_release` | Spinlock correctness |
| K11 | `test_tcp_retransmission` | TCP data retransmission |
| K12–K13 | `test_mutex_lock_unlock`, `test_mutex_stress` | Mutex correctness + concurrency |
| K14 | `test_udp_endpoint_multi` | Multiple UDP endpoints |
| K15 | `test_udp_endpoint_queue_full` | UDP queue overflow handling |
| K16 | `test_block_cache_eviction` | Block cache LRU eviction |
| K17 | `test_tcp_state_transitions` | TCP full state machine |
| K18 | `test_error_codes` | Kernel ERR_* to POSIX errno |
| K19–K20 | `test_veth_pair_basic`, `test_veth_frame_roundtrip` | Veth network pair |
| K21 | `test_smp_concurrent_spinlock` | SMP spinlock stress (2-CPU) |
| K22 | `test_smp_pmm_concurrent` | SMP PMM concurrent alloc/free |
| K23–K25 | `test_sched_steal`, `test_sched_balance_push`, `test_sched_affinity_pin` | SMP scheduler (2-CPU) |
| K26–K28 | `test_rwlock_basic`, `test_seqlock_basic`, `test_lockdep_ordering` | Sync primitives |
| K29–K31 | `test_pmm_alloc_free_stress`, `test_pmm_multi_page_stress`, `test_pmm_accounting` | PMM stress |
| K32–K33 | `test_vmm_map_unmap_stress`, `test_vmm_page_permissions` | VMM stress |
| K34 | `test_sched_thread_storm` | Many-thread scheduling |
| K35 | `test_sched_sleep_accuracy` | Sleep timer precision |
| K36 | `test_guard_page_basic` | Guard page detection |
| K37 | `test_numa_basic` | NUMA node alloc/free + SLIT distances |

**Invocation:** `make test-kernel` (runs 37/37 passing; +1 with lockdep)

---

#### 4. **SFS Filesystem Tests** (`sfs_test.c`) — 6 Tests
| Test ID | Name | Coverage | Status |
|---------|------|----------|--------|
| F1 | `test_sfs_create_write_read` | File create/write/read roundtrip | ✅ Passing |
| F2 | `test_sfs_mkdir_and_file` | Directory + file in subdir | ✅ Passing |
| F3 | `test_sfs_rename` | File rename | ✅ Passing |
| F4 | `test_sfs_links` | Hard links + symlinks | ✅ Passing |
| F5 | `test_sfs_stat` | File stat fields | ✅ Passing |
| F6 | `test_sfs_error_paths` | OOM/invalid-path errors | ✅ Passing |

**Invocation:** `make test-sfs` (runs 6/6 passing)

---

#### 5. **Process Tests** (`process_test.c`) — 4 Tests
| Test ID | Name | Coverage | Status |
|---------|------|----------|--------|
| P1 | `test_process_create_find_exit` | Create/find/exit lifecycle | ✅ Passing |
| P2 | `test_process_exit_code` | Exit code propagation to parent | ✅ Passing |
| P3 | `test_process_fork_basic` | Fork + child PID | ✅ Passing |
| P4 | `test_process_zombie_cleanup` | Zombie reap via waitpid | ✅ Passing |

**Invocation:** `make test-process` (runs 4/4 passing)

---

#### 6. **Security Tests** (`security_test.c`) — 19 Tests
| Test ID | Name | Coverage | Status |
|---------|------|----------|--------|
| SEC1 | `test_syscall_bad_fd` | Bad FD rejection | ✅ Passing |
| SEC2 | `test_syscall_null_buf` | NULL buffer rejection | ✅ Passing |
| SEC3 | `test_user_ptr_checks` | Kernel/user pointer range checks | ✅ Passing |
| SEC4 | `test_file_permissions` | File permission enforcement | ✅ Passing |
| SEC5 | `test_process_memory_isolation` | Separate CR3 + VMA parity | ✅ Passing |
| SEC6 | `test_stack_canary` | Stack canary verification | ✅ Passing |
| SEC7 | `test_vfs_fd_mode_enforcement` | VFS fd read/write mode enforcement | ✅ Passing |
| SEC8 | `test_cap_system` | Capability checks (CAP_SYS_BOOT, etc.) | ✅ Passing |
| SEC9 | `test_fork_limit` | Fork limit enforcement | ✅ Passing |
| SEC10 | `test_audit_log` | Audit ring buffer write/read | ✅ Passing |
| SEC11 | `test_uid_gid` | UID/GID syscalls | ✅ Passing |
| SEC12 | `test_dac_permissions` | POSIX DAC permission model | ✅ Passing |
| SEC13 | `test_syscall_filtering` | Syscall mask enforcement | ✅ Passing |
| SEC14 | `test_sha256` | SHA-256 known digest (NIST vectors) | ✅ Passing |
| SEC15 | `test_getrandom` | CSPRNG non-deterministic output | ✅ Passing |
| SEC16 | `test_unix_buf_direct` | AF_UNIX ring buffer I/O | ✅ Passing |
| SEC17 | `test_socketpair` | socketpair data round-trip + credentials | ✅ Passing |
| SEC18 | `test_no_new_privs` | prctl no_new_privs blocks setuid | ✅ Passing |
| SEC19 | `test_secure_boot` | Known-good ELF passes, garbage rejected | ✅ Passing |

**Invocation:** `make test-security` (runs 19/19 passing)

---

### Integration Test Scripts

#### 7. **Two-QEMU Test** (`test-2qemu.sh`)
- **Purpose:** IPv6 TCP/UDP echo validation between two QEMU instances
- **Coverage:** End-to-end network communication
- **Status:** ✅ Working (with timing issues)

#### 8. **Test Runner** (`test-runner.sh`)
- **Purpose:** Automated prompt-based output capture
- **Status:** ⚠️ Partial (pipe buffering issues in no-KVM QEMU)

---

## 🔴 Major Coverage Gaps

### Critical Missing Tests

| Category | Gap | Impact | Priority |
|----------|-----|--------|----------|
| **Memory** | No memory leak detection | Kernel corruption | CRITICAL |
| **Concurrency** | No race condition tests | Deadlock, corruption | CRITICAL |
| **Error Handling** | No error path tests | Undefined behavior | CRITICAL |
| **SFS Filesystem** | No SFS unit tests | Data loss | HIGH |
| **Networking** | No TCP reliability tests | Connection issues | HIGH |
| **Security** | No privilege escalation tests | Security holes | HIGH |

---

## 🎯 Proposed New Tests (50+)

### Phase 1: Critical Bug Fixes (Tests for Audit Issues)

#### Memory Safety Tests (10 tests)
```
M1. strcpy() Recursive Call Detection
M2. NULL Pointer Dereference After kmalloc
M3. Buffer Overflow in strcpy/strcat
M4. Integer Overflow in Size Calculations
M5. Use-After-Free Detection
M6. Memory Leak Detection
M7. Stack Overflow Detection
M8. Double-Free Detection
M9. Uninitialized Variable Detection
M10. Heap Corruption Detection
```

#### Concurrency Tests (12 tests)
```
C1. Lock Acquisition/Release Balance
C2. Spinlock Deadlock Detection
C3. Race Condition: Shared Variable Updates
C4. Race Condition: Reference Counting
C5. Priority Inversion Detection
C6. Atomicity of Critical Sections
C7. Signal Safety in Locked Code
C8. Lock Timeout Handling
C9. Re-entrancy Testing
C10. Inter-process Lock Conflicts
C11. Starvation Detection
C12. Fairness Testing
```

#### Error Path Tests (8 tests)
```
E1. Error Path Lock Release
E2. Error Path Resource Cleanup
E3. Error Path Memory Deallocation
E4. Error Path File Descriptor Closure
E5. Error Path Socket Cleanup
E6. Error Path Thread Termination
E7. Cascading Error Handling
E8. Error Code Propagation
```

---

### Phase 2: Network Subsystem (15 tests)

#### TCP Tests (8 tests)
```
TCP1. TCP State Machine All Transitions
TCP2. TCP Retransmission on Timeout
TCP3. TCP Duplicate ACK Handling
TCP4. TCP Fast Retransmit
TCP5. TCP Congestion Control (if implemented)
TCP6. TCP Connection Teardown (FIN sequence)
TCP7. TCP RST Handling in All States
TCP8. TCP Window Size Management
```

#### UDP Tests (4 tests)
```
UDP1. UDP Multicast Send/Receive
UDP2. UDP Broadcast Handling
UDP3. UDP Fragmentation & Reassembly
UDP4. UDP Socket Options (SO_REUSEADDR, etc.)
```

#### Protocol Tests (3 tests)
```
PROTO1. IPv4/IPv6 Dual-Stack Operations
PROTO2. DNS Resolution Timeout
PROTO3. DHCP Lease Renewal
```

---

### Phase 3: File System Tests (12 tests)

#### SFS Filesystem Tests (8 tests)
```
SFS1. SFS Inode Allocation/Deallocation
SFS2. SFS Dirent Spanning Blocks (cross-block bug)
SFS3. SFS Block Allocation Bitmap Corruption
SFS4. SFS Inode Bitmap Overflow
SFS5. SFS Large File Support (>4GB)
SFS6. SFS Directory Traversal Race
SFS7. SFS Concurrent Writes to Same File
SFS8. SFS Rename Atomicity
```

#### VFS Tests (4 tests)
```
VFS1. VFS Mount/Unmount
VFS2. VFS Symlink Following
VFS3. VFS Permission Checking
VFS4. VFS Path Canonicalization
```

---

### Phase 4: Memory Management Tests (8 tests)

#### Paging Tests (5 tests)
```
PAGE1. Page Allocation/Deallocation
PAGE2. Page Table Corruption Detection
PAGE3. Page Invalidation TLB Shootdown
PAGE4. Page Reclamation Under Pressure
PAGE5. COW (Copy-on-Write) Semantics
```

#### Swap Tests (3 tests)
```
SWAP1. Swap Thrashing Detection
SWAP2. OOM Killer Activation
SWAP3. Memory Pressure Handling
```

---

### Phase 5: Process Management Tests (5 tests)

```
PROC1. Process Creation/Destruction Leak Detection
PROC2. Zombie Process Cleanup
PROC3. Signal Delivery Race Conditions
PROC4. Core Dump on Segfault
PROC5. Exit Code Propagation
```

---

## 📝 Better Test Practices for Kernel Code

### 1. **Test Organization Structure**

```c
// Current structure (good):
#ifdef SUBSYSTEM_SELF_TEST
  static int test_case_1(void) { ... }
  void subsystem_self_test(void) { ... }
#endif

// Recommended improvements:
#include "test_framework.h"

DEFINE_TEST_SUITE(tcp_tests);

TEST(tcp_tests, state_machine_syn_sent_to_established) {
    ASSERT_EQ(tcp_conn_state(conn), TCP_SYN_SENT);
    tcp_handle_synack(conn, ...);
    ASSERT_EQ(tcp_conn_state(conn), TCP_ESTABLISHED);
}
```

### 2. **Assertions & Error Reporting**

```c
// Current:
if (value != expected) {
    kprintf("[TEST] test_name: FAIL — value mismatch\n");
    return TEST_FAIL;
}

// Better:
ASSERT_EQ(value, expected, "Expected %d but got %d", expected, value);
ASSERT_NOT_NULL(ptr, "Pointer allocation failed");
ASSERT_IN_RANGE(value, min, max, "Value %d outside range [%d, %d]");
```

### 3. **Setup & Teardown Fixtures**

```c
struct tcp_test_fixture {
    tcp_conn_t* conn;
    uint8_t* packet_buf;
    size_t packet_len;
};

static struct tcp_test_fixture* setup_tcp_test(void) {
    // Allocate and initialize test data
}

static void teardown_tcp_test(struct tcp_test_fixture* fix) {
    // Clean up resources
}
```

### 4. **Parametrized Testing**

```c
// Test with multiple inputs
TEST_PARAM(tcp_tests, syn_flags_parsing, {
    {0x02, TCP_SYN},
    {0x12, TCP_ACK | TCP_SYN},
    {0x10, TCP_ACK},
    {0x01, TCP_FIN},
}) {
    ASSERT_EQ(tcp_parse_flags(param.raw_byte), param.expected);
}
```

### 5. **Test Coverage Metrics**

```makefile
test-coverage:
@gcc -fprofile-arcs -ftest-coverage -c os/src/kernel/*.c
@ld ... (link)
@./kernel.elf
@gcov os/src/kernel/*.c
@lcov --capture ... > coverage.info
@genhtml coverage.info -o coverage_report
```

---

## 🏗️ Recommended Testing Infrastructure

### 1. **Kernel Test Framework** (New)

Create `os/src/kernel/test_framework.h`:

```c
#ifndef TEST_FRAMEWORK_H
#define TEST_FRAMEWORK_H

// Test case registration
typedef struct {
    const char* suite_name;
    const char* test_name;
    int (*test_func)(void);
    void (*setup)(void);
    void (*teardown)(void);
} test_case_t;

// Global test registry
#define DEFINE_TEST_SUITE(name) \
    static test_case_t __test_suite_##name##_cases[] = {

#define TEST(suite, name) \
    static int __test_##suite##_##name(void); \
    __attribute__((used)) test_case_t __test_##suite##_##name = { \
        .suite_name = #suite, \
        .test_name = #name, \
        .test_func = __test_##suite##_##name, \
    }; \
    static int __test_##suite##_##name(void)

// Assertions
#define ASSERT_EQ(actual, expected, ...) \
    do { if ((actual) != (expected)) { \
        kprintf("[ASSERT FAIL] " __VA_ARGS__); return -1; \
    }} while(0)

#define ASSERT_NOT_NULL(ptr, ...) \
    do { if (!(ptr)) { \
        kprintf("[ASSERT FAIL] " __VA_ARGS__); return -1; \
    }} while(0)

#endif
```

### 2. **Automated Test Execution**

```makefile
test: test-units test-integration test-stress

test-units:
@$(MAKE) -C os clean
@$(MAKE) -C os ENABLE_KERNEL_TEST=1
@timeout 30 qemu-system-x86_64 -kernel os/build/kernel.elf \
-serial mon:stdio -m 512M -no-reboot -nographic 2>&1 | \
tee test-output.log
@grep -q "All tests passed" test-output.log || exit 1

test-integration:
@./os/test-2qemu.sh

test-stress:
@timeout 60 qemu-system-x86_64 -kernel os/build/kernel.elf \
-serial mon:stdio -m 512M -no-reboot -nographic \
-device vhost-net (stress network) 2>&1 | tee stress-output.log
```

### 3. **Test Coverage Tracking**

Create `tools/coverage_report.sh`:

```bash
#!/bin/bash
# Generate test coverage report
gcc -fprofile-arcs -ftest-coverage $(find os/src -name "*.c")
# ... run tests ...
gcov os/src/kernel/*.c
lcov --capture -d . -o coverage.info
genhtml coverage.info -o coverage_html
echo "Coverage: $(grep "lines" coverage.info)"
```

---

## 🎯 Recommended Implementation Order

### Week 1 (Critical)
- [ ] Add memory safety tests (detect strcpy bug, NULL checks)
- [ ] Add lock balance tests (detect deadlock)
- [ ] Add error path tests (TCP, UDP error scenarios)

### Week 2 (High Priority)
- [ ] Expand TCP test coverage (all state transitions, retransmission)
- [ ] Add SFS filesystem unit tests
- [ ] Add concurrency race condition tests

### Week 3-4 (Medium Priority)
- [ ] Parametrized network protocol tests
- [ ] Virtual memory tests (paging, COW)
- [ ] Process lifecycle tests

### Month 2 (Quality Improvements)
- [ ] Test coverage metrics & reporting
- [ ] Stress testing framework
- [ ] Fuzzing for network handlers

---

## 📋 Test Execution Commands

### Current Commands
```bash
make test           # Runs kernel boot + embedded tests
make test-net       # Network unit tests (5/5 pass)
make test-storage   # Storage unit tests (10/10 pass)
make test-net-2qemu # Two-QEMU IPv6 TCP/UDP echo
```

### Proposed Commands (Post-Implementation)
```bash
make test-all              # All tests, all suites
make test-coverage         # With coverage metrics
make test-coverage-html    # Generate HTML report
make test-stress           # Stress testing (chaos)
make test-fuzzing          # Fuzzing network handlers
make test-valgrind         # Memory detection
make test-thread-sanitizer # Race condition detection
```

---

## 📊 Coverage Metrics Summary

| Category | Current | Proposed | Gap |
|----------|---------|----------|-----|
| **Unit Tests** | 25 | 75+ | 50 |
| **Integration Tests** | 1 | 10 | 9 |
| **Stress Tests** | 0 | 5 | 5 |
| **Fuzzing Tests** | 0 | 10 | 10 |
| **Error Path Coverage** | ~10% | ~80% | 70% |
| **Code Coverage** | ~15% | ~60% | 45% |
| **Network Protocol Coverage** | ~30% | ~85% | 55% |
| **Filesystem Coverage** | ~10% | ~70% | 60% |

---

## 🔗 Critical Tests to Add FIRST

Based on the audit findings, prioritize these 10 tests:

1. ✅ **Memory: Recursive strcpy Detection** → Prevent userspace crashes
2. ✅ **Memory: kmalloc NULL Check** → Prevent kernel panics
3. ✅ **Concurrency: Lock Balance** → Prevent system deadlock
4. ✅ **Network: TCP Retransmission** → Fix connection reliability
5. ✅ **Network: UDP Error Handling** → Fix socket robustness
6. ✅ **SFS: Cross-block Dirent** → Verify directory fix
7. ✅ **Filesystem: Concurrent Write** → Detect race conditions
8. ✅ **Memory: Stack Overflow** → Kernel hardening
9. ✅ **Process: Zombie Cleanup** → Resource leak prevention
10. ✅ **Signal: Signal-safe Operations** → Concurrency safety

---

## 📝 Test Quality Standards

### Each Test Should Include:

✅ **Clear Test Purpose**
```
/* Test: Verify TCP retransmission on timeout
 * Precondition: Connection established with sent data
 * Action: Simulate RTO expiry
 * Expected: Data retransmitted from original sequence number
 * Bug Prevention: Detects loss of retransmit buffer
 */
```

✅ **Setup & Cleanup**
```
setup: Create mock connection, populate buffers
action: Run test logic
teardown: Free all resources, reset state
```

✅ **Error Checking**
```
- NULL pointer checks after allocation
- Range validation for returned values
- State machine consistency checks
```

✅ **Negative Test Cases**
```
- What if allocation fails?
- What if timeout expires?
- What if packet is malformed?
```

---

## 🚀 Implementation Checklist

- [ ] Create test framework header (`test_framework.h`)
- [ ] Implement 10 critical memory/concurrency tests
- [ ] Expand network test suite (+15 tests)
- [ ] Add SFS filesystem tests (+8 tests)
- [ ] Add process management tests (+5 tests)
- [ ] Set up coverage metrics tracking
- [ ] Create automated test execution pipeline
- [ ] Document test running procedures
- [ ] Add CI/CD integration
- [ ] Achieve 60%+ code coverage

---

## 📚 References

- **Network Tests Location:** `os/src/kernel/net_test.c`
- **Storage Tests Location:** `os/src/kernel/storage_test.c`
- **Thread Tests Location:** `os/src/boot/thread_test-c.c`
- **Test Makefile Targets:** `os/Makefile`
- **Integration Scripts:** `os/test-2qemu.sh`, `os/test-runner.sh`

---

**Report Completed:** June 13, 2026 *(historical — see AGENTS.md for current state)*  
**Total Tests Existing (Jun 13):** 25  
**Total Tests Existing (Jul 10):** 81  
**Tests Recommended:** 50+ (many now implemented)  
**Critical Gap Priority:** Memory Safety, Concurrency, Error Paths


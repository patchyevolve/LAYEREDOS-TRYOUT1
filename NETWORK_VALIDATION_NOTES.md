# Network Validation Notes — OPERtur/TRY1

> Last updated: 2026-06-11
> Codebase: os/src/kernel/
> Build: `make -C os && make -C os test-net`

---

## 1. CURRENT IMPLEMENTATION

### 1.1 E1000 RX/TX Flow

**Source:** `nic.h`, `e1000.c`, `eth.c`

- **TX:** `nic.send()` → writes descriptor ring, updates TDT. Polls for completion (TDH catch-up). Source in `e1000.c:tx()`.
- **RX:** `nic.poll()` → reads descriptor ring from RDH to RDT. Returns one packet per call. Source in `e1000.c:poll()`.
- **eth_rx_poll()** (`eth.c:50-81`): drains all available packets in a `while()` loop, dispatches each by EtherType to registered handlers.
- **NOT thread-safe**: eth_rx_poll() is called from multiple contexts (NIC poll thread, user thread poll loops). With cooperative scheduling this is safe because only one thread runs at a time.

### 1.2 Ethernet Framing

**Source:** `eth.c`

- **eth_send()**: builds Ethernet frame (dst MAC, src MAC, EtherType), pads to 64 bytes minimum, calls `nic.send()`.
- **eth_rx_poll()**: dispatches by EtherType to registered handlers (`eth_register()`).
- **ETH_MAX_HANDLERS**: 8 slots. IPv4 = 0x0800, IPv6 = 0x86DD, ARP = 0x0806.

### 1.3 IPv4 Path

**Source:** `ipv4.c`

- Registered handler for EtherType 0x0800.
- `ipv4_send()`: builds IPv4 header, computes checksum, calls `arp_resolve()` or `eth_send()` for direct/gateway.
- `ipv4_register_handler()`: dispatches to TCP/UDP/ICMP by protocol field.

### 1.4 IPv6 Path

**Source:** `ipv6.c`

- Registered handler for EtherType 0x86DD.
- `ipv6_eth_handler()` (`ipv6.c:28-75`): **learns source IPv6→MAC mapping from every received IPv6 packet** via `ndp_cache_update()`.
- `ipv6_send()` (`ipv6.c:77-116`):
  - For multicast destination (`dst[0] == 0xFF`): derives Ethernet MAC 33:33:XX:XX:XX:XX directly, no NDP.
  - For unicast destination: calls `ndp_resolve(dst, next_mac, 30000)` — 30 second timeout.
  - Calls `eth_send()` with resolved MAC.

### 1.5 NDP (Neighbor Discovery)

**Source:** `ndp.c`

- **Cache**: 16 entries (`NDP_CACHE_SIZE`), LRU eviction, indexed by IPv6 address.
- **`ndp_cache_lookup(ipv6, mac)`**: returns 1 if found, 0 if miss.
- **`ndp_cache_update(ipv6, mac)`**: updates existing entry or inserts new.
- **`ndp_resolve(ipv6, mac, timeout_ms)`** (`ndp.c:99-124`):
  1. Check cache → hit: return OK.
  2. Miss: send NS via `icmpv6_send_ns(ipv6)` (solicited-node multicast).
  3. Poll loop: `eth_rx_poll()` + `thread_sleep(50)` until cache hit or timeout.
- **`ndp_make_lladdr(mac, ipv6_out)`**: EUI-64 link-local (`fe80:: + inverted MAC bit 7 + 0xFF 0xFE`).
- **`ndp_make_solicited_node(ipv6, mc_out)`**: `ff02::1:ffXX:XXXX` from last 3 bytes of target.

### 1.6 ICMPv6

**Source:** `icmpv6.c`

- Registered handler for IPv6 next-header 58.
- **Echo reply** (`icmpv6.c:65-81`): type 129, swaps source/dest, echoes payload.
- **NS handler** (`icmpv6.c:95-129`): checks `target == my_ip` before sending NA. NA has flags (Solicited=1, Override=1), target address, TLLAO.
- **NA handler** (`icmpv6.c:131-157`): extracts TLLAO, calls `ndp_cache_update()`.
- **`icmpv6_send_ns()`**: constructs NS with SLLAO, sends to solicited-node multicast.
- **`icmpv6_send_rs()`**: RS to `ff02::2` (all-routers).

### 1.7 TCP

**Source:** `tcp.c`

- Connection table: `tcp_conn_t tcp_conns[TCP_MAX_CONN]` (global, fixed array).
- States: LISTEN, SYN_SENT, SYN_RECEIVED, ESTABLISHED, FIN_WAIT1, FIN_WAIT2, CLOSE_WAIT, CLOSING, LAST_ACK, TIME_WAIT, CLOSED.
- **`tcp_find_conn()`** (`tcp.c:47-64`): matches 5-tuple (af, src_ip, src_port, dst_port). IPv6 match fixed in past session.
- **`tcp_handle_common()`** (`tcp.c:110-306`): dispatches by connection state.
- **`tcp_conn_connect()`** (`tcp.c:490-538`): sends SYN, polls until ESTABLISHED or timeout, retransmits SYN every 5 seconds.
- **`tcp_conn_recv()`** (`tcp.c:573-593`): polls until `recv_done`.
- **`tcp_conn_close()`**: sends FIN, state machine transitions through FIN handshake.
- **recv_data → recv_buf fix**: Fixed use-after-return of `eth_rx_poll()` stack buffer in past session.
- **TCP_TIME_WAIT handler** (`tcp.c:298-301`): sets state=CLOSED, used=0.

### 1.8 UDP

**Source:** `udp.c`, `udp.h`

- Endpoint table: `udp_endpoint_t udp_endpoints[UDP_MAX_ENDPOINTS]` (16 max).
- **Endpoint-based receive queue**: each endpoint has a ring buffer (`queue[16]`) of datagrams.
- **`udp_sendto()`** (`udp.c:58-89`): builds UDP header, computes checksum (v4 or v6), calls `ipv4_send()` or `ipv6_send()`.
- **`udp_handle_common()`** (`udp.c:179-197`): matches incoming by (af, dst_port), enqueues to endpoint.
- **`udp_endpoint_dequeue()`** (`udp.c:150-175`): polls `eth_rx_poll()` + `thread_sleep(50)` until data arrives or timeout.
- **`udp_endpoint_enqueue()`** (`udp.c:134-148`): if queue full, drop silently (no overflow handling).

### 1.9 Socket Layer

**Source:** `net.c`, `net.h`

- **`socket_t`**: refcounted, ops table dispatch (TCP ops or UDP ops).
- **FD table**: `net_sockets[32]`, indexed by fd 0-31.
- **`socket_alloc()`** (`net.c:369-393`): creates TCP or UDP socket.
- **TCP ops**: wrap `tcp_conn_*` functions.
- **UDP ops**:
  - `udp_sock_bind()`: creates endpoint, registers.
  - `udp_sock_sendto()`: auto-binds if needed, calls `udp_sendto()`.
  - `udp_sock_recvfrom()`: calls `udp_endpoint_dequeue()`.
  - `udp_sock_close()`: calls `udp_unbind_endpoint()`.
- **`udp_sock_setsockopt()`** (`net.c:330-345`): handles `SOL_SOCKET/SO_RCVTIMEO`; sets `s->recv_timeout` and `ep->recv_timeout`.

### 1.10 Userspace Syscall Flow

**Source:** `syscall.c`, `unistd.c` (libuser)

- **Socket syscalls** (38-48): `socket/bind/connect/listen/accept/send/recv/sendto/recvfrom/setsockopt/getsockopt`.
- **`af_from_user()`** (`syscall.c:879-885`): user AF_INET (2) → kernel AF_INET (4), user AF_INET6 (10) → kernel AF_INET6 (6).
- **User→kernel copy**: via `copy_from_user()` with SMAP + range validation.
- **`sys_close()`** (`syscall.c:213-225`): checks `sock_lookup(fd)` first, falls through to `vfs_close()`.

### 1.11 Scheduler

**Source:** `sched.c`, `sched.h`

- **Run queues**: one per priority (256 levels), doubly-linked list, bitmap for O(1) highest-prio lookup.
- **`sched_add_thread(t)`** (`sched.c:116-135`): appends to tail of `run_queues[t->priority]`. Sets `state = THREAD_READY`.
- **`sched_remove_thread(t)`** (`sched.c:137-155`): unlinks from queue.
- **`pick_next()`** (`sched.c:157-180`): removes and returns head of the highest-priority non-empty queue. **Clears `rq_next` and `rq_prev`** on returned thread.
- **`schedule()`** (`sched.c:186-224`): pick_next() → if next != current: add current to run queue (if RUNNING) → switch_context.
- **`thread_exit()`** (`sched.c:279-301`): state=ZOMBIE, wake join_queue, `pick_next()` directly (NOT `schedule()`), switch_context. **Does NOT call `sched_remove_thread()`**.
- **`sched_block(wq)`** (`sched.c:323-336`): if state is READY or RUNNING, call `sched_remove_thread()`. Set BLOCKED, add to wait queue, call `schedule()`.
- **`sched_wake(wq)`** (`sched.c:338-355`): move all waiters to READY, add to run queue via `sched_add_thread()`.
- **`thread_sleep(ms)`** (`sched.c:303-311`): set SLEEPING + wakeup_tick, call `thread_yield()` → `schedule()`.

### 1.12 Process + Thread Creation

**Source:** `process.c`

- **`process_exec()`** (`process.c:113-345`):
  1. ELF load (main program + optional interpreter).
  2. Allocate user stack page, map into process page table.
  3. Allocate `thread_t` page.
  4. Allocate kernel stack (16KB).
  5. Build kernel stack layout: iretq frame → pre-iretq push → callee-save slots → `user_thread_entry` return address.
  6. Set `tcb->cr3 = proc->cr3`, `tcb->state = THREAD_CREATED`.
  7. `all_threads_add(tcb)` → `sched_add_thread(tcb)`.

- **`user_thread_entry`** (asm in `boot.S` or `ctx.S`): pops iretq frame → iretq to user mode.
- **`thread_create(func, arg)`** (`sched.c:234-277`): allocates TCB + kernel stack, set up switch_context frame with `thread_trampoline`.

---

## 2. CONFIRMED FIXES

### 2.1 tcp_find_conn missing IPv6 match

- **Bug description**: `tcp_find_conn()` only compared remote IP for AF_INET. IPv6 path never set `match`.
- **Root cause**: Missing `kmemcmp` for IPv6 address comparison (tcp.c:47-64).
- **Files changed**: `tcp.c`, `tcp.h`
- **Evidence**: `test_tcp_find_conn_ipv6` regression test added. Without fix, IPv6 5-tuple lookup always returned NULL.

### 2.2 ndp_resolve never sent NS

- **Bug description**: `ndp_resolve()` returned `ERR_TIMEOUT` immediately on cache miss without sending NS.
- **Root cause**: NS transmission code was missing before the poll loop.
- **Files changed**: `ndp.c`
- **Evidence**: `test_ndp_cache_miss` regression test verifies resolve→fail→update→resolve cycle.

### 2.3 NS handler responded to all targets

- **Bug description**: NS handler sent NA for every received NS, regardless of target.
- **Root cause**: Missing `target == my_ip` check in `icmpv6.c:96-103`.
- **Files changed**: `icmpv6.c`
- **Evidence**: `test_icmpv6_ns_parse` regression test verifies target at correct offset.

### 2.4 Socket refcount use-after-free

- **Bug description**: `socket_release()` freed socket before replacing `proto` in `tcp_sock_accept`.
- **Root cause**: `tcp_conn_destroy()` was called on the auto-allocated proto, then the same pointer was replaced with the accepted child.
- **Files changed**: `net.c`
- **Evidence**: `test_socket_refcount` regression test verifies alloc→retain→release→register cycle.

### 2.5 UDP callback-based receive replaced with endpoint queue

- **Bug description**: UDP used callback table (`udp_listen()`), which didn't work with blocking recvfrom.
- **Root cause**: Architecture mismatch; callback-based dispatch cannot support `SYS_RECVFROM`.
- **Files changed**: `udp.c`, `udp.h`, `net.c`, `net.h`
- **Evidence**: `test_udp_queue_roundtrip` regression test.

### 2.6 recv_data use-after-return

- **Bug description**: `tcp_input` stored raw pointer to `eth_rx_poll()`'s stack buffer (`buf[1518]`). After `eth_rx_poll` returned, pointer dangled.
- **Root cause**: `const uint8_t* recv_data` pointed into stack frame.
- **Files changed**: `tcp.c`, `tcp.h`
- **Evidence**: `recv_data` replaced with `uint8_t recv_buf[TCP_MSS]`, payload copied in `tcp_input`.

### 2.7 tcp_sock_close immediate destruction

- **Bug description**: `tcp_sock_close()` called `tcp_conn_destroy()` immediately after `tcp_conn_close()`, breaking FIN handshake.
- **Root cause**: `tcp_conn_destroy()` freed connection before FIN exchange completed.
- **Files changed**: `net.c`
- **Evidence**: TCP FIN handshake now completes naturally through state machine.

### 2.8 Missing TCP_TIME_WAIT handler

- **Bug description**: `tcp_input` had no case for `TCP_TIME_WAIT`, so connections never transitioned to CLOSED.
- **Root cause**: Switch statement fell through to default.
- **Files changed**: `tcp.c:290-301`
- **Evidence**: Active close now properly transitions to CLOSED.

### 2.9 PID stamp in PML4 causing GP fault

- **Bug description**: PID stamp at PML4[255] (e.g., value 3) has bit 0 set → appears as present page-table entry with `pdpt_phys = 0` → kernel reads physical page 0 (BIOS IVT) as a PDPT → non-canonical address → GP fault.
- **Root cause**: `vmm_free_user_pages` misinterpreted PID stamp as page-table entry.
- **Files changed**: `process.c:405` (clear stamp), `vmm.c:146-149` (self-reference check)
- **Evidence**: Stress test (500 iteration spawn/exit) passes without fault.

---

## 3. CURRENT FAILURES — ALL RESOLVED

> All items in this section were resolved in the 2026-06-11 session. The root cause was `sched_remove_thread()` run queue corruption when called on a thread not currently in the queue. Fix: added guard in `sched_remove_thread()` to return early if thread is not in queue (rq_prev==NULL && rq_next==NULL && q->head != t).

### 3.1 UDP echo test: udp_echo.elf produces no output (RESOLVED)

**Observed behavior:**
- udp_echo.elf is spawned (pid=5) on the connector QEMU instance.
- No `[DBG-CTX]` or `[UDP]` messages appear from the process.
- No `[NET] /udp_echo.elf exited with code X` message.
- Process eventually killed by 180s timeout.
- TCP echo test (tcp_echo.elf, pid=4) completes successfully on the same instance.

**Evidence:**
- Console log shows: `[NET] /udp_echo.elf spawned, pid=5` followed by NIC poll thread messages, then 180s timeout.
- No user-space output (socket(), sendto(), recvfrom() all produce `[UDP]` prefix messages in user program).
- tcp_echo.elf (same spawning mechanism, same execution environment) works reliably.

**Last confirmed working point:**
- `process_exec()` for udp_echo.elf completes (returns ERR_OK).
- `sched_add_thread(tcb)` in process_exec adds the udp_echo thread to the run queue.
- The log line `[NET] /udp_echo.elf spawned, pid=5` is printed after process_exec returns.

**First unknown point:**
- After `sched_block(&udproc->exit_waiters)` is called from main_nic_task, the udp_echo thread is never scheduled.
- The scheduler never switches to the udp_echo user thread.
- `user_thread_entry` (which prints `[DBG-CTX]`) is never reached.

**Relevant files:**
- `os/src/kernel/main.c:698-727` — UDP echo spawning code
- `os/src/kernel/process.c:113-345` — `process_exec()` thread creation and scheduling
- `os/src/kernel/sched.c:279-301` — `thread_exit()` (preceding tcp_echo thread exit)
- `os/src/kernel/sched.c:323-336` — `sched_block()` called by main_nic_task
- `os/src/kernel/sched.c:157-180` — `pick_next()` run queue selection

**Timing note**: tcp_echo.elf works because a timer interrupt likely fires between `process_exec` and `sched_block`, causing `schedule()` to add main_nic_task to the run queue before `sched_block` corrupts it.

---

## 4. REJECTED HYPOTHESES

### 4.1 NDP resolution timeout blocks sendto

- **Hypothesis**: When the UDP echo server (kernel thread) tries to `udp_sendto` back to the client, NDP resolution for the client's IP times out because the client's MAC is not in the NDP cache.
- **Evidence**: The NDP cache IS pre-seeded on both sides.
  - Listener pre-seeds connector's IP→MAC (`main.c:599-617`).
  - Connector pre-seeds listener's IP→MAC (`main.c:618-637`).
  - Additionally, `ipv6_eth_handler()` learns source IPv6→MAC from every received packet (`ipv6.c:39`).
  - The UDP echo server's `udp_sendto` reply goes to the SENDER's IP (already in cache from received packet or pre-seeding).
- **Verdict**: Rejected. NDP cache is populated for all required destinations.

### 4.2 NIC poll thread starves user thread

- **Hypothesis**: The NIC poll thread runs continuously (eth_rx_poll + thread_sleep(10)) and prevents the scheduler from selecting the user thread.
- **Evidence**: `thread_sleep(10)` yields the CPU. The scheduler (`pick_next()`) selects the highest-priority thread. NIC poll and user threads have the same priority (THREAD_DEF_PRIO=128). The user thread should be selected when the NIC poll thread sleeps.
- **Verdict**: Rejected. Both threads have equal priority; scheduling should alternate.

### 4.3 UDP echo server on connector interferes with client

- **Hypothesis**: The UDP echo server (kernel thread, bound to port 9999) conflicts with the client's socket operations.
- **Evidence**: The echo server and the client bind to different endpoints. The server binds to port 9999. The client auto-binds to an ephemeral port (>49152). The echo reply to the client goes to the ephemeral port, not port 9999. No endpoint conflict.
- **Verdict**: Rejected. Separate ports, no interference.

### 4.4 Interrupt dispatcher fails for IPv6 UDP

- **Hypothesis**: The IPv6 handler registration (next-header 17) is missing, so incoming UDP packets are not dispatched to `udp_handle_common`.
- **Evidence**: `udp_init()` calls `ipv6_register_handler(17, udp_ipv6_handler)` (udp.c:217). TCP echo over IPv6 works, confirming IPv6 dispatch works. ICMPv6 echo reply works (next-header 58). The handler registration mechanism is validated by multiple protocols.
- **Verdict**: Rejected. IPv6 dispatch is confirmed working by TCP and ICMPv6.

### 4.5 eth_rx_poll() hangs in NIC poll thread

- **Hypothesis**: The NIC poll thread's `eth_rx_poll()` hangs in the e1000 `poll()` function, preventing any other thread from running.
- **Evidence**: `eth_rx_poll()` is a simple polling function that reads the e1000 RX descriptor ring and returns. The e1000 `poll()` function (`e1000.c`) reads RDH, checks descriptors, copies data, updates RDT. No blocking operations. The NIC poll thread successfully outputs `[NIC-POLL]` messages, confirming it iterates through its loop.
- **Verdict**: Rejected. eth_rx_poll() is non-blocking.

### 4.6 User thread faults on entry (page fault before DBG-CTX)

- **Hypothesis**: The user thread's `user_thread_entry` faults before reaching the `[DBG-CTX]` printf.
- **Evidence**: A fault in `user_thread_entry` would produce a kernel panic or GP fault message. No such messages appear.
- **Verdict**: Rejected. No fault indicators in logs.

---

## 5. BUG INVESTIGATION: sched_remove_thread run queue corruption — ALL RESOLVED

> All bugs in this section were fixed in the 2026-06-11 session and later hardened in the 2026-07-09 full-system audit. See AGENTS.md for details.

### 5.1 Bug Description

When `sched_block()` is called for a thread that was just picked from the run queue (by `pick_next()`), `sched_remove_thread()` corrupts the run queue because the thread's `rq_prev` and `rq_next` are NULL (cleared by `pick_next()`).

### 5.2 Root Cause

**File:** `sched.c`

**`pick_next()`** (line 176-177):
```c
t->rq_next = NULL;
t->rq_prev = NULL;
```

**`sched_remove_thread()`** (line 142-146):
```c
if (t->rq_prev) t->rq_prev->rq_next = t->rq_next;
else q->head = t->rq_next;     // rq_next is NULL → q->head = NULL

if (t->rq_next) t->rq_next->rq_prev = t->rq_prev;
else q->tail = t->rq_prev;     // rq_prev is NULL → q->tail = NULL
```

When `t->rq_prev == NULL && t->rq_next == NULL` (thread not in queue):
- `q->head` is set to NULL (destroying the queue head)
- `q->tail` is set to NULL (destroying the queue tail)
- `q->count` is decremented

This clears ALL threads from the run queue, not just the intended thread.

### 5.3 Trigger Path

1. main_nic_task (init thread) is scheduled: `pick_next()` removes it from run queue, clears `rq_prev`/`rq_next`.
2. main_nic_task calls `process_exec(udp_echo)` → udp_echo thread added to run queue.
3. main_nic_task calls `sched_block(&udproc->exit_waiters)`.
4. `sched_block()`: `current_thread->state == THREAD_RUNNING` → calls `sched_remove_thread(current_thread)`.
5. `sched_remove_thread(main_nic_task)`: `rq_prev=NULL, rq_next=NULL` → sets `q->head=NULL, q->tail=NULL` → **all threads in the queue are lost**.
6. `schedule()` → `pick_next()` → empty queue → returns `idle_thr`.
7. udp_echo thread is never scheduled.

### 5.4 Why tcp_echo works but udp_echo doesn't

For tcp_echo, a timer interrupt likely fires between `process_exec` and `sched_block`, causing `schedule()` to add main_nic_task to the run queue (as old current_thread before pick_next removes it). This means main_nic_task IS in the run queue when `sched_remove_thread` is called, so it's correctly removed without corruption.

For udp_echo, the timer interrupt timing is different (or absent), so main_nic_task is NOT in the queue when `sched_remove_thread` is called, causing the corruption.

This is a **race condition** dependent on timer interrupt timing.

### 5.5 Fix Applied (Option A) — 2026-06-11

**File:** `sched.c` — `sched_remove_thread()` (line ~137)

Added guard at the top of `sched_remove_thread()`:
```c
/* Guard: if rq_prev == NULL && rq_next == NULL && q->head != t,
 * the thread is not in the run queue (already removed by pick_next).
 * Without this guard, sched_block() on a THREAD_RUNNING thread would
 * set q->head = NULL and q->tail = NULL, wiping all ready threads. */
if (t->rq_prev == NULL && t->rq_next == NULL && q->head != t) {
    hal_restore_irq(flags);
    return;
}
```

**Effect:** The udp_echo process is now correctly scheduled because `sched_block()` no longer destroys the run queue when called on the currently-running init thread between `process_exec()` and the first timer interrupt.

---

## 6. INFRASTRUCTURE

### 6.1 Test Command
```sh
make -C os test-net
```
Runs 5 regression tests without network backend. 120s timeout.

### 6.2 Two-QEMU Test
```sh
make -C os test-net-2qemu
```
Automated TCP+UDP echo validation between two QEMU instances over socket backend.

### 6.3 Manual Two-QEMU
Terminal 1 (listener):
```
qemu-system-x86_64 -kernel os/build/kernel.elf -serial mon:stdio -m 512M \
  -no-reboot -no-shutdown \
  -netdev socket,id=n1,listen=:12345 -device e1000,netdev=n1
```

Terminal 2 (connector):
```
qemu-system-x86_64 -kernel os/build/kernel.elf -serial mon:stdio -m 512M \
  -no-reboot -no-shutdown \
  -netdev socket,id=n1,connect=127.0.0.1:12345 \
  -device e1000,netdev=n1,mac=52:54:00:12:34:57
```

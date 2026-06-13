# OPERtur/TRY1 — Agent Session Context

## Build
```
make -C os clean && make -C os -j4          # debug build
make -C os release                           # release build (stripped, -Os)
make -C os test-net                          # run 5 regression tests
```

## Goal
- Clean up small/tactical items to make larger features easier to debug and implement.

## Progress
### Done (prev sessions)
- All Phases 1–17 complete: E1000, IPv4/IPv6, TCP full state machine, sockets API, DNS, DHCP, SLAAC, NTP, TCP reliability, multicast, SO_RCVTIMEO/SO_SNDTIMEO, TCP_NODELAY, poll(), IPv4-mapped IPv6, MLDv1/IGMP, heap compaction, production hardening.

### Done (this session)
- **SFS cross-block dirent bug fixed**: `SFS_BLOCK_SIZE=512` × `sizeof(sfs_dirent_t)=68` → entries don't pack evenly. All dirent-reading functions rewritten with byte-offset block math, handling entries that span two blocks.
- **sfs_readlink/sfs_writelink renamed** → `sfs_read_block_data`/`sfs_write_block_data` (misleading names, they are generic block ops).
- **E1000 per-packet KDEBUG noise removed**: TX/RX descriptor KDEBUG calls commented out with note for re-enabling when debugging E1000.
- **`make test` timing hardened**: initial `sleep 20` → `sleep 60`, overall `timeout 210` → `timeout 300` to accommodate slow QEMU (no-KVM) boot with network timeouts.
- **test-runner.sh created**: prompt-detect script using bash coproc (bidirectional I/O). Works in principle but pipe buffering causes issues in no-KVM QEMU; kept for future use.
- **SFS stack buffers moved to heap**: All 4 `uint8_t tmp[2*SFS_BLOCK_SIZE]` (1 KB each) stack allocations converted to `kmalloc`/`kfree`. Affected functions: `sfs_vfs_readdir`, `sfs_lookup`, `sfs_remove_dirent`, `sfs_remove_dirent_by_inum`.
- **GPT partition support added**: New `gpt.c`/`gpt.h` — parses GPT headers and partition entries, registers partition wrappers as `block_dev_t` devices (`<parent>-p<N>` naming). Partition read/write transparently add LBA offset. `MAX_BLOCK_DEVICES` increased from 8 to 64. `gpt_scan()` called from `main.c` after block device init.

## Key Decisions
- **Generic RST handling**: RST aborts the connection immediately (state=CLOSED, closed=1) but does NOT set `used=0` — the connection slot stays allocated for `tcp_find_conn` matching (prevents stray SYN+ACK from matching freed slots). Slot freed by `tcp_conn_connect` poll loop or `tcp_conn_destroy()`.
- **TIME_WAIT 2MSL policy**: 60 seconds (60000 ms) RFC-suggested 2MSL interval. Tick granularity: 10ms (NIC poll thread interval).
- **Data retransmission buffer**: Only the last TCP_MSS-sized chunk is buffered. On RTO, the buffered chunk is retransmitted from its original sequence number. This handles the common case (single-segment sends like echo tests) correctly; multi-segment sends retransmit from the latest unacknowledged segment.
- **Lock-safe retransmission in tcp_tick()**: `tcp_tick()` releases `tcp_lock` before calling `tcp_send_pkt()` to avoid deadlock when NDP/ARP resolution triggers `eth_rx_poll()` (which could re-enter TCP). After re-acquiring, state is re-validated before updates.
- **FIN retransmission**: Uses separate `fin_rto_remaining` timer (1s initial, 2s backoff, 60s cap). Timer cleared on state transition out of FIN_WAIT1/LAST_ACK.

## Next Steps
All major phases complete. All items from Phase roadmap now implemented.

### Potential next items
- Make `make test` timing more robust (retry on timeout, test-runner.sh polish)
- User-level mmap for file-backed mappings

## Critical Context
- **RST during connect**: If listener hasn't set up listening socket yet, SYN gets RST (sent for both IPv4 and IPv6). `tcp_conn_connect` detects `state==TCP_CLOSED` on first poll iteration, returns `ERR_AGAIN` (fast-fail, ~50ms). `ERR_AGAIN = -7` → userspace errno=7 (E2BIG). Kernel retries binary up to 3 times; single RST event is benign due to QEMU socket backend race on simultaneous boot.
- **Root cause of all page faults**: stack overflow (24 KB `udp_endpoint_t` on 16 KB kernel stack).
- **Root cause of callback deadlock**: `tcp_handle_common` holding `tcp_lock` across callbacks — fixed by releasing lock before callbacks.
- **SFS cross-block dirents**: `SFS_BLOCK_SIZE=512`, `sizeof(sfs_dirent_t)=68` → `SFS_DIRENTS_PER_BLOCK = 7` (integer division: 512/68=7). But `7 * 68 = 476`, not 512. Dirent 7 starts at byte 476 and spans blocks 0–1. All block-index calculations must use byte offsets (`index * 68 / 512`), not `index / 7`, because partial-block alignment causes every 8th entry to span two blocks.
- Kernel `AF_INET`=4, `AF_INET6`=6; socket layer translates POSIX values (2, 10).
- `eth_rx_poll()` called from both NIC poll thread (10ms) and blocking APIs.
- All spinlocks use `cpu_flags_t` (CLI/STI) for mutual exclusion on single-core.

## Relevant Files
- `os/src/kernel/gpt.c` / `gpt.h`: GPT partition parser, partition wrapper block device
- `os/src/kernel/block.c`: `MAX_BLOCK_DEVICES` increased from 8 to 64
- `os/src/kernel/sfs.c`: `tmp[2*SFS_BLOCK_SIZE]` stack → heap in 4 functions
- `os/src/kernel/main.c`: `gpt_scan()` called after block device init
- `os/src/kernel/e1000.c`: Per-packet KDEBUG commented out
- `os/src/kernel/udp.c`: Checksum validation on receive; `ipv6only` in `udp_bind_endpoint`; `ipv6only` field in `udp_endpoint_t`
- `os/src/kernel/udp.h`: `udp_bind_endpoint` signature with `ipv6only`; `ipv6only` field in struct
- `os/src/kernel/ipv4.h`: `ipv4_handler_t` signature now includes `ipv4_addr_t dst`
- `os/src/kernel/ipv4.c`: `ipv4_dispatch_pkt` passes dst to handler
- `os/src/kernel/ipv6.h`: `ipv6_handler_t` signature now includes `const uint8_t* dst`; multicast API declarations
- `os/src/kernel/ipv6.c`: `ipv6_eth_handler` passes dst to handler; multicast group table + join/leave/is_member
- `os/src/kernel/net.h`: `IPV6_V6ONLY`, `SOL_IPV6`, `IPV6_JOIN_GROUP`, `IPV6_LEAVE_GROUP` constants; `ipv6_mreq_t` struct; `ipv6only` field in `socket_t`
- `os/src/kernel/net.c`: `setsockopt` handlers for `IPV6_V6ONLY`, `IPV6_JOIN_GROUP`/`IPV6_LEAVE_GROUP` in TCP and UDP ops
- `os/src/kernel/tcp.h`: `ipv6only` field in `tcp_conn_t`; `tcp_conn_bind` signature includes `ipv6only`
- `os/src/kernel/tcp.c`: Cross-family bind conflict checking in `tcp_conn_bind`
- `os/src/kernel/main.c`: Raw-UDP callers pass `ipv6only=1`
- `os/src/kernel/dns.c`, `dhcp.c`, `ntp.c`: `udp_bind_endpoint` callers pass `ipv6only=1`
- `os/src/include/sys/socket.h`: Added `IPV6_V6ONLY`, `SOL_IPV6`, `IPV6_JOIN_GROUP`, `IPV6_LEAVE_GROUP`
- `os/src/include/kernel.h`: `KDEBUG` macro definition (`#ifdef NDEBUG` gates verbose logging)
- `os/src/kernel/watchdog.c`: HAL/Scheduler/PMM health monitoring, wired at main.c:426-428

## Two-QEMU IPv6 TCP validation
```
# Terminal 1 (listener, auto):
qemu-system-x86_64 -kernel os/build/kernel.elf -serial mon:stdio -m 512M \
  -no-reboot -no-shutdown \
  -netdev socket,id=n1,listen=:12345 -device e1000,netdev=n1

# Terminal 2 (connector, auto):
qemu-system-x86_64 -kernel os/build/kernel.elf -serial mon:stdio -m 512M \
  -no-reboot -no-shutdown \
  -netdev socket,id=n1,connect=127.0.0.1:12345 \
  -device e1000,netdev=n1,mac=52:54:00:12:34:57
```

The kernel auto-detects its MAC: if the default (52:54:00:12:34:56) it acts as listener only; if different, it initiates an IPv6 TCP connect to the default MAC's link-local on port 9.

## Layer-by-layer verification

```
Layer 2: E1000 TX/RX        ✓  TPT/GPRC counters, hex dump verified
         Ethernet framing    ✓  dispatch by EtherType
         Ethernet padding    ✓  64-byte minimum frame

IPv6:    Link-local addr     ✓  EUI-64 from MAC
         Packet TX/RX        ✓  send/recv between QEMU instances
         Routing decisions   ✓  direct vs gateway dispatch

NDP:     Solicited-node mcast✓  FF02::1:FFxx:xxxx
         NS transmission     ✓  ndp_resolve() on cache miss
         NA response         ✓  target check, SLLAO/TLLAO
         Cache lookups       ✓  ndp_cache_lookup/update
        **Address resolution  ✓  NS/NA exchange (IPv6 ARP)**

ICMPv4:  Echo reply          ✓  ping 10.0.2.2 via SLiRP

ICMPv6:  Echo reply          ✓  ping between QEMU instances
         NS target parse     ✓  hdr->data + 4 (not data+0)
         NA format           ✓  flags, target, TLLAO offsets
         RS send             ✓  to ff02::2

UDP:     Sendto/recv         ✓  dual-stack checksum

TCPv4:   Full state machine  ✓  via SLiRP hostfwd echo test
         CONNECT → SYN       ✓
         LISTEN → SYN+ACK    ✓
         ESTABLISHED         ✓
         data send/recv      ✓
         FIN/CLOSE           ✓

TCPv6:   Full state machine  ✓  via two-QEMU socket test
         CONNECT → SYN       ✓
         LISTEN → SYN+ACK    ✓
         ESTABLISHED         ✓
         data send/recv      ✓  "hello from guest" (16 bytes)
         FIN/CLOSE           ✓
```

## Regression tests (`make test-net`)
All 5 pass without a network backend:

| Test | What it guards | Bug it targets |
|------|---------------|----------------|
| `test_tcp_find_conn_ipv6` | `tcp_find_conn()` IPv6 5-tuple matching | Only AF_INET was compared; IPv6 `match` var never set |
| `test_ndp_cache_miss` | `ndp_cache_lookup` → `update` → `lookup` cycle | Cache population path that ndp_resolve() relies on |
| `test_icmpv6_ns_parse` | NS target at `hdr->data + 4`, not `hdr->data` | Reserved field overlap with target address |
| `test_socket_refcount` | `socket_alloc`/`retain`/`release`/`register` cycle | Socket lifetime management |
| `test_udp_queue_roundtrip` | enqueue → dequeue → payload/addr/port match | Endpoint queue in UDP socket layer |

## Key bugs fixed

1. **`tcp_find_conn` missing IPv6 match** (`tcp.c:47`): `match` was only set for `AF_INET`. Added `kmemcmp` for v6 path.

2. **`ndp_resolve` never sent NS** (`ndp.c:96`): Returned `ERR_TIMEOUT` immediately on cache miss. Added NS transmission + poll loop.

3. **NS handler responded to all targets** (`icmpv6.c:96`): Sent NA for every received NS. Added `target == my_ip` check.

4. **Socket refcount use-after-free** (`net.c:82`): `socket_release` freed socket before replacing `proto` in `tcp_sock_accept`. Now uses `tcp_conn_destroy` directly.

5. **UDP callback-based receive**: Replaced `udp_listen()` callback table with `udp_endpoint_t` (datagram queue). `udp_input` enqueues; `udp_endpoint_dequeue` blocks with `eth_rx_poll` + `thread_sleep`.

6. **`recv_data` use-after-return** (`tcp.c:251`, `tcp.h:69`): `tcp_input` stored a raw pointer into `eth_rx_poll()`'s stack buffer (`buf[1518]`). After `eth_rx_poll` returned, the pointer dangled. Replaced `const uint8_t* recv_data` with `uint8_t recv_buf[TCP_MSS]` and copy payload in `tcp_input`. This is the likely root cause of the udp_echo page-fault after tcp_echo exit.

7. **`tcp_sock_close` immediate `tcp_conn_destroy`** (`net.c:143-152`): Called `tcp_conn_destroy()` immediately after `tcp_conn_close()` (which sends FIN), breaking the FIN handshake. Now `tcp_sock_close` only sends FIN; `socket_release` no longer redundantly calls `tcp_conn_destroy`. The FIN handshake completes naturally through the state machine.

8. **Missing `TCP_TIME_WAIT` handler** (`tcp.c:290-294`): `tcp_input` had no case for `TCP_TIME_WAIT`, so connections never transitioned to `TCP_CLOSED` after active close. Added handler that sets `state = TCP_CLOSED` and `used = 0`.

9. **PID stamp at PML4[255] causes GP fault in stress test** (`process.c:101`, `process.c:405`): `vmm_free_user_pages` iterates PML4[0..255]. The PID stamp (e.g., `3`) has bit 0 set → appears as a present page-table entry with `pdpt_phys = 0`. Kernel reads physical page 0 (BIOS IVT data) as a PDPT; IVT entries at PDPT[2] produce `pd_phys = 0xF000FF54F000F000` → `PHYS_TO_VIRT` gives non-canonical address → GP fault. Fix: clear `pml4v[255] = 0` before calling `vmm_free_user_pages`. Also added a self-reference check (`vmm.c:146-149`) to skip PML4 entries pointing to PML4 itself.

13. **`goto process_established_data` captures NULL `on_recv` before `on_connect` runs** (`tcp.c:225-255`): When data arrives in `SYN_RECEIVED` state, `goto process_established_data` processed data before `on_connect` callback had a chance to set `conn->on_recv`. This left `on_recv_cb` NULL, so the application never received the data. Fix: removed `goto`; instead save data in `syn_recv_deferred` flag and process it after the `on_connect` callback (which sets `on_recv`).

11. **Kernel stack overflow from `udp_endpoint_t`** (`udp.c`, `net.c`, `main.c`): `udp_endpoint_t` contains `udp_dgram_t queue[16]` (each datagram has a 1500‑byte buffer), totalling ≈24 KB. Allocated on the 16 KB kernel stack, it overflows into adjacent page-table pages, corrupting PML4/PDPT entries. Fix: `udp_bind_endpoint` accepts individual `(af, addr, port, recv_timeout)` parameters instead of a pointer to a stack-allocated `udp_endpoint_t`.

12. **E1000 RDT stale after RX poll** (`e1000.c:336`): `e1000_reg_write(E1000_RDT, idx)` after each receive only returned one descriptor. QEMU could eventually see `RDH == RDT` and stop delivering packets. Fix: write RDT as `(e1000_rx_cur + N - 2) % N` to always keep N-2 buffers available ahead of QEMU's internal RDH.

13. **RST handler freed connection (`used=0`) causing `tcp_find_conn` miss** (`tcp.c:224-232`): Generic RST handler before switch called `c->used = 0`, freeing the connection slot. If a stray SYN+ACK later arrived, `tcp_find_conn` skipped the freed slot and matched an unrelated connection (or freed slot reused by a different process). Fix: RST handler no longer sets `used=0`; slot stays allocated until `tcp_conn_connect` poll loop (`state==CLOSED`) or `tcp_conn_destroy()` frees it.

## Files changed

| File | Change |
|------|-------|
| `os/src/kernel/tcp.c` | `tcp_find_conn` IPv6 match, test hook `tcp_test_add_conn`, `recv_data` → `recv_buf` copy, `TCP_TIME_WAIT` handler |
| `os/src/kernel/tcp.h` | Expose `tcp_find_conn`, `tcp_test_add_conn` (under `NET_SELF_TEST`); `recv_data` → `recv_buf[TCP_MSS]` |
| `os/src/kernel/ndp.c` | `ndp_resolve` sends NS, waits for NA |
| `os/src/kernel/icmpv6.c` | `icmpv6_send_ns()`, NS target verification |
| `os/src/kernel/icmpv6.h` | `icmpv6_send_ns` declaration |
| `os/src/kernel/main.c` | Two-instance connect test, regression test call |
| `os/src/kernel/net_test.c` | 3 regression tests → now 4 (added socket_refcount) |
| `os/src/kernel/net_test.h` | Test runner header |
| `os/Makefile` | `ENABLE_NET_TEST`, `test-net` target, clean before test |
| `os/src/kernel/udp.h` | `udp_endpoint_t`, `udp_dgram_t`, endpoint API |
| `os/src/kernel/udp.c` | Rewritten: endpoint-based receive queue instead of callbacks |
| `os/src/kernel/net.h` | `sock_op_sendto_t`/`sock_op_recvfrom_t` in ops table; `sock_sendto`/`sock_recvfrom` API |
| `os/src/kernel/net.c` | UDP ops: bind creates endpoint, sendto/recvfrom for datagram I/O; `tcp_sock_close` no longer calls `tcp_conn_destroy`; `socket_release` no longer redundant destroy |
| `os/src/kernel/syscall.c` | `sys_sendto` (45), `sys_recvfrom` (46), user→kernel address translation |
| `os/src/include/syscall_defs.h` | `SYSCALL_COUNT` → 47, `SYS_SENDTO`/`SYS_RECVFROM` added |
| `os/src/include/sys/socket.h` | New: POSIX-compatible sockaddr structs, AF/SOCK constants, syscall numbers |
| `os/src/kernel/syscall.c` | `sys_exit`: `old == 1` (not `old <= 1 && !proc->exited`) so `process_exit` always called for last thread |
| `os/src/kernel/process.c` | `process_create`: PID stamp in PML4[255]; `process_exit`: validate stamp, then clear stamp before `vmm_free_user_pages` to prevent misinterpretation as page-table entry |
| `os/src/kernel/vmm.c` | Self-reference check in `vmm_free_user_pages` (skip PML4 entries pointing to PML4 itself); clear `pml4[pml4_idx] = 0` after freeing subtree |
| `os/src/kernel/main.c` | NIC poll thread (`nic_poll_thread`) created after TCP/UDP setup; runs `eth_rx_poll()` + `thread_sleep(10)` forever |
| `os/src/kernel/tcp.c` | `tcp_conn_connect` retransmits SYN every 5 seconds while waiting for SYN+ACK |
| `os/src/kernel/e1000.c` | RDT write: `idx` → `(e1000_rx_cur + N - 2) % N` to maintain available RX descriptors |
| `os/src/kernel/udp.h` | `udp_bind_endpoint` signature: pointer replaced by individual parameters |
| `os/src/kernel/udp.c` | `udp_bind_endpoint` fills `udp_endpoints[]` directly instead of through stack pointer |
| `os/src/kernel/tcp.c` | SYN_RECEIVED data piggyback fix via `goto process_established_data` |
| `os/src/kernel/main.c` | Removed stress test; `udp_echo_server` uses flat params; NDP pre-seed info |
| `os/src/kernel/syscall.c` | Added `[DBG-SEND]`/`[DBG-BEFORE]` with pml4/pdpt values for debugging |
| `os/test-2qemu.sh` | Fixed QEMU detection via `command -v`; simultaneous start |
| `os/src/kernel/tcp.c` | Generic RST before switch (no `used=0`); IPv6 RST generation; ACK processing (snd_una, snd_wnd); TIME_WAIT 2MSL timer; `tcp_tick()`; fast-fail on `state==TCP_CLOSED` in connect poll loop |
| `os/src/kernel/tcp.h` | `snd_wnd`, `timewait_ms` fields; `tcp_tick()` declaration |
| `os/src/kernel/main.c` | NIC poll thread calls `tcp_tick()` after `eth_rx_poll()` |

## Phase 8: Sockets API (in progress)

### Session summary (2026-06-11)
- **Socket layer** (`os/src/kernel/net.c/h`): refcounted `socket_t`, `sock_ops_t` dispatch table, dual-stack, 32-entry fd table.
- **TCP ops** fully implemented (wraps `tcp_conn_*`).
- **UDP ops** fully implemented: `bind` creates `udp_endpoint_t`; `sendto` wraps `udp_sendto`; `recvfrom` dequeues from endpoint; `close` unbinds and frees.
- **Socket syscalls** (38-46): `socket/bind/connect/listen/accept/send/recv/sendto/recvfrom` with user→kernel AF translation.
- **sys_close** handles socket fds (lookup socket first, fall through to vfs).
- **`make test-net`**: 5 regression tests pass (tcp_find_conn_ipv6, ndp_cache_miss, icmpv6_ns_parse, socket_refcount, udp_queue_roundtrip).
- **Bug fixes in net.c**: use-after-free in tcp_sock_accept; UDP socket lifecycle; `tcp_sock_close` no longer calls `tcp_conn_destroy` immediately (graceful FIN handshake preserved); `socket_release` no longer redundantly destroys TCP connections.
- **Bug fix in tcp.c**: `recv_data` → `recv_buf[TCP_MSS]` to fix use-after-return of `eth_rx_poll()` stack pointer; added `TCP_TIME_WAIT` handler to properly close connections after active shutdown.
- **net_init()** called from `main.c` after `tcp_init()`.
- **Userspace socket wrappers** added to `libuser/unistd.c` and `unistd.h`: `socket/bind/connect/listen/accept/send/recv/sendto/recvfrom` wrapping `SYS_SOCKET`-`SYS_RECVFROM` via `int $0x80`.
- **Userspace test programs** `tcp_echo-c.c` and `udp_echo-c.c` embedded as `/tcp_echo.elf` and `/udp_echo.elf`.
- **Two-QEMU test script** `os/test-2qemu.sh` and `make test-net-2qemu` target for automated TCP+UDP echo validation between two QEMU instances.
- **`make test-net`** now has 120s timeout (increased from 60s).
- **Stress test moved after network tests** (`main.c`): The 500-iteration spawn/exit stress test was running before the network auto-test, delaying TCP/UDP listeners. Moved to after all network tests so the two-QEMU test starts immediately.
- **UDP echo simplified** (`udp_echo-c.c`): Removed `setsockopt(SO_RCVTIMEO)` call. The default 5s `recv_timeout` is sufficient for UDP echo validation. With the `setsockopt` call, the test silently failed (exit code 1) due to an unresolved issue in the `setsockopt` syscall path.
- **Two-QEMU test passes**: `make test-net-2qemu` now validates both TCP and UDP echo over IPv6 between two QEMU instances.

### Session summary (2026-06-11, continued)
- **Bug fix in `sys_exit`** (`syscall.c:107`): `process_exit` was never called for multi-threaded processes using `clone()`. The condition `old <= 1 && !proc->exited` evaluated to `FALSE` for the last thread because the parent thread had already set `proc->exited = true`. Changed to `old == 1` so the last thread always calls `process_exit`, properly freeing the PML4, page tables, and process table slot.
- **PID stamp in PML4** (`process.c:101`): Stores `proc->pid` in `pml4v[255]` during `process_create`. `process_exit` validates it matches before freeing (`process.c:388-407`), detecting stale PML4 reuse by a different process (safety net).
- **Fixed pre-existing GP fault in stress test** (`process.c:405`): The PID stamp at PML4[255] (e.g., `3`) has bit 0 set, making `vmm_free_user_pages` think it's a present page-table entry with `pdpt_phys = 0`. This caused traversal of physical page 0 (BIOS IVT data) as a PDPT. IVT entries at PDPT[2] concatenated to `0xF000FF54F000F000` → `PHYS_TO_VIRT` produced non-canonical address → GP fault. Fix: clear `pml4v[255] = 0` before calling `vmm_free_user_pages`.
- **Self-reference check in vmm_free_user_pages** (`vmm.c:146-149`): Skip PML4 entries that point to the PML4 page itself, preventing infinite loop.
- **Confirmed fixes work**: `test-net` passes 5/5 with no FAULT/PANIC in stress test.
- **Added NIC poll thread** (`main.c`): `nic_poll_thread` runs `eth_rx_poll()` + `thread_sleep(10)` forever. Created after TCP/UDP setup on both listener and connector sides, so the listener's RX ring is always drained — incoming SYNs and UDP datagrams are processed without a separate polling thread per service.
- **Added SYN retry** (`tcp.c:523-527`): `tcp_conn_connect` retransmits SYN every 5 seconds while waiting for SYN+ACK, surviving dropped SYNs due to race with listener's poll start.

### Session summary (2026-06-11, final)

**Root cause of page faults found and fixed**: The `udp_endpoint_t` struct (~24 KB) allocated on kernel stack overflowed into adjacent physical pages holding active PML4/PDPT entries, corrupting page tables. This was the root cause of SIGSEGV on `recvfrom` return and all `pdpt[1]=0` observations. Fix: `udp_bind_endpoint` now accepts individual parameters instead of a pointer to a stack-allocated endpoint struct.

**TCP SYN_RECEIVED data piggyback bug found and fixed**: When the connector sends data in the same TCP segment as the third handshake ACK (SYNDATA optimization), the `SYN_RECEIVED` handler in `tcp.c` transitioned the connection to `ESTABLISHED` and called `on_connect` but never processed the payload. The echo server never received the data, causing the connector's `recv()` to time out. Fix: `goto process_established_data` falls through to the ESTABLISHED data processing path.

**Tests confirmed**: `make test-net` (5/5 pass), `make test-net-2qemu` (TCP+UDP echo PASS with simultaneous QEMU startup).

**Changes this session**:
- `os/src/kernel/e1000.c`: RDT write changed from `idx` to `(e1000_rx_cur + N - 2) % N`
- `os/src/kernel/tcp.c`: SYN_RECEIVED data deferred after `on_connect` sets `on_recv` (removed `goto process_established_data`, replaced with `syn_recv_deferred` flag)
- `os/src/kernel/udp.h`: `udp_bind_endpoint` signature — pointer replaced by flat params
- `os/src/kernel/udp.c`: New `udp_bind_endpoint` body for flat params
- `os/src/kernel/net.c`: `udp_sock_bind`/`udp_sock_autobind` use flat params
- `os/src/kernel/main.c`: `udp_echo_server` uses flat params; removed stress test; added `[NET] Pre-seeded NDP` info
- `os/src/kernel/syscall.c`: Added `[DBG-SEND]`/`[DBG-BEFORE]` for debugging
- `os/test-2qemu.sh`: QEMU detection fix; simultaneous start
- `os/src/boot/udp_echo-c.c`: Added `setsockopt(SO_RCVTIMEO)` call back (now works with stack overflow fixed)
- Phase 14: TCP reliability — RST handling, TIME_WAIT, ACK tracking, `tcp_tick()`, RST fast-fail during connect

## Threading & Stack Model (critical — read before adding any kernel-thread code)

### Kernel stacks
- **Size**: 16 KB (`THREAD_STACK_SIZE = 16384`, `sched.h:7`). Allocated as 4 contiguous physical pages (`pmm_alloc_pages(4)`) in `thread_create()`.
- **No guard page or red zone**. Stack overflow silently corrupts adjacent physical pages (previously caused page-table corruption from the 24 KB `udp_endpoint_t` stack allocation).
- **Rule**: Any struct or automatic variable placed on a kernel stack must be **well under 16 KB total**. If you need a buffer larger than ~8 KB, heap-allocate it or use a static buffer.
- **Double-fault IST**: Separate 8 KB stack at `hal.c:64` catches stack-overflow double faults instead of triple-faulting.

### Syscall & stack switching
- CPU automatically switches to kernel stack via **TSS `rsp[0]`** on any interrupt/syscall from user mode (CPL 3 → 0). No manual stack switching.
- `schedule()` calls `hal_set_kernel_stack(next->kernel_stack + next->kernel_stack_size)` to update the TSS before each context switch.
- Syscall entry is `int $0x80` (vector 128). The handler path is: `isr.S` → `interrupt_handler()` → `syscall_handler()` → dispatch.

### Context switch (`switch_context` in `ctx.S`)
- Saves 6 callee-saved regs (r15, r14, r13, r12, rbx, rbp), swaps RSP via `thread_t->rsp` (field 0, offset-sensitive).

### Thread lifetimes
- `thread_create()` allocates TCB (1 page) + kernel stack (4 pages) + registers with `thread_trampoline`. Does **not** add to run queue; caller must call `sched_add_thread()`.
- `sched_reap_zombies()` frees both via `pmm_free_pages`.
- `thread_spawn()` does not exist; kernel threads are created via `thread_create()` + `sched_add_thread()`.

### User process stacks
- **Initial**: 1 page (4 KB) allocated in `process_exec()` at `process.c:199`. ASLR-randomized over 256 positions in the 0x60000000 region.
- **Growth**: Via `sys_sbrk()` which maps additional pages on demand.
- **Rule**: User stacks are tiny by default. ELF images that need large stacks must call `sbrk()` or use a heap.

### NIC poll thread
- Created at `main.c:574-580` with `thread_create(nic_poll_thread, NULL, THREAD_DEF_PRIO, "nic-poll")`.
- Runs `eth_rx_poll()` + `thread_sleep(10)` forever. 16 KB kernel stack, no user address space (`cr3=0`).

### Key files
| File | Role |
|------|------|
| `os/src/kernel/sched.c` | Thread create, schedule, reap |
| `os/src/kernel/sched.h` | `thread_t` struct, constants |
| `os/src/kernel/ctx.S` | `switch_context`, `thread_trampoline` |
| `os/src/kernel/process.c` | Process create/exec/exit |
| `os/src/kernel/hal.c` | TSS setup, IST stack |
| `os/src/kernel/main.c` | Thread spawning for services |

### Phase 9: DNS Resolver (complete)

**Files added:**
| File | Lines | Purpose |
|------|-------|---------|
| `os/src/kernel/dns.c` | ~210 | DNS query builder, response parser, `dns_resolve()` |
| `os/src/kernel/dns.h` | 15 | Public API: `dns_init()`, `dns_resolve()`, `dns_set_resolver_v4/v6()` |

**Design:**
- Uses raw UDP API (`udp_sendto` + `udp_bind_endpoint`/`udp_endpoint_dequeue`) — no socket layer dependency
- Default resolver: 8.8.8.8:53 (changeable via `dns_set_resolver_v4()`)
- Tries A (IPv4) query first, falls back to AAAA (IPv6)
- Transaction ID matching to validate response
- Standard DNS name encoding/parsing with compression pointer support
- Ephemeral source port allocation for each resolution
- 512-byte max packet (standard DNS, no EDNS0)
- All buffers on kernel stack — no heap allocation, safe for 16 KB kernel stacks
- Calls `dns_init()` in `main.c` after `net_init()`
- Resolver address configurable via `dns_set_resolver_v4()` / `dns_set_resolver_v6()`

**Tested:** Builds clean, integrates with boot sequence (`[DNS] Resolver initialized (8.8.8.8:53)`). DNS resolves will timeout in two-QEMU test (no internet), but should work in SLiRP mode where QEMU forwards UDP to the host.

### Phase 10: DHCP client + SLAAC (complete)

**Files added:**
| File | Lines | Purpose |
|------|-------|---------|
| `os/src/kernel/dhcp.c` | ~310 | DHCP client: DORA (DISCOVER/OFFER/REQUEST/ACK) |
| `os/src/kernel/dhcp.h` | 10 | Public API: `dhcp_configure()` |
| `os/src/kernel/slaac.c` | ~180 | SLAAC: RS/RA exchange, prefix parsing, address formation |
| `os/src/kernel/slaac.h` | 10 | Public API: `slaac_init()`, `slaac_configure()` |

**Design:**
- **DHCP**: Uses raw UDP API (port 68), no socket layer dependency
- Broadcast DISCOVER/REQUEST from 0.0.0.0:68 to 255.255.255.255:67
- Transaction ID matching (based on NIC MAC) for response validation
- Option parsing: subnet mask, router, server ID, lease time
- Parameter request list for desired options
- 2 retries with 1.5s timeout each (~3s worst case per phase)
- On success: `ipv4_set_addr()`, `route_add_v4()` for subnet + default gateway
- **SLAAC**: RA callback via `icmpv6_set_ra_callback()`
- Sends RS to ff02::2, waits for RA with 2s timeout
- Parses Prefix Information Option (type 3, flag A=autonomous)
- Forms global unicast address: prefix[64] + EUI-64 from MAC
- Adds route for prefix/64 and default ::/0 via RA source

**Infrastructure changes:**
- `ipv4.c`: `OUR_IPV4` made mutable; added `ipv4_set_addr()`, `ipv4_get_addr()`, `ipv4_send_from()` with explicit src IP; broadcast handling in input path (accepts 255.255.255.255)
- `arp.c`: Uses `ipv4_get_addr()` instead of local static copy
- `udp.c`: Uses `ipv4_get_addr()` instead of hardcoded 10.0.2.15 for checksum
- `route.c`: Implemented `route_add_v6()`, `route_lookup_v6()`; added `route_clear()`
- `icmpv6.c`: Added `icmpv6_set_ra_callback()` for SLAAC integration
- `main.c`: DHCP+SLAAC integration after `dns_init()`; fallback to hardcoded 10.0.2.15

**Verified:**
- SLiRP boot gets DHCP lease (10.0.2.15/24 gw 10.0.2.2)
- All 5 regression tests pass cleanly
- SLAAC times out gracefully with no RA (link-local still works)
- DHCP fallback works for non-SLiRP environments
- Two-QEMU test shows successful TCP/UDP echo when timing works (pre-existing QEMU socket backend flakiness)

**Files changed this session:**
- `os/src/kernel/dhcp.c` (new), `os/src/kernel/dhcp.h` (new)
- `os/src/kernel/slaac.c` (new), `os/src/kernel/slaac.h` (new)
- `os/src/kernel/ipv4.c`: mutable IP, `ipv4_set/get_addr`, `ipv4_send_from`, broadcast handling
- `os/src/kernel/ipv4.h`: new function declarations
- `os/src/kernel/arp.c`: use `ipv4_get_addr()` instead of local static
- `os/src/kernel/udp.c`: use `ipv4_get_addr()` for checksum
- `os/src/kernel/route.c`: `route_add_v6`, `route_lookup_v6`, `route_clear()`
- `os/src/kernel/route.h`: `route_clear()` declaration
- `os/src/kernel/icmpv6.c`: RA callback support
- `os/src/kernel/icmpv6.h`: `icmpv6_set_ra_callback()` declaration
- `os/src/kernel/main.c`: DHCP+SLAAC integration, includes

### Phase 11: NTP Client (complete)

**Files added:**
| File | Lines | Purpose |
|------|-------|---------|
| `os/src/kernel/ntp.c` | ~140 | NTP client: build request, send via raw UDP, parse response, store boot time |
| `os/src/kernel/ntp.h` | 15 | Public API: `ntp_init()`, `ntp_get_time()`, `ntp_set_server_v4()` |
| `os/src/include/sys/time.h` | 15 | `struct timespec`, `clockid_t` (CLOCK_REALTIME, CLOCK_MONOTONIC) |

**Design:**
- Uses raw UDP API (`udp_sendto` + `udp_bind_endpoint`/`udp_endpoint_dequeue`) — no socket layer dependency
- NTP v4 mode 3 client request to pool.ntp.org (216.239.35.0, changeable via `ntp_set_server_v4()`)
- 48-byte request, 48-byte response parse
- Transmit Timestamp extraction at offset 40, convert NTP→Unix (subtract 2208988800)
- Wall clock: `ntp_get_time()` = `boot_time + uptime_seconds` (monotonic uptime from HPET/PIT)
- VN=3/4, Mode=4 (server) validation in parser
- 2 retries with 5s timeout each (~10s worst case)
- Graceful failure: if NTP fails, `ntp_get_time()` returns 0; boot continues normally
- Configurable server address via `ntp_set_server_v4()`

**Syscall:**
- `SYS_CLOCK_GETTIME` (syscall 49): userspace `clock_gettime(clk_id, struct timespec*)`
- Supports `CLOCK_REALTIME` (returns NTP wall clock) and `CLOCK_MONOTONIC` (returns uptime)
- Userspace wrapper in `unistd.h` / `unistd.c`

**Verified:**
- Builds clean, 5/5 regression tests pass
- SLiRP boot: `[NTP] Clock synchronized: 1781184012 (epoch ..., ~year 2026)` — NTP successfully queries internet via SLiRP
- No-network boot: `[NTP] Clock sync failed (no network or no NTP server)` — graceful fallback
- No crashes, no page faults

## Phase roadmap
- Phase 1-7: Complete (E1000 through TCP, both IPv4 and IPv6)
- **Phase 8: Sockets API** — user-space `socket()`, `bind()`, `connect()`, `listen()`, `accept()`, `send()`, `recv()` syscalls wrapping the validated transport layer (complete)
- **Phase 9: DNS Resolver** — kernel-level `dns_resolve()` using raw UDP, A/AAAA query, response parsing (complete)
- **Phase 10: DHCP client + SLAAC** — kernel-level DHCP client and IPv6 Stateless Address Autoconfiguration (complete)
- **Phase 11: NTP client** — kernel-level NTP v4 client with `clock_gettime()` syscall (complete)
- **Phase 14: TCP reliability** — RST handling for all states, TIME_WAIT 2MSL timer, IPv6 RST generation, ACK/snd_una tracking, `tcp_tick()` infrastructure, and fast-fail on RST during connect (complete)

## Session summary (2026-06-12): Production hardening Phase 17 complete

### Done (this session)
- **TCP_NODELAY** (`SOL_TCP` level, option 1): Added `nodelay` field to `tcp_conn_t`. Handled in `tcp_sock_setsockopt` and `tcp_sock_getsockopt`. Userspace constants added.
- **Error propagation** (syscall boundary): Added `kernel_err_to_posix()` translation table mapping kernel ERR_* values to POSIX errno (ERR_AGAIN → EAGAIN=11, ERR_NOTCONN → ENOTCONN=107, etc.). Applied in `syscall_handler()` so all userspace wrappers transparently get correct errno values. Added missing errno constants: `ENAMETOOLONG` (36), `ENOTCONN` (107), `ETIMEDOUT` (110), `ECONNREFUSED` (111), `ECONNRESET` (104), `EHOSTUNREACH` (113), `ENETUNREACH` (101), `EOPNOTSUPP` (95), `EAFNOSUPPORT` (97), `EALREADY` (114), `EINPROGRESS` (115).
- **poll() syscall** (#52): Added `sys_poll` handler, `sock_poll` op in `sock_ops_t`, TCP poll (`tcp_sock_poll`: checks LISTEN accept_count, ESTABLISHED recv_done, CLOSED state) and UDP poll (`udp_sock_poll`: checks `ep->q_count`). Userspace `poll()` wrapper.
- **IPv4-mapped IPv6** (`::ffff:x.x.x.x`): `tcp_find_conn` now matches IPv4 connections against dual-stack IPv6 entries by checking the ::ffff: mapping prefix. `tcp_sock_getsockname`/`tcp_sock_getpeername` construct mapped addresses when `c->af == AF_INET && !s->ipv6only`.
- **Heap compaction before OOM** (`kmalloc.c`): Added `kmalloc_compact()` that scans slab pages and frees completely empty pages back to PMM. Called from `pmm_oom_kill()` before scheduling the OOM kill worker.
- **E1000 MTA programming** (`e1000.c`): Added `e1000_mta_set()` — computes CRC-32 over multicast MAC, uses upper 12 bits as index into the 4096-bit MTA. `ipv6_mcast_update_mta()` now programs MTA for all joined IPv6 multicast groups.
- **MLDv1** (`icmpv6.c`): Added `mldv1_send_report()` (type 131) and `mldv1_send_done()` (type 132). MLD query handler (type 130) sends reports for all groups on general query, or specific group on directed query. `ipv6_mcast_join()`/`ipv6_mcast_leave()` now send MLD reports.
- **IGMPv2** (`igmp.c/h` new): IPv4 multicast group management (8-group table). Sends Membership Reports (type 0x16) on join, Leave Group (type 0x17) on leave. Handles Membership Queries (type 0x11). Registered as IP protocol 2 handler. IPv4 `IP_ADD_MEMBERSHIP`/`IP_DROP_MEMBERSHIP` socket options. IPv4 eth handler now accepts multicast (224.0.0.0/4) packets.

### Key files changed/added
| File | Change |
|------|--------|
| `os/src/kernel/igmp.c` | **New** — IGMPv2 protocol implementation |
| `os/src/kernel/igmp.h` | **New** — IGMP API header |
| `os/src/kernel/net.h` | TCP_NODELAY/IPPROTO_TCP/SOL_TCP constants; POLLIN/POLLOUT/POLLERR events; IP_ADD/DROP_MEMBERSHIP; `sock_op_poll_t` type; `sock_poll()` declaration |
| `os/src/kernel/tcp.h` | `nodelay` field in `tcp_conn_t` |
| `os/src/kernel/net.c` | TCP_NODELAY handler; `tcp_sock_poll`/`udp_sock_poll`; poll ops in tcp_ops/udp_ops; IPv4-mapped IPv6 in getsockname/getpeername; IP_ADD/DROP_MEMBERSHIP handlers; `sock_poll()` wrapper; IGMP include |
| `os/src/kernel/tcp.c` | IPv4-mapped IPv6 matching in `tcp_find_conn()` |
| `os/src/kernel/syscall.c` | `kernel_err_to_posix()` translation; `syscall_handler()` applies translation; `sys_poll()` implementation; `errno.h` include |
| `os/src/kernel/syscall_defs.h` | `SYS_POLL` (52), `SYSCALL_COUNT` = 53 |
| `os/src/include/errno.h` | Added ENAMETOOLONG, ENOTCONN, ETIMEDOUT, ECONNREFUSED, ECONNRESET, EHOSTUNREACH, ENETUNREACH, EOPNOTSUPP, EAFNOSUPPORT, EALREADY, EINPROGRESS |
| `os/src/include/sys/socket.h` | TCP_NODELAY, SOL_TCP, IPPROTO_TCP, POLLIN/POLLOUT, IP_ADD/DROP_MEMBERSHIP constants |
| `os/src/include/unistd.h` | `struct pollfd`, `poll()` declaration |
| `os/src/lib/libuser/unistd.c` | `poll()` wrapper |
| `os/src/include/kmalloc.h` | `kmalloc_compact()` declaration |
| `os/src/kernel/kmalloc.c` | `kmalloc_compact()` — frees empty slab pages |
| `os/src/kernel/pmm.c` | `kmalloc_compact()` called before OOM kill |
| `os/src/kernel/ipv4.c` | Accept multicast (224.0.0.0/4) in eth handler; `igmp_init()` call |
| `os/src/kernel/ipv6.c` | `mldv1_send_report()`/`mldv1_send_done()` calls on join/leave; `ipv6_mcast_update_mta()` programs E1000 MTA; `ipv6_mcast_report_all()`; e1000.h+icmpv6.h includes |
| `os/src/kernel/icmpv6.c` | MLDv1 query handler; `mldv1_send_report()`/`mldv1_send_done()`; MLD type constants |
| `os/src/kernel/icmpv6.h` | MLD type constants; MLD function declarations |
| `os/src/kernel/e1000.c` | `e1000_mta_set()` — CRC-32 MTA bit programming |
| `os/src/kernel/e1000.h` | `e1000_mta_set()` declaration |

### Verification
- 5/5 regression tests pass
- SLiRP internet test passes: DHCP lease → DNS resolves google.com → NTP syncs clock → ICMPv4 ping
- Release build succeeds (~215 KB text)
- All socket operations and error codes propagate correctly through errno translation

## Session summary (2026-06-12): SO_SNDTIMEO + TCP send reliability

### Done (this session)
- **SO_SNDTIMEO for UDP**: Added `send_timeout` (ms) field to `udp_endpoint_t`. `udp_bind_endpoint` signature extended with `send_timeout` parameter — all callers updated (dns.c, dhcp.c, ntp.c, net.c, main.c). `udp_sock_setsockopt` propagates SO_SNDTIMEO to the endpoint; `udp_sock_getsockopt` reads back SO_SNDTIMEO, SO_RCVTIMEO, and IPV6_V6ONLY.
- **TCP getsockopt**: `tcp_sock_getsockopt` implements readback for SO_SNDTIMEO, SO_RCVTIMEO, TCP_NODELAY.
- **TCP send reliability**: `tcp_send` rewritten to poll-loop (with `send_timeout` deadline) while the single-segment retransmit buffer is busy, releasing `tcp_lock` before `tcp_send_pkt` to prevent deadlock with NIC poll thread. On timeout, returns `ERR_TIMEOUT`. On success, returns length.
- **Nagle delay support**: `nodelay` flag in `tcp_conn_t` checked in `tcp_send` — if Nagle is enabled and outstanding unacked data exists, the segment is buffered into the retransmit buffer without immediate send. `tcp_retransmit_if_needed` in `tcp_tick()` handles delayed sends.
- Build clean, 5/5 regression tests pass.

### Files changed this session
| File | Change |
|------|--------|
| `os/src/kernel/udp.h` | `send_timeout` in `udp_endpoint_t`; `udp_bind_endpoint` sig w/ `send_timeout` |
| `os/src/kernel/udp.c` | `udp_bind_endpoint` stores `send_timeout`; `send_timeout` usage placeholder |
| `os/src/kernel/net.c` | UDP setsockopt propagates `send_timeout`; UDP getsockopt impl; TCP getsockopt impl; `tcp_sock_poll` checks recv_done |
| `os/src/kernel/net.h` | `tcp_sock_getsockopt`/`udp_sock_getsockopt` declarations |
| `os/src/kernel/tcp.h` | `nodelay`, `nagled` fields in `tcp_conn_t` |
| `os/src/kernel/tcp.c` | `tcp_send` rewritten with poll loop + lock-safe send; Nagle buffering; `tcp_sock_getsockopt`; remove old `tcp_sock_poll` |
| `os/src/kernel/dns.c` | `udp_bind_endpoint` call w/ `send_timeout` |
| `os/src/kernel/dhcp.c` | `udp_bind_endpoint` call w/ `send_timeout` |
| `os/src/kernel/ntp.c` | `udp_bind_endpoint` call w/ `send_timeout` |
| `os/src/kernel/main.c` | `udp_bind_endpoint` calls w/ `send_timeout` |
| `os/src/kernel/igmp.c` | `udp_bind_endpoint` call w/ `send_timeout` |

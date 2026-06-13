# Userspace TCP/UDP IPv6 Test Failure — Root Cause Analysis

## Summary

Two bugs prevented userspace TCP/UDP echo tests from working over IPv6 link-local
between two QEMU instances connected via the socket backend.

- **Bug 1 (CRITICAL)**: `e1000_poll()` wrote RDT = rx_cur - 1 as a "flush trigger",
  which told QEMU the RX ring was full. After the kernel-level TCP test consumed
  packets and advanced `e1000_rx_cur`, subsequent `e1000_poll()` calls sealed the ring,
  causing QEMU to silently drop ALL incoming packets. The userspace `connect()` call
  sent SYN but never received SYN+ACK, timing out after 30 seconds.

- **Bug 2**: `udp_sock_close()` called `kfree(ep)` where `ep` pointed into the static
  `udp_endpoints[]` array (not heap-allocated). This corrupted the kernel heap, causing
  a page fault in the UDP echo test.

## Bug 1 — E1000 RX Ring RDT Trigger Write

### File
`os/src/kernel/e1000.c`, `e1000_poll()` function (originally lines 314-317, 340)

### Root Cause

`e1000_poll()` began every call by writing RDT = e1000_rx_cur - 1 as a trigger to force
QEMU's `set_rdt` → `qemu_flush_queued_packets`. This is semantically wrong.

In the E1000 model, the ring is considered full when RDH == RDT. After the kernel-level
TCP test consumed N packets, both RDH and RDT advanced to N % NUM_RX_DESC (the ring
appeared full to hardware). The trigger write then set RDT = (N-1) % NUM, making
RDH (N) > RDT (N-1). QEMU's `e1000_ring_full()` check (RDH == (RDT+1) % len) matched
exactly: N == (N-1+1) % len = N. QEMU silently dropped all incoming packets.

### Why Kernel Test Worked

During the kernel test, `e1000_rx_cur` was 0 at the time of the first SYN+ACK. The
trigger branch `e1000_rx_cur ? (cur-1)%NUM : NUM-1` wrote RDT = NUM-1, which was the
initial value — no damage. The bug only manifested after `e1000_rx_cur` advanced past 0.

### Additionally

The post-consume RDT write (line 340) wrote `RDT = e1000_rx_cur % NUM` after
incrementing `rx_cur`, which also equals `RDH` — another ring-full condition. This
meant every consumed packet immediately re-sealed the ring for the NEXT poll.

### Fix

1. **Deleted** the trigger write block (lines 314-317).
2. **Changed** post-consume RDT write from `e1000_rx_cur % E1000_NUM_RX_DESC` to `idx`
   (the descriptor index that was just freed). This keeps RDT pointing one slot behind
   RDH, so the ring always has at least one free slot visible to QEMU.

```c
// Before (broken):
e1000_reg_write(E1000_RDT, e1000_rx_cur % E1000_NUM_RX_DESC);

// After (fixed):
e1000_reg_write(E1000_RDT, idx);
```

### Effect

After fix: TCP userspace `connect()` → SYN → SYN+ACK received → ESTABLISHED →
data sent → data received → close → PASS.

## Bug 2 — UDP kfree on Static Array Pointer

### File
`os/src/kernel/net.c`, `udp_sock_close()` (previously line ~325)

### Root Cause

`udp_find_endpoint()` returns `&udp_endpoints[i]` — a pointer into a static array.
`udp_sock_close()` called `kfree(ep)` on this pointer, corrupting the kernel heap's
free-list metadata. This caused subsequent allocations or frees to dereference garbage
pointers, resulting in a page fault (error code -11) when the UDP echo test called
`close()`.

### Fix

Removed `kfree(ep)` from `udp_sock_close()`. The endpoint lives in a static array and
is managed by `udp_bind_endpoint()`/`udp_unbind_endpoint()`, not the heap.

## Bug 3 — TCP ACK Storm in ESTABLISHED State

### File
`os/src/kernel/tcp.c`, `tcp_handle_common()` ESTABLISHED case

### Root Cause

The ESTABLISHED handler sent an ACK in response to **every** received packet with
`(flags & TCP_ACK)` set, including bare ACK-only packets (payload_len == 0). This
created an infinite ACK ping-pong: connector receives ACK → sends ACK → listener
receives ACK → sends ACK → ... The connector was stuck in this loop and never reached
the userspace test spawn code.

### Fix

Changed the condition from `if (payload_len > 0 || (flags & TCP_ACK))` to
`if (payload_len > 0)`. Only send ACK in response to data, not to bare ACKs.

## Test Results

### Regression tests (make test-net)
All 5 pass:
- tcp_find_conn_ipv6: PASS
- ndp_cache_miss: PASS
- icmpv6_ns_parse: PASS
- socket_refcount: PASS
- udp_queue_roundtrip: PASS

### Two-QEMU IPv6 TCP Test
```
Connector (non-default MAC 52:54:00:12:34:57):
  Kernel test: SYN → SYN+ACK → ESTABLISHED → data(16B) → FIN → PASS
  Userspace TCP echo (/tcp_echo.elf):
    socket(AF_INET6, SOCK_STREAM) → fd=0
    connect(fe80::5054:ff:fe12:3456:9) → ESTABLISHED
    send → 16 bytes
    recv → 16 bytes: "hello from guest"
    close → PASS
    Exit code: 0

  Userspace UDP echo (/udp_echo.elf):
    socket(AF_INET6, SOCK_DGRAM) → fd=0
    sendto(fe80::5054:ff:fe12:3456:9999) → 24 bytes sent
    Exit code: 1 (recvfrom path issue — separate investigation)

Listener (default MAC 52:54:00:12:34:56):
  TCP listening on port 9 and 80
  UDP echo server on port 9999
  Received and echoed UDP data successfully
  Pre-seeded NDP for connector
```

### Logs
- `connector.log` — full connector serial output
- `listener.log` — full listener serial output

## Files Changed

| File | Change |
|------|--------|
| `os/src/kernel/e1000.c` | Removed RDT trigger write, changed post-consume RDT to `idx` |
| `os/src/kernel/tcp.c` | ESTABLISHED handler: only ACK on data, not bare ACKs |
| `os/src/kernel/net.c` | Removed `kfree(ep)` from `udp_sock_close()` (prior fix) |

## Remaining Work

1. UDP userspace recvfrom not completing — the sendto succeeds at wire level (24 bytes,
   listener receives and echoes) but the userspace program exits with code 1 before
   reaching recvfrom. Possible issue with printf buffering, setsockopt path, or
   the udp_endpoint_dequeue blocking. Needs investigation.

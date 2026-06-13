# OPERtur / TRY1 OS — Implementation Plan

**Version:** 2.0  
**Baseline:** Stages 1–5 complete. Stage 4 gap closed, Stage 5 networking fully implemented. Remaining work starts at Stage 6.  
**Format:** Each task is atomic — it has a single entry point, a defined done state, and no external guesswork required.

---

## How to Read This Document

Each task block contains:
- **What:** Exactly what to implement
- **Files:** Which files to create or modify
- **Done when:** Binary acceptance test — passes or fails, no ambiguity
- **Depends on:** Task(s) that must be complete first

Tasks within a stage can be parallelised unless a dependency is listed.

---

## Stage 4 — Shell and Terminal (Gap Closure) ✅ COMPLETE

### T4.1 — termios ioctl (TCGETS / TCSETS) ✅

**What:** Implement `SYS_IOCTL` (syscall 34) with TCGETATTR/TCSETATTR/TIOCGPGRP/TIOCSPGRP.

**Done when:** A userspace program can call `tcsetattr(0, TCSANOW, &raw_termios)` and subsequent `read(0, buf, 1)` returns after each keypress without waiting for newline. ✅

### T4.2 — SIGTTIN / SIGTTOU ✅

**What:** When a background process reads the TTY, deliver SIGTTIN. When it writes with TOSTOP set, deliver SIGTTOU.

**Done when:** Background `cat /dev/ttyS0` gets stopped (SIGTTIN); `fg` resumes it and it proceeds to read. ✅

### T4.3 — PTY Pseudo-Terminal ✅

**What:** `SYS_PTY_PAIR` (syscall 37) creates master-slave pseudo-terminal pair. 8-slot pool in pty.c. Full line discipline, signal chars, termios ioctl on slave.

**Done when:** A userspace program can open a PTY pair, write to master, read processed output from slave. ✅

---

## Stage 5 — Networking ✅ COMPLETE

### T5.1 — Kernel Crypto Primitives ❌ (Deferred)

**What:** SHA-256, CSPRNG, /dev/random. Required for TLS and capability system.

**Status:** Not implemented. TLS and capability system are Stage 6 dependencies. /dev/random exists in devfs but reads from weak RDRAND directly.

### T5.2 — E1000 NIC Driver ✅

**What:** E1000 PCI detection (0x8086:0x100E), BAR0 MMIO, descriptor rings (32-entry TX + RX), DMA bounce buffers, IRQ 11 handler, MTA multicast programming via CRC-32.

**Files:** `kernel/e1000.c`, `kernel/e1000.h`, `kernel/nic.h`

**Done when:** `eth_test` shell command sends and receives frames between two QEMU instances. ✅

### T5.3 — Ethernet + ARP Layer ✅

**What:** Ethernet frame encode/decode, EtherType dispatch (0x0800=IPv4, 0x0806=ARP, 0x86DD=IPv6). ARP: 8-entry cache, request/response, poll-and-wait resolution.

**Files:** `kernel/eth.c`, `kernel/eth.h`, `kernel/arp.c`, `kernel/arp.h`

**Done when:** ARP request for gateway IP produces a valid ARP reply; cache populated. ✅

### T5.4 — IPv4 Layer ✅

**What:** `ipv4_input`/`ipv4_output`, header checksum, mutable IP address, broadcast handling, `ipv4_set/get_addr`.

**Files:** `kernel/ipv4.c`, `kernel/ipv4.h`

**Done when:** ICMP handler receives echo requests sent to kernel IP. ✅

### T5.5 — ICMPv4 + ICMPv6 ✅

**What:** ICMPv4 echo request/reply (ping). ICMPv6 echo, Neighbor Solicitation/Advertisement, Router Solicitation/Advertisement, MLDv1 reports/queries.

**Files:** `kernel/icmp.c`, `kernel/icmp.h`, `kernel/icmpv6.c`, `kernel/icmpv6.h`

**Done when:** `ping 10.0.2.2` returns replies from QEMU SLiRP gateway; IPv6 ping between QEMU instances works. ✅

### T5.6 — UDP ✅

**What:** `udp_input`/`udp_sendto`, endpoint-based receive queue (16 datagrams), `udp_bind_endpoint`, blocking `udp_endpoint_dequeue`. IPv4 + IPv6. Checksum validation.

**Files:** `kernel/udp.c`, `kernel/udp.h`

**Done when:** Two-QEMU UDP echo test passes; kernel sends/receives IPv6 UDP datagrams. ✅

### T5.7 — TCP ✅

**What:** Full RFC 793 state machine (CLOSED/LISTEN/SYN_SENT/SYN_RECV/ESTABLISHED/FIN_WAIT1-2/TIME_WAIT/CLOSE_WAIT/LAST_ACK). SYN retransmit (5s), data RTO retransmit, FIN retransmit (1s→2s→60s). TIME_WAIT 2MSL (60s). IPv4 + IPv6 5-tuple matching. Lock-safe callbacks. RST handling.

**Files:** `kernel/tcp.c`, `kernel/tcp.h`

**Done when:** Two-QEMU IPv6 TCP echo test passes: connector sends "hello from guest" (16 bytes), listener echoes back, connection closes cleanly. ✅

### T5.8 — Sockets API ✅

**What:** `socket_t` with `sock_ops_t` dispatch table. 9 socket syscalls (SYS_SOCKET=35 through SYS_GETPEERNAME=48). TCP ops: bind/connect/listen/accept/send/recv/close/poll/getsockname/getpeername. UDP ops: bind/sendto/recvfrom/close/poll. Dual-stack (AF_INET/AF_INET6). IPv4-mapped IPv6. Socket options: TCP_NODELAY, IPV6_V6ONLY, SO_RCVTIMEO, SO_SNDTIMEO, IP_ADD/DROP_MEMBERSHIP.

**Files:** `kernel/net.c`, `kernel/net.h`

**Done when:** Userspace `socket(AF_INET6, SOCK_STREAM, 0)` returns fd; `connect` establishes IPv6 TCP connection. ✅

### T5.9 — DNS Resolver ✅

**What:** `dns_resolve()`: raw UDP, A/AAAA query, compression pointer parsing. Configurable resolver (default 8.8.8.8:53).

**Files:** `kernel/dns.c`, `kernel/dns.h`

**Done when:** `dns_resolve("google.com")` returns a valid IPv4 address. ✅

### T5.10 — DHCP Client ✅

**What:** DORA (DISCOVER/OFFER/REQUEST/ACK) via UDP port 67/68. Option parsing (subnet mask, router, DNS, lease). 2 retries with 1.5s timeout. Falls back to static 10.0.2.15.

**Files:** `kernel/dhcp.c`, `kernel/dhcp.h`

**Done when:** SLiRP boot obtains DHCP lease; `ifconfig` shows assigned IP. ✅

### T5.11 — SLAAC ✅

**What:** RS to ff02::2, wait for RA, parse Prefix Information Option (flag A=autonomous), form global unicast (prefix[64] + EUI-64 from MAC).

**Files:** `kernel/slaac.c`, `kernel/slaac.h`

**Done when:** After RA received, kernel has a global-scope IPv6 address. ✅

### T5.12 — NTP Client ✅

**What:** NTP v4 mode 3 request, transmit timestamp extraction at offset 40, NTP→Unix conversion (subtract 2208988800). `ntp_get_time()` = boot_time + uptime. `SYS_CLOCK_GETTIME` (syscall 49).

**Files:** `kernel/ntp.c`, `kernel/ntp.h`, `include/sys/time.h`

**Done when:** `clock_gettime(CLOCK_REALTIME)` returns post-2020 epoch value. ✅

### T5.13 — IGMPv2 + MLDv1 ✅

**What:** IGMPv2 membership reports (type 0x16), leave (0x17), query handler (0x11). 8-group table. MLDv1 report (ICMPv6 type 131), done (132), query handler (130). E1000 MTA programming for hardware multicast filtering.

**Files:** `kernel/igmp.c`, `kernel/igmp.h` (new); extended `icmpv6.c`, `e1000.c`

**Done when:** Joining 224.0.0.1 sends IGMP report; kernel receives multicast traffic. ✅

### T5.14 — Network Self-Tests ✅

**What:** 5 regression tests (no network backend required): tcp_find_conn_ipv6, ndp_cache_miss, icmpv6_ns_parse, socket_refcount, udp_queue_roundtrip.

**Files:** `kernel/net_test.c`, `kernel/net_test.h`

**Done when:** `make test-net` runs 5/5 tests with 120s timeout, all pass. ✅

### T5.15 — Two-QEMU Validation ✅

**What:** Automated `make test-net-2qemu` target. Two QEMU instances connected via socket backend. Validates TCP echo + UDP echo over IPv6.

**Files:** `os/test-2qemu.sh`

**Done when:** Both TCP and UDP echo tests PASS between two QEMU instances. ✅

### T5.16 — poll() Syscall ✅

**What:** `SYS_POLL` (syscall 52). TCP poll: checks LISTEN accept_count, ESTABLISHED recv_done, CLOSED state. UDP poll: checks q_count.

**Done when:** `poll()` returns POLLIN when TCP data is available. ✅

### T5.17 — errno Propagation ✅

**What:** `kernel_err_to_posix()` translation table maps kernel ERR_* to POSIX errno (ERR_AGAIN→EAGAIN=11, ERR_NOTCONN→ENOTCONN=107, etc.). Applied at syscall boundary.

**Files:** `kernel/syscall.c`

**Done when:** Socket syscalls return correct POSIX errno on failure. ✅

---

## Stage 6 — Security

### T6.1 — CSPRNG in Kernel

**What:** Already covered by T5.1. Ensure `/dev/random` in devfs reads from `csprng_get_bytes()`.

**Files:**
- `kernel/devfs.c` — wire `/dev/random` read to `csprng_get_bytes`

**Done when:** `cat /dev/random | hexdump -C` produces non-repeating output; no frozen loop.

**Depends on:** T5.1

---

### T6.2 — ASLR for EXEC Binaries

**What:** In `sys_exec`, when loading a non-PIE ELF (ET_EXEC):
- Generate random load offset using `csprng_get_bytes(4)` masked to range `[0x400000, 0x7FFF0000]`, 2 MiB aligned
- Add offset to every PT_LOAD virtual address before mapping
- Update ELF entry point by same offset
- Store offset in `process_t.aslr_offset` for debugging

**Files:**
- `kernel/elf.c` — modify `elf_load()`

**Done when:** Run the same static binary twice; addresses differ between runs.

**Depends on:** T5.1 (CSPRNG)

---

### T6.3 — User/Group Identity

**What:**
- Add `uid_t uid`, `gid_t gid` to `process_t`
- PID 1 starts with uid=0, gid=0
- `fork()` inherits uid/gid from parent
- Add `SYS_GETUID` (53), `SYS_GETGID` (54), `SYS_SETUID` (55), `SYS_SETGID` (56)
- `setuid(n)` only permitted if current uid == 0
- VFS permission check on `vfs_open()`: if `uid != 0`, enforce `mode & S_IRUSR` / `S_IWUSR` bits against file owner

**Files:**
- `kernel/process.h` / `kernel/process.c` — add uid/gid fields
- `kernel/syscall.c` — add 4 new syscalls
- `kernel/vfs.c` — add permission check in `vfs_open()`

**Done when:** As non-root process, `open("/etc/shadow", O_RDONLY)` returns `-EPERM`. As root, it succeeds.

**Depends on:** nothing

---

### T6.4 — Capability System

**What:** Implement exactly as specified in Backend Schema §16.1:
- `capability_t` struct
- `cap_issue(obj_type, obj_id, rights) → token`
- `cap_delegate(token, rights_subset) → derived_token`
- `cap_revoke(token)` — walk derivation tree; mark all derived tokens revoked
- `cap_validate(proc, obj_type, obj_id, rights) → bool`
- Per-process `cap_table_t`: array of 256 `capability_t`; spinlock-protected
- Gate `sys_open`, `sys_kill`, and future IPC syscalls behind `cap_validate`

**Files:**
- `kernel/capability.c` + `kernel/capability.h` (new)
- `kernel/process.h` — add `cap_table_t *caps`
- `kernel/syscall.c` — add `SYS_CAPGET` (57), `SYS_CAPSET` (58)

**Done when:** TC5 passes: issue cap A, delegate to B, delegate B to C. Revoke A. `cap_validate` on A, B, C all return false.

**Depends on:** T5.1 (CSPRNG for token generation), T6.3

---

### T6.5 — Syscall Filter (seccomp-style)

**What:**
- `syscall_policy_t`: 64-bit bitmask (covers syscalls 0–63)
- `SYS_SECCOMP (59)`: installs policy on calling process; cannot be uninstalled
- In `syscall_handler()`: if `current->seccomp != NULL && !(policy->allowed & (1 << syscall_num))` → SIGKILL + audit log

**Files:**
- `kernel/process.h` — add `syscall_policy_t *seccomp`
- `kernel/syscall.c` — check at top of `syscall_handler()`

**Done when:** Process that installs STRICT policy (only read/write/exit/sigreturn allowed) gets killed when it calls `sys_open`.

**Depends on:** T6.3

---

## Stage 7 — IPC and Services

### T7.1 — MAP_SHARED Shared Memory

**What:**
- Extend `sys_mmap`: detect `MAP_SHARED` flag
- Allocate/find named shared memory object by integer key (passed as `fd` parameter; key 0–255 supported)
- Map same physical pages into both processes' PML4s (no COW)
- Reference count physical pages; only freed when all holders munmap

**Files:**
- `kernel/vmm.c` — add shared page tracking
- `kernel/syscall.c` — extend `sys_mmap` handler

**Done when:** Two processes map same key; process A writes 0xDEAD to offset 0; process B reads 0xDEAD from offset 0 without any copy.

**Depends on:** nothing (VMM already exists)

---

### T7.2 — IPC Message Queue

**What:** Implement `ipc_queue_t` (see Backend Schema §16.2):
- `SYS_IPC_CREATE (60)`: create named queue; return handle fd
- `SYS_IPC_SEND (61)`: copy message into queue; block if full; rate-limit: max 1000 msgs/100ms per queue
- `SYS_IPC_RECV (62)`: copy message out of queue; block if empty

**Files:**
- `kernel/ipc.c` + `kernel/ipc.h` (new)
- `kernel/syscall.c` — add 3 syscalls

**Done when:** TC8: sender floods queue; after 100ms, rate limiter slows sender; receiver processes at least 10% of messages.

**Depends on:** nothing

---

### T7.3 — epoll

**What:**
- `SYS_EPOLL_CREATE (63)`: allocate `epoll_t`; return fd
- `SYS_EPOLL_CTL (64)`: add/remove/modify watched fds; events: `EPOLLIN | EPOLLOUT | EPOLLERR`
- `SYS_EPOLL_WAIT (65)`: block until at least one fd is ready; copy ready events to user buffer; timeout in ms

**Files:**
- `kernel/epoll.c` + `kernel/epoll.h` (new)
- `kernel/syscall.c`

**Done when:** A process watching two pipe fds with `epoll_wait` wakes up exactly when data is written to one of them.

**Depends on:** T7.2

---

### T7.4 — procfs

**What:**
- Mount a new virtual filesystem at `/proc`
- Read-only; all content generated on `vfs_read()` from live kernel data
- Files to implement:
  - `/proc/<pid>/status` — name, pid, ppid, state, uid, gid, vmrss (pages used)
  - `/proc/<pid>/maps` — one line per VMA: `start-end perm offset name`
  - `/proc/meminfo` — MemTotal, MemFree, SwapTotal, SwapFree
  - `/proc/uptime` — seconds.centiseconds since boot

**Files:**
- `kernel/procfs.c` + `kernel/procfs.h` (new)
- `kernel/vfs.c` — register procfs mount at boot

**Done when:** `cat /proc/1/status` prints PID 1's information. `cat /proc/meminfo` shows physical memory totals.

**Depends on:** T6.3 (uid/gid needed for status)

---

### T7.5 — Service Manager

**What:**
- Parse `/etc/services.d/*.service` files at boot (format defined in Backend Schema §16.3)
- Topological sort of deps[]; panic if cycle detected
- Start services in order: `fork()` + `exec()` per service; record pid
- Watchdog: every 5 s, check each running service's pid via `sys_kill(pid, 0)` (existence check); if gone and `restart_policy != NEVER`, restart
- Shell commands: `svcstart <name>`, `svcstop <name>`, `svcstatus`

**Files:**
- `kernel/svcmgr.c` + `kernel/svcmgr.h` (new)
- `kernel/shell.c` — 3 new commands

**Done when:** A service defined in `/etc/services.d/test.service` starts at boot; if killed manually, it restarts within 10 s.

**Depends on:** T7.4 (for `/proc` existence checks)

---

## Stage 8 — Advanced Memory (Partial)

### T8.1 — COW Fork

**What:** Replace full-copy fork with copy-on-write:
- On `sys_fork()`: walk parent PML4; for each user writable page, mark both parent and child PTE as read-only; set `PAGE_COW` bit in PTE
- In `page_fault_handler`: if fault is a write fault and `PAGE_COW` is set: allocate new physical page; copy data; remap as writable in faulting process; clear `PAGE_COW`

**Files:**
- `kernel/vmm.c` — add COW PTE marking in `vmm_fork()`
- `kernel/hal.c` — add COW case in `page_fault_handler`

**Done when:** Fork of a process with 1 MiB of heap data completes in < 1 ms (vs several ms for full copy). Subsequent writes in child do not affect parent's data.

**Depends on:** nothing (building on existing VMM)

---

### T8.2 — Swap Eviction (LRU Clock)

**What:** Implement active eviction so OOM is not the only mechanism to free pages:
- Clock hand: scan all user physical pages in a ring; if `ACCESSED` bit clear → candidate; if set → clear bit and advance
- On eviction: write page to swap slot; update PTE with swap marker; invalidate TLB
- Run eviction when `free_frames < total_frames * 0.1` (below 10% free)

**Files:**
- `kernel/vmm.c` / `kernel/swap.c` — add eviction pass triggered from PMM slow path

**Done when:** Allocate memory until PMM < 10% free; eviction kicks in; free_frames rises; system does not OOM. A subsequent access to an evicted address completes via swap-in.

**Depends on:** nothing (swap-in already exists)

---

## Stage 9 — Quality and Testing

### T9.1 — GDB Remote Stub

**What:**
- Implement RSP (GDB Remote Serial Protocol) over UART COM2 (0x2F8)
- Required packets: `?` (halt reason), `g`/`G` (read/write all regs), `m`/`M` (read/write memory), `c` (continue), `s` (single step via TF flag), `Z0`/`z0` (sw breakpoint via INT3)
- Stub activates when kernel panics OR when `Ctrl-C` received on GDB UART

**Files:**
- `kernel/gdb_stub.c` + `kernel/gdb_stub.h` (new)
- `kernel/panic.c` — call `gdb_stub_enter()` before halt

**Done when:** `gdb -ex "target remote :1234" opertur.elf` connects; `info registers` shows correct RIP; breakpoint on `sys_write` halts on next write syscall; `continue` resumes.

**Depends on:** nothing

---

### T9.2 — In-Kernel Test Harness

**What:**
- Macro `KTEST(name, fn)`: registers a test function at a special ELF section `.ktests`
- `run_ktests()`: iterates section; calls each fn; tracks pass/fail/panic-trapped
- Each test fn calls `KTEST_ASSERT(cond)` and `KTEST_ASSERT_EQ(a, b)`
- Tests run at boot before `init` if kernel command line contains `runtest`
- Exit code (0 = all pass) written to port 0xF4 (QEMU test device); allows scripted CI

**Files:**
- `kernel/ktest.c` + `kernel/ktest.h` (new)
- Initial test coverage:
  - PMM: alloc/free round-trip × 1000
  - Slab: alloc 128-byte objects × 512; free all; re-alloc
  - Page fault: map page, write, read back
  - Scheduler: two threads increment shared counter 10,000 times; verify count = 20,000

**Done when:** `qemu ... -append runtest` exits with code 0 (QEMU `echo $?`). All 4 initial tests pass.

**Depends on:** nothing

---

### T9.3 — Crash Dumps

**What:**
- On kernel panic: serialise registers + last 32 KB of kernel stack + last 256 WAL entries + process list to a fixed physical region (`0x100000`–`0x200000`, reserved at boot from PMM)
- On next boot: if magic cookie present in that region → dump to `/var/crash/crash-<timestamp>.bin`
- `crashdump` shell command: print dump summary (panic string, RIP, stack trace)

**Files:**
- `kernel/panic.c` — extend `kernel_panic()` to write dump region
- `kernel/boot.c` — check for dump region cookie before PMM init
- `kernel/shell.c` — add `crashdump` command

**Done when:** Induce a kernel panic; reboot; `crashdump` prints the panic string and RIP from the previous session.

**Depends on:** T9.1 (GDB stub confirms register state for cross-check)

---

## Stage 10 — GUI Foundation

### T10.1 — Framebuffer Initialisation

**What:**
- At boot, before switching from UEFI to the kernel, query GOP (`EFI_GRAPHICS_OUTPUT_PROTOCOL`) for framebuffer base address, resolution, and pixel format
- Map framebuffer physical region into kernel address space (no caching: PAT set to WC write-combining)
- Expose `/dev/fb0` in devfs: `mmap(MAP_SHARED)` maps the framebuffer into a userspace process
- Kernel boot screen: fill framebuffer with `#1A1A2E`; print kernel version string using bitmap font

**Files:**
- `boot/uefi_fb.c` (new) — GOP query, store `fb_base`, `fb_width`, `fb_height`, `fb_pitch`
- `kernel/devfs.c` — add `/dev/fb0`
- `kernel/framebuffer.c` + `kernel/framebuffer.h` — `fb_fill_rect`, `fb_put_glyph`, `fb_blit`

**Done when:** QEMU shows a dark blue screen with "OPERtur 1.0" printed in white at top-left immediately after boot.

**Depends on:** nothing (UEFI boot path independent of kernel subsystems)

---

### T10.2 — PS/2 Mouse Driver

**What:**
- Initialise PS/2 controller auxiliary port (port 0x60 / 0x64); enable mouse IRQ 12
- 3-byte packet decode: buttons (L/R/M), signed X delta, signed Y delta
- Maintain `mouse_state_t { int abs_x, abs_y; uint8_t buttons; }` in `kernel/mouse.c`
- Clamp abs_x to [0, fb_width-1] and abs_y to [0, fb_height-1]
- Expose `/dev/mouse` in devfs: `read()` returns one `mouse_event_t` struct per packet

**Files:**
- `kernel/mouse.c` + `kernel/mouse.h` (new)
- `kernel/devfs.c` — add `/dev/mouse`

**Done when:** `cat /dev/mouse | hexdump` shows changing values as mouse moves in QEMU. Cursor position (abs_x, abs_y) tracked correctly across screen bounds.

**Depends on:** T10.1

---

### T10.3 — Bitmap Font Rendering

**What:**
- Embed IBM CP437 8×16 bitmap font as a `const uint8_t font_8x16[256][16]` array in the kernel
- `fb_put_glyph(int x, int y, char c, uint32_t fg, uint32_t bg)`: blit a single 8×16 glyph
- `fb_put_string(int x, int y, const char *s, uint32_t fg, uint32_t bg)`: iterate chars, advance x by 8 per char; wrap at fb_width

**Files:**
- `kernel/font_8x16.c` (new — generated from existing CP437 bitmap data)
- `kernel/framebuffer.c` — add `fb_put_glyph`, `fb_put_string`

**Done when:** `fb_put_string(0, 0, "Hello, GUI!", 0xFFFFFF, 0x1A1A2E)` renders legible text on screen.

**Depends on:** T10.1

---

### T10.4 — Window Manager (Minimal)

**What:**
- `window_t` struct: `{int x, y, w, h; char title[64]; uint32_t *surface; pid_t owner}`
- `wm_create_window(pid, x, y, w, h, title)` → window handle (int)
- `wm_destroy_window(handle)`
- `wm_draw_frame(window_t*)`: draw title bar (24px, colour by focus state) + 1px border; blit client surface below
- `wm_event_loop()`: runs in a dedicated kworker thread; reads mouse + keyboard; dispatches to focused window; calls compositor
- Click on title bar: drag window; click outside focused window: refocus; title-bar close button: send SIGTERM to owner

**Files:**
- `kernel/wm.c` + `kernel/wm.h` (new)
- `kernel/shell.c` — add `wm_test` command that opens a test window

**Done when:** `wm_test` opens a 400×300 window titled "Test" on screen. Moving the mouse over the title bar and pressing the left mouse button drags the window. Clicking close sends SIGTERM.

**Depends on:** T10.1, T10.2, T10.3

---

### T10.5 — Compositor

**What:**
- Maintain z-ordered `window_t *stack[32]`
- `compositor_blit()`: for each window from bottom to top, blit `surface` into back buffer; then blit cursor bitmap at `(mouse_state.abs_x, mouse_state.abs_y)`
- Damage tracking: `dirty_rect_t`; only call `compositor_blit` on regions that changed
- Double-buffering: back buffer = `kmalloc(fb_width * fb_height * 4)`; `memcpy` to `fb_base` after each frame (vsync-free; target 60 fps via APIC timer callback every 16 ms)

**Files:**
- `kernel/compositor.c` + `kernel/compositor.h` (new)
- `kernel/wm.c` — call `compositor_invalidate_rect()` on window move/resize/content update

**Done when:** Two overlapping windows correctly layer (top window occludes bottom). Moving a window updates only the dirty region.

**Depends on:** T10.4

---

## Execution Order Summary

```
Stage 4: T4.1 → T4.2 → T4.3  ✅ Complete
                                  │
Stage 5: T5.2 → T5.3 → T5.4 ─→ T5.5  ✅
                   │          │
                   │          ├→ T5.6 ─→ T5.7 ─→ T5.8  ✅
                   │          │         │
                   │          │         └→ T5.14 ─→ T5.15  ✅
                   │          │
                   │          └→ T5.9 → T5.10 → T5.11 → T5.12 → T5.13  ✅
                   │
                   └→ T5.16 → T5.17  ✅
                                  │
Stage 6: T5.1 ─→ T6.1 / T6.2     │
           T6.3 ─→ T6.4 (also needs T5.1)
           T6.3 ─→ T6.5
           T6.3 ─→ T7.4
                                  │
Stage 7: T7.1 (independent)       │
         T7.2 → T7.3              │
         T7.4 → T7.5              ▼
                                  Next
Stage 8: T8.1 (independent)
         T8.2 (independent)

Stage 9: T9.1 (independent)
         T9.2 (independent)
         T9.1 → T9.3

Stage 10: T10.1 → T10.2
          T10.1 → T10.3
          T10.1, T10.2, T10.3 → T10.4 → T10.5
```

---

## Done Criteria Per Stage

| Stage | Done When |
|-------|-----------|
| S4 gap | ✅ tcsetattr raw mode works; SIGTTIN stops background reader; PTY pair creates functional master-slave |
| S5 | ✅ `ping 10.0.2.2` succeeds; TCP echo passes between QEMU instances; DHCP gets IP; DNS resolves; NTP syncs clock; 5/5 regression tests pass |
| S6 | Non-root blocked from root files; ASLR base randomises; cap revoke test passes; seccomp kills on policy violation |
| S7 | Shared memory visible across 2 processes; IPC rate limiter verified; procfs `/proc/1/status` readable; service auto-restarts |
| S8 | COW fork < 1 ms for 1 MiB heap; swap eviction keeps free_frames > 10% |
| S9 | GDB attaches over serial; ktest suite 100% pass; crash dump readable after reboot |
| S10 | Framebuffer shows colour; text renders; window drags; two overlapping windows composite correctly |

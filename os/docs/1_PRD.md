# OPERtur / TRY1 OS — Product Requirements Document (PRD)

**Version:** 2.0  
**Project:** OPERtur / TRY1  
**Target Platform:** x86-64 (bare metal + QEMU)  
**Status Baseline:** Stages 1–5 complete; Stages 6–10 in progress

---

## 1. Purpose

OPERtur is a custom x86-64 operating system built from scratch. The goal is to produce a complete, self-hosting OS with a layered architecture, real process isolation, a persistent filesystem, a network stack, a security model, and a GUI foundation — implemented sequentially through 10 defined stages.

This PRD defines **what** the system must do, for whom, and to what standard. It does not prescribe implementation details — those are covered in the TRD.

---

## 2. Stakeholders

| Role | Description |
|------|-------------|
| Primary Developer | Sole implementer; designs and writes all kernel and userspace code |
| Test Environment | QEMU/KVM + x86-64 bare metal targets |
| Future Users | Developers who may run userspace programs on OPERtur |

---

## 3. Product Scope

OPERtur is a monolithic-kernel OS with the following top-level capability areas:

1. **Hardware abstraction** — manage CPU, RAM, timers, interrupts, serial I/O, and block storage
2. **Memory management** — physical pages, virtual address spaces, heap, swap, OOM
3. **Process and thread lifecycle** — fork/exec/exit/wait, scheduler, signals, synchronisation
4. **Syscall interface** — stable ABI between userspace and kernel
5. **Persistent storage** — SFS filesystem, VFS layer, journaling, multiple mount points
6. **Shell and terminal** — interactive CLI, pipes, redirection, scripting, PTY
7. **Networking** — full TCP/IP stack, E1000 NIC driver, sockets API
8. **Security** — capability tokens, user/group model, syscall filtering, ASLR
9. **IPC and services** — shared memory, message queues, service manager, procfs/sysfs
10. **GUI foundation** — framebuffer, window manager, compositor, font rendering

---

## 4. Implemented Capabilities (Baseline)

The following areas are **complete or substantially complete** as of the current build:

### 4.1 Boot and Hardware (Stage 1 — 100%)
- Boots to interactive shell on bare metal and QEMU
- Higher-half kernel mapping, GDT/IDT/TSS, all 256 interrupt vectors registered
- PIC/APIC interrupt routing (APIC MMIO skipped on QEMU TCG — falls back to legacy PIC; APIC timer active on KVM/bare metal)
- UART serial console available from earliest boot
- PIT+HPET timer (HPET MMIO skipped on QEMU TCG — falls back to PIT for scheduling tick)
- Panic path produces register dump, stack trace, and halts
- Software watchdog with per-CPU heartbeat; QEMU TCG detection via CPUID

### 4.2 Memory Management (Stage 1 — 100%)
- Bitmap PMM; 4-level page tables per process; slab + buddy heap
- Demand paging, swap-in on page fault, double-free detection
- RAM-backed swap with transparent swap-in on page fault
- OOM killer with prior heap compaction; Page-table self-reference check; PML4 PID stamp
- ELF loader handles overlapping segments correctly

### 4.3 Scheduler and Threads (Stage 1 — 100%)
- Preemptive multilevel feedback queue scheduler
- Priority inheritance, priority aging / starvation avoidance
- fork, exec, exit, wait; per-process PML4; per-thread kernel stack
- Signal delivery with sigframe_t / SYS_SIGRETURN trampoline
- Work queues, deferred tasks, kernel worker threads
- Job control (SIGTSTP/SIGCONT), process groups, fg/bg

### 4.4 Syscall Interface (Stage 2 — 100%)
- 53 syscalls via int 0x80 (numbers 0–52); all pointer arguments range-checked
- SYS_MMAP / SYS_MUNMAP / SYS_MPROTECT for dynamic linker
- SYS_IOCTL / SYS_SETPGID / SYS_GETPGID / SYS_PTY_PAIR for terminal/job-control
- SYS_SOCKET–SYS_GETPEERNAME (9 socket syscalls) + SYS_POLL for networking
- SYS_CLOCK_GETTIME for NTP-based wall clock
- kernel_err_to_posix() translation at syscall boundary for correct POSIX errno
- ELF64 loader with PT_LOAD, PT_INTERP, PIE + RDTSC-based ASLR
- ld.so dynamic linker: RELA/PLT relocations, shared library mmap, init/fini arrays

### 4.5 Storage and Filesystem (Stage 3 — 100%)
- PCI bus enumeration; ATA PIO (IRQ-driven) + AHCI DMA + NVMe drivers; LRU write-back sector cache
- SFS on-disk filesystem: superblock, bitmaps, inodes, direct + 2-level indirect blocks (~8 MB max)
- VFS with up to 8 mount points; tmpfs at /tmp; devfs at /dev (/dev/null, zero, random, full, ttyS0)
- Journaling/WAL: 63-slot circular buffer, crash recovery, fsck
- Hard links, symlinks, sparse files, O_APPEND, atomic rename
- File permissions (mode bits), advisory locking, timestamps, chmod
- Backup/restore with recursive archive format

### 4.6 Shell and Terminal (Stage 4 — 100%)
- 50+ built-in commands; pipes (8-stage); I/O redirection; env vars; scripting (8 nesting levels)
- Line editing (readline-style), 64-entry history, tab completion (commands + VFS paths)
- TTY line discipline: canonical/raw mode, echo, signal generation, Ctrl-C/D/Z/U/K/W/backslash
- Termios ioctl (TCGETATTR/TCSETATTR/TIOCGPGRP/TIOCSPGRP) via SYS_IOCTL
- PTY pseudo-terminal subsystem (8-slot pool, master-slave VFS ops, line discipline, signal chars)
- SIGTTIN/SIGTTOU job control for background processes
- Job control: bg/fg/jobs; process group tracking
- Aliases, tab completion, scripting source command

### 4.7 Networking (Stage 5 — 100%)
- NIC driver: E1000 (Intel 82540EM/82545EM) — PCI detection, BAR0 MMIO, descriptor rings, send/poll, IRQ 11, MTA programming
- Ethernet layer: frame encode/decode, EtherType dispatch (ARP/IPv4/IPv6)
- ARP: request/response, 8-entry cache, poll-and-wait resolution
- IPv4: packet TX/RX, header checksum, mutable IP address, broadcast handling, `ipv4_set/get_addr`
- IPv6: packet TX/RX, link-local EUI-64 address from MAC, multicast group table (join/leave/is_member), MLDv1 support
- ICMPv4: echo request/reply (ping)
- ICMPv6: echo, Neighbor Solicitation/Advertisement, Router Solicitation/Advertisement, MLDv1 reports/queries
- NDP: Neighbor Discovery cache, NS transmission with poll-wait, cache lookup/update
- Routing table: IPv4/IPv6 (add, lookup, clear)
- UDP: checksum validation, endpoint-based receive queue (16 datagrams), blocking dequeue
- TCP: full state machine (CLOSED/LISTEN/SYN_SENT/SYN_RECV/ESTABLISHED/FIN_WAIT1-2/TIME_WAIT/CLOSE_WAIT/LAST_ACK), IPv4+IPv6 5-tuple matching, SYN retransmit (5s), data retransmission (RTO), FIN retransmit (1s→2s backoff→60s cap), TIME_WAIT 2MSL timer (60s), `tcp_tick()`, lock-safe callback dispatch
- DNS resolver: kernel-level, raw UDP, A/AAAA queries with compression pointer parsing
- DHCP client: DORA (DISCOVER/OFFER/REQUEST/ACK), option parsing, 2 retries with 1.5s timeout
- SLAAC: RS/RA exchange, prefix parsing, global unicast address (prefix[64] + EUI-64)
- NTP client: NTP v4 mode 3, transmit timestamp extraction, NTP→Unix conversion, `clock_gettime()` syscall
- Sockets API: refcounted socket_t, sock_ops_t dispatch table, dual-stack (AF_INET/AF_INET6), 32-entry fd table with socket detection
- TCP sockets: bind/connect/listen/accept/send/recv/close/poll/getsockname/getpeername
- UDP sockets: bind/sendto/recvfrom/close/poll
- Socket options: TCP_NODELAY, IPV6_V6ONLY, SO_RCVTIMEO, SO_SNDTIMEO, IP_ADD/DROP_MEMBERSHIP
- IPv4-mapped IPv6 (::ffff:x.x.x.x) matching in tcp_find_conn()
- IGMPv2: IPv4 multicast group management (8-group table), Membership Reports/Leave Group
- MLDv1: IPv6 multicast listener discovery (reports, done, query handler)
- E1000 MTA programming via CRC-32 for hardware multicast filtering
- **5/5 regression tests pass** (tcp_find_conn_ipv6, ndp_cache_miss, icmpv6_ns_parse, socket_refcount, udp_queue_roundtrip)
- **Two-QEMU IPv6 TCP+UDP echo validated** via automated test script
- poll() syscall with TCP/UDP support
- kernel_err_to_posix() errno translation (ERR_AGAIN→EAGAIN, ERR_NOTCONN→ENOTCONN, etc.)

### 4.8 Userspace C Library
- Full libc: stdio/printf, stdlib/malloc/free/calloc/realloc, string, unistd syscall wrappers (read/write/open/close/fork/execve/wait/pipe/dup2/ioctl/chdir/getcwd/sleep/kill/lseek/sbrk), signal, errno, crt0
- All socket wrappers (socket/bind/connect/listen/accept/send/recv/sendto/recvfrom/setsockopt/getsockopt/getsockname/getpeername/poll)

---

## 5. Outstanding Requirements

The following capabilities are **not yet implemented** and are required for completion:

### 5.1 Security (Stage 6 — ~15%)
- **Capability system**: token issue/revoke/validate, capability-gated syscalls, per-process capability set
- **User/group model**: uid/gid fields in process descriptor, /etc/passwd equivalent, login
- **Syscall filtering**: seccomp-style policy hook per process
- **Privilege separation**: services run as non-root identities
- **Secure IPC**: capability-token-gated IPC channels
- **Kernel crypto**: hash primitives (SHA-256 minimum), CSPRNG in kernel
- **ASLR hardening**: EXEC binaries currently load at fixed address — must randomise
- **Secure boot chain** and signed binaries (stretch goal)
Already present: SMAP/SMEP, NX, kernel .text read-only after boot, PIE ASLR via RDTSC

### 5.2 IPC and Services (Stage 7 — 0%)
- Shared memory (MAP_SHARED mmap flag)
- Message queues (kernel-managed FIFO with sender/receiver blocking)
- Event multiplexing (epoll or kqueue equivalent)
- Kernel event bus exposed to userspace (process-start/exit, FS change, device hotplug)
- Service manager: dependency-ordered startup, liveness tracking, restart policies
- procfs at /proc, sysfs at /sys

### 5.3 Advanced Memory (Stage 8 — ~10%)
- COW fork (currently full copy on fork)
- MAP_SHARED shared memory (needed for Stage 7 IPC)
- Huge page support (2 MiB pages for large allocations)
- Swap eviction policy (LRU clock algorithm)

### 5.4 Quality and Testing (Stage 9 — ~20%)
- GDB remote stub over serial
- In-kernel test harness with pass/fail reporting
- Syscall fuzzer (1 M sequences without panic)
- Crash dump collection (instead of halt-on-panic)
- Kernel module loading/unloading

### 5.5 GUI Foundation (Stage 10 — ~5%)
- Linear framebuffer initialisation (UEFI GOP or VESA)
- CPU-side 2D drawing primitives (blit, fill, line, clip)
- PS/2 mouse driver; USB HID keyboard/mouse
- Window manager: placement, focus, decoration
- Compositor: layered surface blending
- Font rendering (bitmap font minimum; TrueType stretch goal)
- GUI event loop and shared rendering buffer

---

## 6. Non-Functional Requirements

| Requirement | Target |
|-------------|--------|
| Boot time | < 2 seconds to interactive shell on QEMU |
| Kernel panic | Must print register dump + stack trace; never silently hang |
| Memory safety | No kernel memory accessible from user ring-3 code |
| Filesystem integrity | WAL guarantees no committed data lost on crash |
| Scheduler fairness | No thread starves for more than 4 seconds (aging mechanism) |
| Syscall correctness | Invalid pointer args return EFAULT, never panic |
| ABI stability | Syscall numbers 0–52 are frozen; new syscalls append only |

---

## 7. Out of Scope

- SMP / multi-core scheduling (single core assumed throughout; NUMA not targeted)
- Hibernation (S4) — S3 suspend-to-RAM is in the architecture spec but low priority
- Full POSIX compliance
- Porting existing applications without source

---

## 8. Delivery Stages Summary

| Stage | Name | Status |
|-------|------|--------|
| 1 | Core Kernel Foundation | ✅ Complete |
| 2 | Userspace Foundation | ✅ Complete |
| 3 | Storage and Filesystem | ✅ Complete |
| 4 | Shell and Terminal | ✅ Complete |
| 5 | Networking | ✅ Complete |
| 6 | Security | 🟡 ~15% |
| 7 | IPC and Services | ❌ 0% |
| 8 | Advanced Memory | 🟡 ~10% |
| 9 | Quality and Testing | 🟡 ~20% |
| 10 | GUI Foundation | 🟡 ~5% |

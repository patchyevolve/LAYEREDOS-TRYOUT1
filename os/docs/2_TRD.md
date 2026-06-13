# OPERtur / TRY1 OS — Technical Requirements Document (TRD)

**Version:** 2.0  
**Architecture Reference:** ARCHITECTURE_REQUIREMENTS.md  
**Toolchain:** GCC cross-compiler (x86-64-elf), GAS, ld, NASM  
**Build target:** Freestanding ELF64 kernel + userspace binaries

---

## 1. Architecture Constraints

All implementation must respect the strict 6-layer model:

```
Layer N   : User Programs (ring 3)
Layer N-1 : Syscall Gateway + Audit
Layer 5   : I/O, Filesystem, Device Management
Layer 4   : Virtual Memory, Paging, Memory Protection
Layer 3   : Process Lifecycle, Threads, Synchronisation
Layer 2   : CPU Scheduling, IPC, Interrupt Handling
Layer 1   : Hardware Abstraction Layer (HAL) + Driver Model
Layer 0   : Hardware (x86-64)
```

**Rules:**
- Layer N calls only Layer N-1. No layer skipping.
- No upward calls. Cross-cutting communication via event bus pub/sub only.
- Every cross-layer operation requires a capability token (once cap system is implemented).
- Circular dependency detector panics on violation.

**Operating modes:** NORMAL (all layers) → SAFE (layers 0–3 only) → EMERGENCY (layers 0–2 only)

---

## 2. Layer 0 — Hardware Assumptions

| Component | Specification |
|-----------|--------------|
| CPU | x86-64, 1 core, rings 0/3, paging, MSRs, FXSAVE |
| RAM | 512 MB minimum, physically contiguous |
| UART | 16550-compatible at COM1 (0x3F8) |
| Block device | ATA/IDE, AHCI, or NVMe; 512-byte sectors; LBA48 |
| Timer | HPET at 0xFED00000 (100 MHz), PIT as fallback (HPET MMIO skipped on QEMU TCG) |
| PIC | 8259A remapped to 0x20–0x2F; xAPIC for scheduling (APIC MMIO skipped on QEMU TCG) |
| NIC | E1000 (Intel 82540EM/82545EM/82573L/82574L) at PCI 0x8086:0x100E |

---

## 3. Layer 1 — HAL and Drivers (Implemented)

### 3.1 CPU HAL
- Long mode entry; GDT with kernel CS/DS (rings 0 and 3), TSS per CPU
- FXSAVE/FXRSTOR on every context switch
- MSR access wrappers (rdmsr / wrmsr)
- CPUID feature detection at boot; gates SSE, NX, FXSAVE, SMAP, SMEP, MWAIT code paths
- QEMU TCG detection via CPUID leaf 0x40000000 (checks for `GenuineTian` vs `KVMKVMKVM`)

### 3.2 Interrupt HAL
- IDT: 256 entries; stubs push error code + vector, call common C dispatcher
- xAPIC: detected via CPUID; MMIO mapping **skipped on QEMU TCG** (softmmu page-walk cache bug). Falls back to legacy PIC.
- PIC remapped to 0x20–0x2F; always active on QEMU TCG
- IRQ 11 wired to E1000 NIC driver; IRQ 14/15 wired to ATA block driver wait queues
- SMAP/SMEP enabled via CR4

### 3.3 Timer HAL
- HPET: 100 MHz counter at 0xFED00000. MMIO mapping **skipped on QEMU TCG** — falls back to PIT.
- PIT: provides scheduling tick on QEMU TCG (IRQ0, ~1ms granularity)
- APIC timer: calibrated against HPET; drives preemption on KVM/bare metal
- `hal_udelay()`: busy-wait microsecond delay using HPET/PIT

### 3.4 UART HAL
- 16550: 115200 baud, 8N1; polling TX, IRQ-driven RX
- ISR calls `tty_input_push()` for signal delivery and line discipline
- Early-boot console before any scheduler exists

### 3.5 Block HAL
- ATA PIO: identify, LBA48 read/write; IRQ-driven with `sched_block`/`sched_wake`
- AHCI DMA: PCI class 0x01/0x06, ABAR mapping, HBA reset, port probe, command list, PRP DMA
- NVMe: PCI class 0x01/0x08, BAR0 MMIO, admin submission/completion queues, I/O queue pair, PRP DMA
- Block device abstraction: `block_read(dev_id, sector, buf)` / `block_write(...)`
- All three drivers registered as block devices

### 3.6 NIC HAL (E1000)
- PCI detection: Intel 82540EM/82545EM/82573L/82574L (vendor 0x8086, device 0x100E)
- BAR0 MMIO mapping; software reset; MAC address read
- 32-entry TX/RX descriptor rings with DMA bounce buffers
- TX: descriptor ring submit; wait for completion
- RX: IRQ 11 handler calls `eth_rx_poll()`; RDT management keeps N-2 descriptors available
- MTA (Multicast Table Array): CRC-32 based bit programming for hardware multicast filtering
- Statistics registers: TPT (TX packets), GPRC (RX packets), etc.
- NIC poll thread runs `eth_rx_poll()` + `tcp_tick()` every 10ms

---

## 4. Layer 2 — Scheduler, IPC, Interrupts (Implemented)

### 4.1 Scheduler
- Multilevel feedback queue; timer-driven preemption via PIT/APIC timer ISR
- O(1) priority pick; priority inheritance on mutex contention
- Priority aging: threads not run for > 50 ms receive a boost; prevents starvation
- Idle thread: selects ACPI C-state (C1/C2/C3) based on next wakeup deadline
- Work queue (`system_wq`): spinlock-protected list; kworker thread at THREAD_DEF_PRIO
- Deferred tasks: one-shot timer API polling on system_wq
- `sched_remove_thread` guard: prevents run queue corruption when blocked thread already dequeued

### 4.2 Context Switch
- `ctx.S`: saves all general-purpose registers + RFLAGS; switches RSP; restores
- CR3 switched per process boundary
- FPU state saved/restored via FXSAVE/FXRSTOR on every context switch
- `fork_child_entry` and `signal_trampoline` assembly stubs

### 4.3 IPC (Not Implemented — Stage 7)
Required additions:
- `ipc_queue_t`: fixed-capacity ring buffer, sender blocks on full, receiver blocks on empty
- `shm_region_t`: MAP_SHARED mmap flag; COW disabled; same physical pages mapped into two address spaces
- `epoll_fd`: event multiplexer; internally a wait queue over a set of fds
- Rate limiter: max messages per 100 ms per queue

### 4.4 Timekeeping
- Monotonic clock: PIT tick counter (HPET when available); never goes backward
- Wall clock: NTP-synchronised (via `ntp_get_time()` = boot_time + uptime_seconds)
- `SYS_CLOCK_GETTIME` (syscall 49): supports CLOCK_REALTIME and CLOCK_MONOTONIC

---

## 5. Layer 3 — Process, Threads, Sync (Implemented)

### 5.1 Process Descriptor (`process_t`)
```c
typedef struct process {
    pid_t           pid;
    pid_t           ppid;
    pid_t           pgid;           // Process group (for TTY signals)
    uint64_t        *pml4;          // CR3 value (PML4 PID stamp stored at pml4[255])
    fd_table_t      *fd_table;      // 32-slot FD table
    signal_state_t  signals;        // sigaction table, pending mask
    uint8_t         *kernel_stack;  // 16 KiB kernel stack per thread
    thread_t        *threads;       // List of threads in this process
    int             exit_code;
    enum { RUNNING, ZOMBIE, STOPPED } state;
    // Planned Stage 6 additions:
    // uid_t            uid;
    // gid_t            gid;
    // cap_table_t      *caps;
    // syscall_policy_t *seccomp;
} process_t;
```

### 5.2 Thread Control Block (`thread_t`)
```c
typedef struct thread {
    cpu_context_t   ctx;            // Saved GP registers + FXSAVE region
    uint64_t        kernel_rsp;     // Kernel stack pointer
    uint64_t        kernel_stack;   // Kernel stack base (16 KB, 4 pages)
    uint64_t        kernel_stack_size;
    int             priority;       // 0 (high) – 255 (low)
    int             effective_prio; // After inheritance / aging
    uint64_t        sleep_until_ns;
    wait_queue_t    *blocked_on;
    process_t       *proc;
    char            name[32];
    uint64_t        *pml4;          // Per-thread PML4 (0 for kernel threads)
    enum { READY, RUNNING, SLEEPING, BLOCKED, ZOMBIE } state;
} thread_t;
```

### 5.3 Synchronisation Primitives (Implemented)
- `mutex_t`: spinlock + wait queue; priority ceiling; try/timed variants
- `condvar_t`: wait/signal/broadcast on mutex
- `rwlock_t`: not yet implemented — needed for Stage 6 VFS locking

### 5.4 Signal Delivery
- Default actions: SIGTERM (exit), SIGKILL (force exit), SIGSTOP (suspend), SIGCONT (resume), SIGTSTP (stop from TTY), SIGTTIN/SIGTTOU (job control)
- `sigframe_t` pushed onto user stack; signal trampoline at `SIGNAL_TRAMPOLINE_ADDR`
- SYS_SIGRETURN pops sigframe and restores saved context
- ISR-level signal detection: Ctrl-C/SIGINT, Ctrl-Z/SIGTSTP, Ctrl-\/SIGQUIT delivered via work queue

### 5.5 Capability Manager (Not Implemented — Stage 6)
Required additions: (unchanged from TRD v1.0 §5.5)

---

## 6. Layer 4 — Virtual Memory (Implemented)

### 6.1 Physical Memory Manager
- Bitmap PMM: one bit per 4 KiB frame
- `pmm_alloc_page()`: O(1) via free-list hint; `pmm_alloc_pages(n)`: O(N)
- Double-free detection: poison pattern `0xDEADBEEFDEADBEEF` in freed frame first 8 bytes
- Corruption validation: free-list pointer alignment check, page index range check, bitmap test for double-alloc
- OOM kill: calls `kmalloc_compact()` before scheduling kill worker

### 6.2 Virtual Memory Manager
- 4-level page tables (PML4 → PDPT → PD → PT)
- `vmm_map(pml4, vaddr, paddr, flags)`: PAGE_USER / PAGE_WRITE / PAGE_NX
- TLB shootdown: INVLPG per page on unmap; full CR3 reload on PML4 switch
- SMAP: `stac`/`clac` around copy_from_user/copy_to_user
- SMEP: kernel never executes user pages
- Self-reference check in `vmm_free_user_pages()`: skips PML4 entries pointing to PML4 itself
- PML4 PID stamp: pml4[255] = proc->pid; validated in process_exit before freeing
- `vmm_dump_pml4()` / `vmm_validate_pagetables()`: debug functions (guarded by VMM_DEBUG)

### 6.3 Heap Allocator
- Slab caches: 16 / 32 / 64 / 128 / 256 / 512 / 1024 byte object sizes
- Large alloc fallback: buddy allocator (powers of two, up to 2 MiB)
- `kmalloc(size)` / `kfree(ptr)` / `krealloc(ptr, size)`
- `calloc(n, size)`: overflow-checked multiply before allocation
- `kmalloc_compact()`: scans slab pages and frees empty pages back to PMM; called before OOM kill

### 6.4 Swap
- RAM-backed swap store; slot bitmap; PTE encodes swap slot in non-present PTE
- Swap-out: eviction policy not yet implemented (manual only)
- Swap-in: page fault handler detects non-present + swap marker; reads slot; maps page

### 6.5 Missing — Stage 8 Targets
- COW page fault handler for fork (currently full copy on fork)
- MAP_SHARED shared memory (needed for Stage 7 IPC)
- Huge page support (2 MiB pages for large allocations)
- Eviction policy for swap (LRU clock algorithm)

---

## 7. Layer 5 — Filesystem and Devices (Implemented)

### 7.1 VFS
```c
typedef struct vnode {
    vfs_t       *fs;            // Owning filesystem
    uint64_t    inode_id;
    uint32_t    mode;           // Permission bits
    uint32_t    uid, gid;       // Not enforced until Stage 6
    uint64_t    size;
    uint64_t    atime, mtime, ctime;
    uint32_t    nlink;
    int         lock_flag;      // Advisory lock
} vnode_t;
```
- Mount table: up to 8 entries; longest-prefix matching on path resolution
- Symlink following: max 8 hops to prevent loops
- `vfs_open`, `vfs_read`, `vfs_write`, `vfs_lseek`, `vfs_ftruncate`, `vfs_create`, `vfs_unlink`, `vfs_mkdir`, `vfs_rename`, `vfs_stat`, `vfs_readdir`, `vfs_chmod`, `vfs_ioctl`
- FD table: 32 slots, refcounted file descriptions; dup2 support
- Socket FDs dispatch to sock_ops via sock_lookup

### 7.2 SFS On-Disk Layout
```
[Superblock 1 sector][Inode bitmap N sectors][Block bitmap N sectors]
[Inode table M sectors][Data blocks ...]
```
- Inode: 12 direct blocks + 1 singly-indirect + 1 doubly-indirect ≈ 8 MB max file
- Journaling: 63-slot WAL ring buffer; DATA entry then COMMIT entry; recovery replays committed transactions; checkpoint zeroes journal slot after successful write

### 7.3 Other Filesystems
- tmpfs: RAM-backed; all VFS ops; dynamic block alloc; lost on reboot
- devfs: /dev/null, /dev/zero, /dev/random, /dev/full, /dev/ttyS0

### 7.4 Sector Cache
- LRU 64-entry write-back cache; dirty bit; eviction flushes to block device

### 7.5 Missing
- procfs at /proc (process list, per-process status, memory maps)
- sysfs at /sys (device tree, driver parameters)

---

## 8. Syscall Gateway (Layer N-1)

### 8.1 Current Syscall Table (53 syscalls, int 0x80)

| Number | Name | Status |
|--------|------|--------|
| 0 | SYS_READ | ✅ |
| 1 | SYS_WRITE | ✅ |
| 2 | SYS_OPEN | ✅ |
| 3 | SYS_CLOSE | ✅ |
| 4 | SYS_FORK | ✅ |
| 5 | SYS_EXEC | ✅ |
| 6 | SYS_EXIT | ✅ |
| 7 | SYS_WAIT | ✅ |
| 8 | SYS_GETPID | ✅ |
| 9 | SYS_KILL | ✅ |
| 10 | SYS_SIGACTION | ✅ |
| 11 | SYS_SIGRETURN | ✅ |
| 12 | SYS_SLEEP | ✅ |
| 13 | SYS_MMAP | ✅ |
| 14 | SYS_MUNMAP | ✅ |
| 15 | SYS_MPROTECT | ✅ |
| 16 | SYS_LSEEK | ✅ |
| 17 | SYS_STAT | ✅ |
| 18 | SYS_MKDIR | ✅ |
| 19 | SYS_UNLINK | ✅ |
| 20 | SYS_RENAME | ✅ |
| 21 | SYS_DUP2 | ✅ |
| 22 | SYS_CHDIR | ✅ |
| 23 | SYS_GETCWD | ✅ |
| 24 | SYS_FTRUNCATE | ✅ |
| 25 | SYS_READDIR | ✅ |
| 26 | SYS_CHMOD | ✅ |
| 27 | SYS_CLONE | ✅ |
| 28 | SYS_SBRK | ✅ |
| 29 | SYS_PIPE | ✅ |
| 30 | SYS_UMASK | ✅ |
| 31 | SYS_IOCTL | ✅ |
| 32 | SYS_GETPGID | ✅ |
| 33 | SYS_SETPGID | ✅ |
| 34 | SYS_PTY_PAIR | ✅ |
| 35 | SYS_SOCKET | ✅ |
| 36 | SYS_BIND | ✅ |
| 37 | SYS_CONNECT | ✅ |
| 38 | SYS_LISTEN | ✅ |
| 39 | SYS_ACCEPT | ✅ |
| 40 | SYS_SEND | ✅ |
| 41 | SYS_RECV | ✅ |
| 42 | SYS_SENDTO | ✅ |
| 43 | SYS_RECVFROM | ✅ |
| 44 | SYS_SETSOCKOPT | ✅ |
| 45 | SYS_GETSOCKOPT | ✅ |
| 46 | SYS_CLOCK_GETTIME | ✅ |
| 47 | SYS_GETSOCKNAME | ✅ |
| 48 | SYS_GETPEERNAME | ✅ |
| 49 | SYS_SIGSUSPEND | ✅ |
| 50 | SYS_SIGPROCMASK | ✅ |
| 51 | SYS_POLL | ✅ |
| 52 | reserved | 🟡 |

**Total: 53 syscall slots (0–52), 53 implemented. Numbers 0–52 frozen.**

### 8.2 Required Additions (Stage 6–10)
- SYS_SHMOPEN, SYS_MMAP extension for MAP_SHARED (IPC)
- SYS_EPOLL_CREATE, SYS_EPOLL_CTL, SYS_EPOLL_WAIT (event multiplexing)
- SYS_CAPGET, SYS_CAPSET (capability system)
- SYS_SETUID, SYS_SETGID, SYS_GETUID, SYS_GETGID (user model)
- SYS_SECCOMP (syscall filtering)

### 8.3 Validation Rules (All Syscalls)
1. Every pointer arg: `(addr >= USER_START && addr + len <= USER_END && is_mapped(addr))`
2. Strings: null-terminator search bounded to 4096 bytes
3. FD args: range check 0–31 then null check in fd_table; socket FD check via sock_lookup
4. Return value: kernel_err_to_posix() translates kernel ERR_* codes to POSIX errno (negative). Never a raw pointer.

---

## 9. Networking Stack (Stage 5 — Implemented)

### 9.1 NIC Driver (E1000)
- Intel 82540EM/82545EM/82573L/82574L on PCI
- 32-entry TX ring (cache-aligned descriptors) + 32-entry RX ring (DMA bounce buffers)
- IRQ 11 handler calls `eth_rx_poll()`; RDT management keeps descriptors available
- MTA multicast filtering via CRC-32
- Statistics: TPT (TX packets), GPRC (RX packets), etc.

### 9.2 Protocol Stack Architecture
```
Userspace socket API (sys_socket/bind/connect/send/recv/sendto/recvfrom/poll/getsockname/getpeername)
    ↓
Socket layer (sock_ops_t dispatch: tcp_sock_*, udp_sock_*)
    ↓
TCP (full state machine, ACK tracking, retransmit, TIME_WAIT 2MSL) / UDP (endpoint queue)
    ↓
IPv4 (routing, broadcast) / IPv6 (link-local, multicast groups, MLDv1)
    ↓
ICMPv4 / ICMPv6 (echo, NDP NS/NA/RS/RA, MLDv1) / ARP / IGMPv2
    ↓
Ethernet frame layer (EtherType dispatch: 0x0800=IPv4, 0x0806=ARP, 0x86DD=IPv6)
    ↓
E1000 NIC driver (DMA rings, IRQ 11, MTA)
```

### 9.3 Buffer Model
- No skbuff_t. All buffers are stack-allocated (1518-byte `buf[]` in `eth_rx_poll()`)
- UDP: `udp_dgram_t` with 1500-byte payload in 16-entry ring per endpoint
- TCP: single `recv_buf[TCP_MSS]` per connection; payload copied in `tcp_input`
- Kernel stack rule: all buffers must be well under 16 KB (no large structs on stack)

### 9.4 Socket Layer
```c
typedef struct socket {
    int domain;             // AF_INET or AF_INET6
    int type;               // SOCK_STREAM or SOCK_DGRAM
    int protocol;
    sock_ops_t *ops;        // tcp_ops or udp_ops
    void *proto;            // tcp_conn_t* or udp_endpoint_t*
    int refcount;
    int fd;                 // FD slot index
    bool ipv6only;          // IPV6_V6ONLY flag
    uint32_t recv_timeout;  // SO_RCVTIMEO (ms)
    uint32_t send_timeout;  // SO_SNDTIMEO (ms)
} socket_t;
```

### 9.5 TCP Connection
```c
typedef struct tcp_conn {
    uint8_t used;
    uint8_t closed;
    uint8_t syn_recv_deferred;
    uint8_t af;                     // AF_INET or AF_INET6
    uint8_t state;                  // TCP_* state enum
    // 5-tuple
    uint32_t src_ip, dst_ip;
    uint16_t src_port, dst_port;
    uint8_t src_ip6[16], dst_ip6[16];
    // Sequence numbers
    uint32_t snd_nxt, snd_una, iss, rcv_nxt, irs;
    uint16_t snd_wnd, rcv_wnd;
    // Retransmit
    uint32_t rto_remaining;         // Data RTO timer (ms)
    uint32_t retry_count;
    uint32_t fin_rto_remaining;     // FIN retransmit timer
    uint32_t fin_retry_count;
    uint32_t syn_retry_count;       // SYN retransmit count
    uint8_t *retransmit_buf;
    uint16_t retransmit_len;
    uint32_t retransmit_seq;
    uint32_t timewait_ms;           // TIME_WAIT 2MSL counter
    // Data
    uint8_t recv_buf[TCP_MSS];
    uint16_t recv_len;
    bool recv_done;
    // Callbacks
    void (*on_connect)(struct tcp_conn *c);
    void (*on_recv)(struct tcp_conn *c, const uint8_t *data, uint16_t len);
    void (*on_close)(struct tcp_conn *c);
    // Options
    bool nodelay;
    // Socket binding
    socket_t *sock;
} tcp_conn_t;
```

### 9.6 UDP Endpoint
```c
typedef struct {
    uint8_t dgrams[16][1500];           // Datagram payloads
    uint16_t dgram_lens[16];
    uint32_t dgram_addrs[16];
    uint16_t dgram_ports[16];
    uint8_t dgram_addrs6[16][16];
    int af[16];
    volatile int q_count;
    volatile int q_head, q_tail;
    spinlock_t lock;
    wait_queue_t recv_wait;
    uint32_t recv_timeout;              // ms
    bool bound;
    uint8_t ipv6only;
    // Bind parameters
    int bind_af;
    uint32_t bind_addr;
    uint8_t bind_addr6[16];
    uint16_t bind_port;
} udp_endpoint_t;
```

### 9.7 Protocol Details
- **TCP MSS**: 1460 bytes (1500 Ethernet MTU – 20 IP – 20 TCP)
- **TCP TIME_WAIT**: 60 seconds (RFC-suggested 2MSL)
- **SYN retransmit**: every 5 seconds
- **Data retransmission**: only last MSS-sized chunk buffered; RTO triggers retransmit
- **FIN retransmit**: 1s initial, 2s backoff, 60s cap
- **NDP cache**: NS on cache miss; poll-loop for NA; 3s timeout
- **DHCP**: 2 retries with 1.5s timeout; falls back to static 10.0.2.15
- **SLAAC**: RS to ff02::2; 2s timeout; prefix[64] + EUI-64 address
- **NTP**: 2 retries with 5s timeout; NTP→Unix epoch conversion (subtract 2208988800)
- **DNS**: A query first, then AAAA fallback; compression pointer support; 512-byte max packet

### 9.8 Networking Test Infrastructure
- 5 regression tests (no network backend required): tcp_find_conn_ipv6, ndp_cache_miss, icmpv6_ns_parse, socket_refcount, udp_queue_roundtrip
- Two-QEMU IPv6 TCP+UDP echo test: `make test-net-2qemu`
- `make test-net` runs all 5 regression tests with 120s timeout

---

## 10. Security Model (Stage 6 — Partially Implemented)

### 10.1 Already Enforced (unchanged from v1.0)
- SMAP / SMEP / NX / read-only kernel text / user address space confinement

### 10.2 To Implement (unchanged from v1.0)
- Capability system, user/group model, syscall filtering, ASLR for EXEC binaries

---

## 11. IPC and Services (Stage 7 — Not Implemented)

Contents unchanged from TRD v1.0 §11.

---

## 12. GUI Foundation (Stage 10 — Minimal)

Contents unchanged from TRD v1.0 §12.

---

## 13. Testing Requirements

| Test Case | Prerequisite | Pass Criterion |
|-----------|-------------|----------------|
| TC1: Page fault isolation | Stage 1 | User write to kernel address → SIGSEGV; no panic |
| TC2: Fork/exec/wait | Stage 2 | No memory leak after 1000 fork+exec+wait cycles |
| TC3: Scheduler starvation | Stage 1 | Priority-128 thread runs within 5 s under 255 priority-1 spinners |
| TC4: WAL crash recovery | Stage 3 | 100 files intact after simulated power loss and reboot |
| TC5: Capability revocation | Stage 6 | Revoking root token invalidates all derived tokens |
| TC6: Syscall fuzzing | Stage 9 | 1 M random syscalls; zero panics |
| TC7: Swap page fault | Stage 1 | Swapped-out page transparently restored on access |
| TC8: IPC flooding | Stage 7 | Rate limiter engages; receiver not starved |
| TC9: Nested interrupts | Stage 1 | Two nested IRQs both complete; no stack overflow |
| TC10: Block device failure | Stage 3 | FS enters read-only; shell continues |
| TC11: TCP connect+echo | Stage 5 | Two-QEMU IPv6 TCP echo test completes (16 bytes echo) |
| TC12: UDP send+recv | Stage 5 | Two-QEMU IPv6 UDP echo test completes |
| TC13: DHCP lease | Stage 5 | SLiRP boot obtains 10.0.2.15/24 via DHCP |
| TC14: DNS resolve | Stage 5 | `dns_resolve("google.com")` returns valid A record |
| TC15: NTP sync | Stage 5 | `ntp_get_time()` returns post-2020 epoch value |

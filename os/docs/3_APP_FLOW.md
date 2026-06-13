# OPERtur / TRY1 OS — App Flow Document

**Version:** 2.0  
**Scope:** All significant runtime flows — from power-on to userspace interaction — documented as step-by-step traces through the kernel layers.

---

## 1. Boot Flow

```
Power-on → BIOS/UEFI firmware POST
    │
    ▼
Bootloader (GRUB2 or custom)
  - Switches CPU to long mode
  - Loads kernel ELF to physical memory
  - Passes memory map, ACPI tables via boot info struct
    │
    ▼
Kernel entry (_start in boot.S)
  1. Load GDT (kernel CS=0x08, DS=0x10, user CS=0x1B, DS=0x23, TSS=0x28)
  2. Load IDT (256 entries, all stubs registered)
  3. Enable paging: map kernel to higher half (above 0xFFFFFFFF80000000)
  4. Zero BSS
  5. Call kmain()
    │
    ▼
kmain()
  1. UART init → serial console online
  2. CPUID: detect NX, SMAP, SMEP, FXSAVE, HPET, QEMU TCG
  3. PMM init: parse bootloader memory map → populate bitmap
  4. VMM init: per-CPU PML4 set up; SMAP/SMEP enabled
  5. Slab + buddy heap init
  6. HPET init: map MMIO (skip on QEMU TCG), read frequency, start counter
  7. PIC remap (IRQs to 0x20–0x2F); xAPIC enable (skip on QEMU TCG); PIT timer calibrate
  8. Scheduler init: idle thread created per CPU
  9. VFS init: SFS mount at /, tmpfs at /tmp, devfs at /dev
 10. Block device init: PCI scan → ATA identify → AHCI/NVMe probe → register block devices
 11. Work queue + kworker thread created
 12. Watchdog thread started
 13. Boot self-tests: PMM alloc/free, slab, page table walk
 14. NIC init (e1000): PCI probe, BAR0 MMIO, MAC read, descriptor rings, IRQ 11 handler
 15. Ethernet layer init: register EtherType handlers (ARP/IPv4/IPv6)
 16. ARP init, IPv4/IPv6 init, ICMP init, NDP init, routing table init
 17. UDP init, TCP init
 18. Sockets API init (net_init)
 19. NDP pre-seed: resolve own MAC for fast neighbour cache
 20. DHCP/SLAAC auto-configuration: obtain IP, subnet, gateway, DNS
 21. DNS init (resolver 8.8.8.8:53)
 22. NTP init (sync clock with pool.ntp.org)
 23. IGMP init (IPv4 multicast)
 24. NIC poll thread created: runs eth_rx_poll() + tcp_tick() every 10ms
 25. Network self-tests (if ENABLE_NET_TEST): 5 regression tests
 26. TTY init, PTY init
 27. Init process (PID 1) created via kernel_thread → execs /sbin/init
 28. Scheduler starts (STI; idle loop)
    │
    ▼
PID 1 (/sbin/init)
  - Sources /etc/rc startup script
  - /etc/rc starts background services with &
  - Forks → execs /bin/shell → interactive prompt
```

---

## 2. Syscall Flow

```
User program calls libc wrapper (e.g. write(1, buf, n))
    │
    ▼
libc stub:
  - Loads syscall number into RAX
  - Args in RBX, RCX, RDX, RSI, RDI, R8 (custom ABI; __syscall5/6 use r10, r8, r9)
  - Executes INT 0x80
    │
    ▼ (privilege ring 3 → ring 0)
IDT vector 0x80 → syscall_entry (asm stub)
  - Saves all GP registers to kernel stack frame
  - Loads kernel GS base (per-CPU data)
  - Calls syscall_handler(frame)
    │
    ▼
syscall_handler(frame)
  - Bounds-checks syscall number (0–52); else return -ENOSYS
  - Calls syscall_table[number](args)
    │
    ▼
Individual handler (e.g. sys_write)
  - Validates all pointer args: copy_from_user / is_user_range / is_user_memory_mapped
  - Performs operation (VFS dispatch, socket dispatch, etc.)
  - Returns int64_t result (negative = kernel ERR_* code)
    │
    ▼
kernel_err_to_posix() translation
  - Maps kernel ERR_* codes to POSIX errno (ERR_AGAIN→EAGAIN=11, etc.)
  - Sets result = -errno for error returns
    │
    ▼
syscall_entry return path
  - Restores GP registers from stack frame
  - IRET back to ring-3 RIP; user stack restored
    │
    ▼
libc wrapper
  - If result < 0: sets errno = -result; returns -1
  - Else: returns result directly
```

---

## 3. Process Creation Flow (fork + exec)

### 3.1 fork()
```
Parent calls sys_fork()
    │
    ▼
kernel sys_fork()
  1. Allocate new process_t; assign PID
  2. Copy PML4: walk parent's page tables; map same physical pages (full copy currently — COW planned Stage 8)
  3. Copy fd_table: increment refcounts on all open file descriptions
  4. Copy signal table (sigaction handlers)
  5. Copy current thread's register state (child's RAX ← 0; parent's RAX ← child PID)
  6. Allocate new kernel stack (16 KiB, 4 pages)
  7. Set child state = READY; enqueue in scheduler run queue
  8. Return child PID to parent; return 0 to child
```

### 3.2 exec()
```
Process calls sys_exec(path, argv, envp)
    │
    ▼
kernel sys_exec()
  1. copy_from_user: path string, argv array, envp array
  2. vfs_open(path) → read ELF header; validate magic + machine type
  3. Free current address space (unmap all user PT entries; free physical pages)
  4. Load PT_LOAD segments: for each segment → pmm_alloc → vmm_map → copy data from ELF
     (handles overlapping segments: reuse existing page mapping, add PAGE_WRITE if needed)
  5. If PT_INTERP present: load ld.so at random base; set entry = ld.so entry
  6. Map signal trampoline at SIGNAL_TRAMPOLINE_ADDR
  7. Build user stack: push envp strings, argv strings, aux vectors, envp[], argv[], argc
  8. Set RSP = top of user stack; RIP = ELF entry (or ld.so entry)
  9. IRET to ring 3
```

---

## 4. Interrupt Handling Flow

```
Hardware event (e.g. timer tick, E1000 IRQ, ATA IRQ, UART RX)
    │
    ▼
CPU fetches IDT entry for vector N
  - Pushes SS, RSP, RFLAGS, CS, RIP (+ error code if applicable) to kernel stack
  - Loads kernel CS; jumps to IDT stub
    │
    ▼
IDT stub (asm, irq_stubs.S)
  - Pushes dummy error code if not supplied by CPU
  - Pushes vector number
  - Calls common_isr_handler(frame)
    │
    ▼
common_isr_handler(frame)
  - Identifies source: CPU exception (0–31), IRQ (32+), or software int
  - For IRQs: calls registered driver handler (e.g. e1000_irq_handler, ata_irq_handler, timer_isr)
  - For exceptions: dispatches to page_fault_handler, gpf_handler, etc.
  - Sends EOI to APIC/PIC
  - Returns to stub
    │
    ▼
Timer ISR (every tick, ~1 ms via PIT, or faster via APIC timer)
  - Increments monotonic tick counter
  - Checks sleep queue: wake any thread whose sleep_until_ns ≤ now
  - Checks preemption: if current thread's timeslice expired → set need_resched flag
  - On ISR return: if need_resched → call schedule()
    │
    ▼
schedule()
  - Saves current thread context
  - Picks next READY thread (highest effective priority)
  - Restores next thread context
  - Switches CR3 if process changed
  - Returns (into next thread's context)
```

---

## 5. Page Fault Flow

```
CPU executes instruction that faults (access violation or unmapped page)
    │
    ▼
CPU pushes error code + RIP onto kernel stack; jumps to vector 14 stub
    │
    ▼
page_fault_handler(frame, fault_addr=CR2)
    │
    ├─ Is fault_addr in kernel space?
    │       Yes → kernel_panic("kernel page fault at 0x...") with page-table walk dump
    │
    ├─ Is fault_addr in user space? Check PTE:
    │
    │   ├─ PTE present=0 AND swap marker set?
    │   │       → swap_in(fault_addr): read slot from RAM store → allocate physical page → map → resume
    │   │
    │   ├─ PTE present=0 AND no swap marker?
    │   │       → demand-zero: allocate physical page → zero fill → map → resume
    │   │
    │   ├─ PTE present=1 AND write fault AND COW bit set? (Stage 8)
    │   │       → copy physical page → map new page RW → resume
    │   │
    │   └─ Otherwise: invalid access
    │           → deliver SIGSEGV to faulting thread → kill process
    │
    └─ IRET back to faulting instruction (retry)
```

---

## 6. Filesystem Read Flow

```
User calls read(fd, buf, n)
    │
    ▼
sys_read(fd, ubuf, n)
  1. Validate fd: range check → sock_lookup() for socket FD, otherwise fd_table[fd]
  2. Validate ubuf: is_user_range(ubuf, n)
  3. For socket FD: dispatch to sock_ops->recv (tcp_sock_recv or udp_sock_recv)
  4. For file FD: look up vnode, call vfs_read(vnode, kernel_buf, n, file->offset)
    │
    ▼
vfs_read dispatches to mounted FS (e.g. sfs_read)
  1. Compute block number = offset / BLOCK_SIZE; block_offset = offset % BLOCK_SIZE
  2. Resolve block number through inode: direct (0–11), singly-indirect (12–…), doubly-indirect
  3. sector_cache_read(dev, lba): check LRU cache; if miss → block_read(dev, lba, buf); insert into cache
  4. Copy data from cache entry into kernel_buf; advance offset
  5. Repeat until n bytes read or EOF
    │
    ▼
sys_read copies kernel_buf → ubuf via copy_to_user
Updates file->offset
Returns bytes_read to userspace
```

---

## 7. Shell Command Execution Flow

```
User types: ls -la /etc | grep config > out.txt
    │
    ▼
readline() line editor collects input → returns complete line on Enter
  - Readline features: left/right arrows, Home/End, backspace/delete, Ctrl-U/K/W/A/E
  - Tab completion: commands + aliases + VFS file paths
  - History: up/down arrows cycle through 64-entry circular buffer
  - Aliases expanded before parsing
    │
    ▼
shell parser
  1. Tokenise: split on spaces, handle quotes, $VAR expansion, $? exit code
  2. Detect pipe character '|' → split into pipeline stages: ["ls -la /etc", "grep config"]
  3. Detect redirection '>' → record output_file = "out.txt"
  4. Detect '&' for background → not present; foreground job
    │
    ▼
pipeline setup (N=2 stages)
  1. Allocate pipe buffers: pipe[0] = {read_end, write_end}
  2. For stage 0 (ls):
       fork() → child: close stdout; dup2(pipe[0].write_end, 1); exec("ls", ["-la", "/etc"])
  3. For stage 1 (grep):
       fork() → child: close stdin; dup2(pipe[0].read_end, 0);
                        close stdout; open("out.txt", O_WRONLY|O_CREAT|O_TRUNC) → dup2 to 1;
                        exec("grep", ["config"])
  4. Parent: close all pipe ends; set fg_pgid = pgid of pipeline
    │
    ▼
Shell waits: waitpid(-pgid, ...) until all stages exit
TTY driver routes Ctrl-C SIGINT to fg_pgid during wait
SIGTTIN if background process reads TTY
SIGTTOU if background process writes TTY with TOSTOP set
    │
    ▼
Shell prints next prompt
```

---

## 8. Signal Delivery Flow

```
sys_kill(target_pid, SIGTERM) called (by shell or user process)
    │
    ▼
signal_send(target_proc, SIGTERM)
  - Sets bit SIGTERM in target_proc->signals.pending mask
  - If target thread is sleeping: wake it (dequeue from sleep queue)
    │
    ▼
On next scheduler return to target process (or explicit check in syscall return path):
signal_process(target_proc)
  1. Find lowest set bit in pending & ~blocked mask → signo
  2. Look up sigaction[signo]:
       - SIG_DFL + SIGTERM → sys_exit(target_proc, 0)
       - SIG_DFL + SIGSTOP → suspend thread (STOPPED state)
       - SIG_DFL + SIGTTIN/SIGTTOU → suspend thread (job control stop)
       - Custom handler → build sigframe_t on user stack:
            push saved RIP, RFLAGS, GP registers
            push signo as first argument
            set user RIP = handler address
            set user RSP = &sigframe
    │
    ▼
IRET into signal handler (ring 3)
Handler runs; at end calls SYS_SIGRETURN (via trampoline at SIGNAL_TRAMPOLINE_ADDR)
    │
    ▼
sys_sigreturn:
  - copy_from_user sigframe from user stack
  - Restore saved RIP, RFLAGS, GP registers into CPU frame
  - IRET resumes at original interrupted instruction
```

### 8.1 ISR-Level Signal Delivery
```
UART ISR receives byte (e.g. Ctrl-C = 0x03)
  → tty_input_push() detects SIGINT character
  → Schedules tty_signal_worker() on system work queue
  → Worker calls signal_send_pgid(tty->fg_pgid, SIGINT)
  → Process receives signal on next kernel entry/return
```

---

## 9. Block Write + Journal Flow

```
vfs_write(vnode, data, n, offset)
    │
    ▼
sfs_write
  1. Identify blocks to write; allocate new blocks if needed
  2. Begin journal transaction: wal_begin_txn()
  3. For each dirty block:
       a. Write DATA entry to WAL ring (slot = (head++) % 63)
       b. Write actual data to sector cache (dirty bit set)
  4. Write COMMIT entry to WAL
  5. wal_commit(): flush WAL entries to block device (sector_cache_flush)
  6. Write actual data blocks to block device via sector cache eviction
  7. Checkpoint: mark WAL slot as free
    │
    ▼
On crash before COMMIT: recovery skips incomplete transaction
On crash after COMMIT: recovery replays DATA entries → data consistent
```

---

## 10. Networking Flows (Stage 5 — Implemented)

### 10.1 Socket Connect Flow (TCP)
```
sys_socket(AF_INET6, SOCK_STREAM, 0) → allocate socket_t with tcp_ops; return fd
sys_bind(fd, {src_addr, port}, addrlen) → tcp_conn_bind()
sys_connect(fd, {dst_addr, port}, addrlen)
  → tcp_conn_connect() allocates tcp_conn_t, sets state = SYN_SENT
  → Build SYN segment (IPv4 or IPv6 header + TCP header)
  → ipv4_output() or ipv6_output()
  → arp_resolve() or ndp_resolve() → eth_send()
  → E1000 TX descriptor ring → wire
  → Block in poll loop (releases tcp_lock, calls eth_rx_poll + thread_sleep)
  → On SYN+ACK received: tcp_input processes, transitions to ESTABLISHED
  → on_connect callback fires (sets up on_recv)
  → Send ACK → return 0 to user
```

### 10.2 Socket Listen/Accept Flow (TCP)
```
sys_socket(AF_INET6, SOCK_STREAM, 0) → allocate socket_t; return fd
sys_bind(fd, {addr, port}, addrlen) → tcp_conn_bind()
sys_listen(fd, backlog) → tcp_conn_listen()
  → sets conn->state = TCP_LISTEN
  → NIC poll thread (or eth_rx_poll during blocking ops) receives incoming SYN
  → tcp_input on LISTEN: allocate new tcp_conn_t, state = SYN_RECEIVED, send SYN+ACK
  → On ACK (third handshake): transition to ESTABLISHED, increment accept_count
sys_accept(fd, addr, addrlen)
  → Block until accept_count > 0 (eth_rx_poll loop)
  → Dequeue accepted connection → create new socket_t → return new fd
  → New fd connected; original listening socket stays LISTEN
```

### 10.3 UDP Send/Receive Flow
```
sys_socket(AF_INET6, SOCK_DGRAM, 0) → allocate socket_t with udp_ops; return fd
sys_bind(fd, {addr, port}, addrlen)
  → udp_bind_endpoint() creates udp_endpoint_t in udp_endpoints[]
sys_sendto(fd, buf, len, flags, {dst_addr, dst_port}, addrlen)
  → udp_sendto() builds UDP header + payload → ipv4/ipv6_output → eth_send
sys_recvfrom(fd, buf, len, flags, {src_addr, src_port}, addrlen)
  → udp_endpoint_dequeue() checks q_count
  → If no data: call eth_rx_poll() + thread_sleep(recv_timeout)
  → Copy datagram from endpoint queue to user buffer
```

### 10.4 DNS Resolution Flow
```
dns_resolve("google.com")
  → Build DNS query: transaction ID, flags=0x0100 (standard query, recursion desired)
  → Encode "google.com" as 3label+5label+3label
  → A record query type (0x0001), class IN (0x0001)
  → udp_sendto to 8.8.8.8:53 from ephemeral source port
  → udp_bind_endpoint() + udp_endpoint_dequeue() with 5s timeout
  → Parse response: match transaction ID, extract A records from answer section
  → Return first resolved IPv4 address
  → Fallback: if no A record, try AAAA query
```

### 10.5 DHCP Flow
```
dhcp_configure()
  → udp_sendto DISCOVER from 0.0.0.0:68 to 255.255.255.255:67
  → Wait for OFFER (1.5s timeout, 2 retries)
  → Send REQUEST with offered server ID + requested IP
  → Wait for ACK (1.5s timeout, 2 retries)
  → Parse options: subnet mask, router, DNS server, lease time
  → ipv4_set_addr(), route_add_v4() for subnet + default gateway
  → On timeout: attempt SLAAC (IPv6); on total failure: fall back to 10.0.2.15/24
```

### 10.6 SLAAC Flow
```
slaac_configure()
  → icmpv6_send_rs() to ff02::2 (all-routers multicast)
  → Register RA callback; wait for RA (2s timeout)
  → Parse Prefix Information Option (type 3, flag A=autonomous)
  → Form global unicast: prefix[64] + EUI-64 from MAC
  → ipv6_set_addr()
  → Add route: prefix/64 via RA source, default ::/0 via RA source
```

### 10.7 NTP Sync Flow
```
ntp_init()
  → Build NTP v4 mode 3 request: 48 bytes, LI=0, VN=4, Mode=3
  → udp_sendto to pool.ntp.org:123
  → Wait for response (5s timeout, 2 retries)
  → Parse: validate VN=3/4, Mode=4 (server)
  → Extract Transmit Timestamp at offset 40
  → Convert NTP→Unix: subtract 2208988800
  → Store boot_time = now_ntp - uptime_seconds
  → ntp_get_time() = boot_time + uptime_seconds
```

### 10.8 IGMPv2 / MLDv1 Flow
```
ipv4_mcast_join(group_ip)
  → igmp_join_group(group_ip)
  → Send IGMPv2 Membership Report (type 0x16) to group_ip
  → Add to 8-group multicast table
  → On query (type 0x11): send delayed report

ipv6_mcast_join(group_ip6)
  → mldv1_send_report() (ICMPv6 type 131) with multicast address record
  → Add to ipv6_mcast_groups table
  → Program E1000 MTA via CRC-32 for hardware filtering
  → On MLD query (type 130): send reports for all groups
```

---

## 11. Planned Flows (Not Yet Implemented)

### 11.1 Service Start Flow (Stage 7)
```
Service manager reads /etc/services.d/named.service
  → Topological sort of dependency graph
  → For each service in order:
       fork() → exec(binary, args)
       Record pid in service table
       Start watchdog ping timer (5 s interval)
  → On ping miss × 3: restart service per restart_policy
```

### 11.2 Capability-Gated File Open (Stage 6)
```
sys_open(path, flags)
  → cap_validate(current_proc->cap_table, CAPOBJ_FILE, path, CAP_READ)
  → If valid: proceed to vfs_open
  → If invalid: return -EPERM; emit audit log entry
```

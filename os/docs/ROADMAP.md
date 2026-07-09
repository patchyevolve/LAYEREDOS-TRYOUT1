# OPERtur / TRY1 OS — Staged Development Roadmap

**Legend:** ✅ Implemented · 🟡 Partial · ❌ Not implemented

---

## STAGE 1 — Core Kernel Foundation `~100%`

### Boot and Hardware

- ✅ **Stable boot path and higher-half mapping** — Bootstraps from bootloader into a higher-half virtual layout; kernel lives above `0xFFFFFFFF80000000`, leaving the lower address space free for userspace.
- ✅ **GDT, IDT, TSS, and interrupt stubs** — GDT sets up kernel/user segments; IDT registers handlers for all 256 vectors; TSS supplies the kernel stack pointer on privilege transitions.
- 🟡 **PIC/APIC interrupt routing** — Legacy 8259 PIC active (remapped to `0x20–0x2F`). xAPIC detected via CPUID and MSR; MMIO mapping skipped on QEMU TCG via runtime `hal_is_qemu_tcg()` CPUID detection — falls back to legacy PIC on TCG. APIC timer active on KVM/bare metal. TCG fallback is intentional: the TCG softmmu page-walk cache does not observe page-table writes to cached boot pages, causing invisible page faults on the APIC MMIO region.
- ✅ **UART/serial console** — 16550-compatible UART driver provides early-boot logging and a kernel debug console before any framebuffer is available.
- 🟡 **PIT and HPET timer** — HPET present at MMIO `0xFED00000` (3 timers, 100 MHz); MMIO mapping skipped on QEMU TCG via `hal_is_qemu_tcg()` detection (same TCG softmmu cache issue as APIC). Falls back to PIT for scheduling tick on TCG. HPET ns-resolution timekeeping available on KVM/bare metal.
- ✅ **Basic CPU feature detection** — CPUID probes for SSE/SSE2, NX, FXSAVE, and other features at boot; results gate optional code paths and capability flags.
- ✅ **Panic path with debug output** — `kernel_panic()` freezes the system, dumps registers, a backtrace, and the panic string to the serial console.
- ✅ **Watchdog / health checking** — A software watchdog tracks per-CPU heartbeat ticks; a missed deadline triggers an NMI-based diagnostic dump.

### Memory Management

- ✅ **Physical memory manager** — Bitmap-based PMM tracks 4 KiB frames across all NUMA regions reported by the bootloader memory map.
- ✅ **Virtual memory manager** — Per-process PML4 page tables managed with a recursive-mapping trick; supports 4 KiB, 2 MiB, and 1 GiB pages.
- ✅ **Kernel heap allocator** — Two-level allocator: slab caches for common object sizes (8–512 bytes) plus a buddy allocator for large allocations.
- ✅ **Page fault handling** — `#PF` ISR distinguishes COW faults, demand-paging, and genuine access violations; illegal accesses in kernel context panic.
- ✅ **User/kernel address split** — Canonical address space split at `0x0000800000000000`; kernel mappings are non-executable from user mode and not visible in user PTEs.
- ✅ **Copy-from-user / Copy-to-user** — Safe cross-boundary copy routines that catch page faults mid-copy and return `-EFAULT` rather than panicking.
- ✅ **OOM handling** — When allocation fails after reclaim attempts, an OOM killer selects and terminates the lowest-priority process to free memory.
- ✅ **Swap support** — RAM-backed swap backing store with slot bitmap allocator; page-level swap-out/swap-in with PTE marker encoding/decoding. Page fault handler checks for swapped PTEs and transparently swaps pages back in. Eviction policy not yet implemented (swap-in only on demand).
- ✅ **Double-free detection** — Slab allocator checks a poison pattern on the free path; freeing an already-free object trips an immediate kernel BUG.
- ✅ **Allocation fast paths** — Per-CPU magazine caches allow lock-free allocation for the most common slab sizes in the hot path.
- ✅ **Page permission flags** — R/W/X/U/G bits enforced at page-table level; page tables themselves are mapped read-only after construction.

### Scheduler and Threading

- ✅ **Preemptive scheduler** — Multilevel feedback queue with timer-driven preemption; CPU is never voluntarily ceded while runnable threads exist.
- ✅ **Thread control blocks** — TCB stores register state, kernel stack pointer, scheduling parameters, and thread-local storage base.
- ✅ **Context switching** — Full register save/restore including FPU/SSE state via FXSAVE/FXRSTOR on context switch.
- ✅ **Sleep and wake queues** — Threads block on typed wait-queues; wakeup is O(1) for the first waiter and supports broadcast.
- ✅ **Mutexes** — Kernel mutexes with priority-ceiling bookkeeping; spin briefly then sleep to avoid thundering-herd on short critical sections.
- ✅ **Condition variables** — `condvar_wait/signal/broadcast` built on the wait-queue primitive; paired with a mutex to avoid lost-wakeup races.
- ✅ **Priority inheritance** — When a high-priority thread blocks on a mutex held by a lower-priority thread, the holder temporarily inherits the higher priority.
- ✅ **Priority aging** — Threads that have not run for a long time receive a temporary priority boost to prevent indefinite starvation.
- ✅ **SYS_CLONE** — Linux-compatible clone syscall; flag set controls which resources (address space, FD table, signal handlers) are shared vs. copied.
- ✅ **Process abstraction** — Process descriptor wraps one or more threads with a shared address space, signal table, and resource accounting.
- ✅ **fork** — Copy-on-write fork duplicates the address space lazily; child inherits open file descriptors and signal disposition.
- ✅ **exec** — Replaces the current address space with a freshly loaded ELF image; handles argv/envp passing and stack setup.
- ✅ **exit** — Tears down the address space, closes all FDs, and posts SIGCHLD to the parent; zombie state held until `wait()`.
- ✅ **wait** — `waitpid()` reaps zombie children and returns exit status; supports `WNOHANG` and `WUNTRACED` flags.
- ✅ **Per-process kernel stacks** — Each process has a dedicated 8 KiB kernel stack mapped in the kernel address space, with a guard page below.
- ✅ **Per-process address spaces** — CR3 is switched on every context change between processes; ASID/PCID tags avoid full TLB flushes where supported.
- ✅ **Signal delivery with sigreturn** — Signal numbers, default actions (TERM/STOP/CONT/IGN), sys_kill, sys_sigaction, signal_send/signal_process all implemented. Custom handler delivery via `sigframe_t` on user stack, trampoline at `SIGNAL_TRAMPOLINE_ADDR` mapped in every process, `SYS_SIGRETURN` syscall restores saved context.
- ✅ **Cleanup and reaping** — Process exit path frees all kernel resources; page tables, slab objects, kernel stack, and FD table are fully released.

### Exit Criteria

- [x] Kernel boots to shell on bare metal or QEMU
- [x] Physical memory allocator passes alloc/free round-trip
- [x] Page tables enforce user/kernel split
- [x] Preemptive scheduler runs at least two threads concurrently
- [x] fork + exec + wait completes without leak
- [x] Kernel panic produces register dump and halts cleanly

---

## STAGE 2 — Userspace Foundation `100%`

### Syscall Layer

- ✅ **Syscall gateway** — int 0x80 fast path with full register save; syscall table maps 90 syscall numbers (0–89) to kernel handlers.
- ✅ **Syscall validation** — All pointer arguments validated against user address range via copy_from_user/copy_to_user; invalid addresses return `-EFAULT` before any kernel state is touched.
- ✅ **Stable userspace ABI** — 90 syscalls (0–89) with stable numbers including: sockets (38–52), capabilities (54–55), audit (56), UID/GID (57–62), CSPRNG (63), syscall filter (64), socketpair (65), prctl (66), secure boot (67), network namespaces (68–71), filesystem ops (72–88), scheduler affinity (89). Syscall numbers are stable within the build.

### Userspace Runtime

- ✅ **Userspace libc** — Full libc library (`libc.a`): stdio (`printf`/`puts`/`snprintf`/`getchar`/`putchar`), stdlib (`malloc`/`free`/`calloc`/`realloc`/`atexit`/`exit`), string (`strlen`/`strcpy`/`strcmp`/`memcpy`/`memset`), unistd (syscall wrappers for read/write/open/close/fork/execve/wait/pipe/dup2/ioctl/chdir/getcwd/sleep/kill/lseek/sbrk), signal, errno, crt0. Statically linked into user binaries.
- ✅ **ELF loading** — Static ELF64 binaries parsed and loaded; PT_LOAD segments mapped with correct permissions; PIE support with ASLR.
- ✅ **Dynamic loader support** — ld.so dynamic linker/loader (ET_DYN PIE) loaded by kernel via PT_INTERP; ELF parsing, symbol resolution, RELA/PLT relocations, shared library loading via mmap, aux-vector stack setup, init/fini array calling.

### Process Environment

- ✅ **File descriptor table** — Per-process FD table (32 slots) with reference-counted file descriptions; `dup`/`dup2`/`close`/`open` all operate correctly.
- ✅ **stdin/stdout/stderr wiring** — File descriptors 0/1/2 are wired to the console TTY through VFS; dup2 syscall enables custom FD wiring. Full TTY line discipline (canon/raw, echo, signal chars, job control) implemented.
- ✅ **Init process** — PID 1 is launched at boot; it reaps orphaned children.
- ✅ **Service launcher** — /etc/rc startup script sourced at boot by the shell; shell runs rc file before showing the prompt. Services can be started from the rc script with `&` for background execution.

### Exit Criteria

- [x] Syscall gateway rejects invalid pointers with EFAULT
- [x] Static ELF binary loads and runs to completion
- [x] FD table handles open/dup/close correctly
- [x] Init process (PID 1) reaps orphaned children

---

## STAGE 3 — Storage and Filesystem `100%`

### Storage

- ✅ **PCI enumeration** — Full PCI/PCIe configuration-space scan at boot; BAR mapping implemented.
- ✅ **ATA/AHCI/NVMe support** — ATA PIO (IRQ-driven, primary/secondary channels, IDENTIFY, LBA48), AHCI DMA (PCI class 0x01/0x06, ABAR mapping, HBA reset, port probe, command list, PRD DMA, IDENTIFY), NVMe (PCI class 0x01/0x08, BAR0 MMIO, admin submission/completion queues, IDENTIFY controller/namespace, I/O queue pair, PRP DMA read/write). All three register as block devices.
- ✅ **IRQ-based block I/O** — ATA PIO uses `sched_block`/`sched_wake` on per-drive wait queues; IRQ handlers for IRQs 14/15; timeout watchdog.
- ✅ **Block device abstraction** — Uniform read/write block interface; devices addressed by ID and sector number.
- ✅ **Sector cache** — LRU write-back cache (64 entries, dirty tracking, eviction flushes to device).

### Filesystem

- ✅ **Writable filesystem (SFS)** — Custom on-disk format with superblock, bitmaps, inode table, direct+indirect blocks.
- ✅ **VFS** — Virtual Filesystem Switch with mount table, path resolution, symlink following.
- ✅ **File creation** — `vfs_create` allocates inode and directory entry; data blocks reserved on demand.
- ✅ **File deletion** — `vfs_unlink` removes directory entry; inode and blocks freed when last reference dropped.
- ✅ **Read/write/truncate** — `vfs_read`/`vfs_write` handle arbitrary offsets; truncate extends or shrinks a file.
- ✅ **Directory support** — `mkdir`/`rmdir`/`readdir`; directory entry compaction on remove.
- ✅ **Multi-mount VFS** — Up to 8 mount points with longest-prefix matching.
- ✅ **tmpfs** — RAM-backed tmpfs at `/tmp`; dynamic block allocation.
- ✅ **devfs** — Device filesystem at `/dev` with null, zero, random, full devices.

### Filesystem Reliability

- ✅ **Journaling/WAL** — Write-ahead log with DATA+COMMIT entries (63 slots); recovery replays committed transactions; checkpoint on commit; mid-txn commit-and-restart on full journal.
- ✅ **Better inode model** — Direct + singly-indirect + doubly-indirect blocks (~8 MB max file size).
- ✅ **Sparse files** — Reads zero-fill holes; writes skip unallocated blocks; truncate handles all block levels.
- ✅ **Atomic updates** — Rename overwrites target atomically within a single journal transaction.
- ✅ **Crash recovery** — Journaling + fsck (read-only check and repair mode).
- ✅ **Snapshot/rollback** — Full-device point-in-time snapshot (`snap take`); rollback restores saved blocks and remounts (`snap rollback`).
- ✅ **Backup/restore** — Full recursive archive format (`backup save <archive>` / `backup restore <archive>`) with file, directory, and symlink support.

### Filesystem Features

- ✅ **Permissions** — POSIX-style rwx bits checked on VFS operations; chmod shell command.
- ✅ **Locking** — Advisory file locks; lock/unlock shell commands.
- ✅ **Metadata timestamps** — atime/mtime/ctime maintained in inode and stat output.
- ✅ **Hard links** — Multiple directory entries can reference same inode; nlink tracked.
- ✅ **Symbolic links** — Symlinks stored as target path; VFS follows with loop limit.
- ✅ **Append mode** — O_APPEND and shell `>>` redirection supported.
- ✅ **Rename support** — vfs_rename; atomic within same filesystem.

### Exit Criteria

- [x] File survives write → read cycle
- [x] Directory create/delete works
- [x] VFS correctly dispatches across two mounted filesystems
- [x] tmpfs and devfs mount cleanly at boot
- [x] File permissions block unauthorized write

---

## STAGE 4 — Terminal and User Environment `100%`

### Shell

- ✅ **Real TTY layer** — Full line discipline (canonical/raw mode, echo, signal generation, Ctrl-C SIGINT, Ctrl-Z SIGTSTP, Ctrl-\ SIGQUIT, Ctrl-D EOF, Ctrl-U/K/W kill-chars). ISR-level signal detection via work queue. Process groups with `pgid` field; shell manages `fg_pgid` for job control. **Termios ioctl** (TCGETATTR/TCSETATTR/TIOCGPGRP/TIOCSPGRP via SYS_IOCTL=34). **PTY subsystem** (8-slot pool, master-slave VFS file_ops, line discipline, echo, signal chars, termios ioctl; SYS_PTY_PAIR=37). **SIGTTIN/SIGTTOU** — background read delivers SIGTTIN; background write with TOSTOP delivers SIGTTOU. `/dev/ttyS0` in devfs.
- ✅ **Line editing** — Readline-style in-place character editing with cursor movement, backspace, and kill-line.
- ✅ **History** — Command history stored in memory; up/down arrows cycle through previous entries.
- ✅ **Tab completion** — Filesystem path and builtin-name completion on Tab; ambiguous completions print a candidate list.
- ✅ **Pipes** — Anonymous pipes connect adjacent commands in a pipeline; FD duplication wires stdout of one process to stdin of the next.
- ✅ **Redirection** — `>`, `>>`, `<`, and here-documents are parsed and applied before the child process is exec'd.
- ✅ **Environment variables** — Per-process environment stored as a key=value array; `export`/`unset` builtins manipulate it.
- ✅ **Scripting** — Shell scripts are interpreted line-by-line; `if`/`while`/`for` constructs and function definitions supported.
- ✅ **Job control** — `&`, `fg`, `bg`, `jobs`; background processes tracked in a job table per shell session.
- ✅ **Builtins** — `cd`, `pwd`, `echo`, `export`, `unset`, `kill`, `exit`, and others handled directly by the shell without forking.

### Userland Tools

- ✅ **File utilities** — `ls`, `cp`, `mv`, `rm`, `mkdir`, `cat`, `find`, `stat` — standard file manipulation tools implemented as standalone binaries.
- ✅ **Process utilities** — `ps`, `kill`, top-like display; read data directly from kernel interfaces (`sched_foreach` and process table), not via a mounted procfs.
- ✅ **Text tools** — `grep`, `sed`, `cut`, `wc`, `head`, `tail` — line-oriented text processing utilities.

### Exit Criteria

- [x] Shell launches and accepts input after boot
- [x] Pipe between two commands produces correct output
- [x] File redirection writes and reads correctly
- [x] Tab completion suggests valid paths
- [x] Script with loop and conditional runs without error

---

## STAGE 5 — Networking `100%`

### Link Layer

- ✅ **NIC driver support** — E1000 driver (e1000.c + e1000.h): PCI detection of Intel 82540EM/82545EM/82573L/82574L, BAR0 MMIO mapping, software reset, MAC address read, 32-entry RX/TX descriptor rings with DMA bounce buffers, send/poll API, IRQ 11 handler. Wired into NIC abstraction (nic.h) and main.c init sequence.
- ✅ **Ethernet** — Frame encode/decode, EtherType dispatch table with handler registration, broadcast/unicast send, RX polling from E1000 IRQ handler. Supports ETHERTYPE_IPV4 (0x0800), ETHERTYPE_ARP (0x0806), ETHERTYPE_IPV6 (0x86DD). Shell commands: `nicstat`, `eth_test`.
- ✅ **E1000 MTA** — Multicast Table Array programming via CRC-32 over multicast MAC; `ipv6_mcast_update_mta()` programs MTA for all joined groups.

### Network Layer

- ✅ **ARP** — ARP cache with 16-entry table, packet construction for IPv4→MAC resolution, gratuitous ARP, broadcast detection.
- ✅ **NDP** — Neighbor Solicitation/Advertisement exchange for IPv6 address resolution; solicited-node multicast targets; SLLAO/TLLAO options; ndp_cache_lookup/update/resolve with poll-loop on cache miss.
- ✅ **IPv4** — Packet routing, send/recv, `ipv4_set_addr()`/`ipv4_get_addr()`, broadcast address acceptance, ipv4_send_from() with explicit source address.
- ✅ **IPv6** — Packet routing, send/recv, link-local EUI-64 address formation from MAC, solicited-node multicast group (FF02::1:FFxx:xxxxx), routing decisions (direct vs gateway).
- ✅ **ICMPv4** — Echo request/reply (ping), registered as IP protocol 1 handler.
- ✅ **ICMPv6** — Echo request/reply (ping), NS/NA processing with target address validation, RS/RA for SLAAC, RA callback registration.
- ✅ **IGMPv2** — IPv4 multicast group management (8-group table). Membership Report (0x16) on join, Leave Group (0x17) on leave, Membership Query (0x11) handler. Registered as IP protocol 2.
- ✅ **MLDv1** — IPv6 multicast listener discovery. Query handler (type 130) sends reports for all groups on general query. Report (131) sent on join, Done (132) on leave.

### Transport Layer

- ✅ **UDP** — Full sendto/recvfrom via endpoint-based datagram queue (16-datagram ring buffer per endpoint); checksum validation on receive; dual-stack (IPv4 + IPv6); `udp_bind_endpoint` with flat parameters (no stack-allocated endpoint struct).
- ✅ **TCP** — Complete RFC 793 state machine: CLOSED/LISTEN/SYN_SENT/SYN_RECEIVED/ESTABLISHED/FIN_WAIT1/FIN_WAIT2/CLOSE_WAIT/CLOSING/LAST_ACK/TIME_WAIT.
  - SYN retransmission every 5s while waiting for SYN+ACK
  - Data retransmission (single-segment buffer, RTO backoff)
  - FIN retransmission (1s initial, 2s backoff, 60s cap)
  - ACK tracking (snd_una, snd_wnd)
  - TIME_WAIT with 60s 2MSL timer
  - RST generation for all states (both IPv4 and IPv6)
  - Lock-safe sends (tcp_lock released before tcp_send_pkt to prevent NDP/ARP deadlock)
  - TCP_NODELAY/Nagle delay support (`nodelay`/`nagled` flags)
  - SO_RCVTIMEO/SO_SNDTIMEO on TCP connections
  - IPv4-mapped IPv6 (::ffff:x.x.x.x)
  - poll() with POLLIN/POLLOUT/POLLERR
  - `tcp_tick()` infrastructure for per-connection periodic processing

### User Networking

- ✅ **DNS** — Kernel-level `dns_resolve()` using raw UDP (no socket dependency); A query first, AAAA fallback; transaction ID matching; compression pointer support; configurable resolver address via `dns_set_resolver_v4()`/`dns_set_resolver_v6()`.
- ✅ **DHCP client** — Kernel-level DORA (DISCOVER/OFFER/REQUEST/ACK); broadcast from 0.0.0.0:68 to 255.255.255.255:67; option parsing (subnet mask, router, server ID, lease time); 2 retries with 1.5s timeout; auto-configures IPv4 address + subnet route + default gateway.
- ✅ **SLAAC** — RS to ff02::2; RA callback via `icmpv6_set_ra_callback()`; Prefix Information Option parsing; EUI-64 global address formation; route add for prefix/64 and default ::/0.
- ✅ **NTP client** — Kernel-level NTPv4 mode 3; 48-byte request/response; NTP→Unix epoch conversion; `ntp_get_time()` = boot_time + uptime; configurable server via `ntp_set_server_v4()`; clock_gettime syscall (CLOCK_REALTIME/CLOCK_MONOTONIC).
- ✅ **Sockets API** — Full BSD socket syscalls: `socket/bind/connect/listen/accept/send/recv/sendto/recvfrom/close/setsockopt/getsockopt/poll`. User→kernel AF translation (POSIX 2/10 → kernel 4/6). Refcounted socket_t with sock_ops_t dispatch. 32-entry fd table with socket-aware sys_close.
- ✅ **Error propagation** — `kernel_err_to_posix()` translation table maps kernel ERR_* values to POSIX errno at syscall boundary.
- ❌ **TLS** — No TLS implementation or crypto library beyond SHA-256. **Plan:** `docs/7_TLS_PLAN.md` — userspace `libtls.a` wrapping TCP sockets with TLS 1.3 (AES-128-GCM, X25519, ChaCha20-Poly1305). ~2850 lines, all userspace.

### Isolation

- ✅ **Network namespaces** — Per-process network stack isolation via `net_ns_t` struct. `unshare(CLONE_NEWNET)` creates new empty namespace. Fork inherits parent namespace. Veth pairs for cross-namespace communication. `sys_netconfig`/`sys_veth_move` syscalls for userspace namespace management. See `docs/8_NETNS_PLAN.md`.

### Network Services

- ✅ DHCP client
- ✅ SLAAC
- ✅ NTP client

### Exit Criteria

- [x] NIC detected by driver
- [x] DHCP lease obtained
- [x] Ping works (IPv4 + IPv6)
- [x] DNS resolves
- [x] TCP connection succeeds
- [x] HTTP request succeeds (via SLiRP hostfwd)

---

## STAGE 6 — Security and Service Infrastructure `~100%`

### Identity

- ✅ **User/group identity** — `uid_t`/`gid_t` types; `uid`/`gid`/`euid`/`egid` per process. Init (pid=1) gets uid=0. Inherited on fork. Syscalls: `getuid`(57), `geteuid`(58), `getgid`(59), `getegid`(60), `setuid`(61), `setgid`(62). Userspace wrappers in libuser.
- ✅ **DAC permission model** — `vfs_access_check()` enforces POSIX owner/group/other bits using process euid/egid vs file uid/gid + mode. Root (euid=0) bypasses; `CAP_DAC_OVERRIDE` bypasses. SFS/DEVFS/TMPFS all propagate uid/gid. `vfs_stat_t` extended with uid/gid fields.
- ✅ **Capability system** — Four capabilities (CAP_SYS_BOOT, CAP_KILL, CAP_NET_RAW, CAP_SYS_ADMIN, CAP_DAC_OVERRIDE, CAP_SYS_SETUID). `capget`/`capset` syscalls allow processes to drop privileges; `cap_check()` enforced at sensitive syscall boundaries (reboot, poweroff, kill-any, setpgid-other). Capabilities inherited on fork.

### Isolation

- ✅ **Sandboxing** — Fork-limit enforcement (`fork_limit`/`fork_count` per process); `sys_fork` rejects when limit is exceeded.
- ✅ **Syscall filtering** — `uint64_t syscall_mask[4]` (256 bits) per process. `syscall_handler()` checks mask before dispatch. `sys_set_ssf`(64) can only drop bits; mask reset on exec. Userspace `set_syscall_filter()`.
- ✅ **Privilege separation** — `capset` drops capabilities permanently. `prctl` with `PR_SET_NO_NEW_PRIVS` (syscall 66) prevents setuid on exec. `no_new_privs` inherited on fork, preserved across exec.
- ✅ **Secure IPC** — `socketpair(AF_UNIX, SOCK_STREAM, 0, sv)` creates a pair of connected stream sockets with 4KB ring buffers per direction. `SO_PEERCRED` returns `ucred_t` with peer uid/gid/pid. Userspace `socketpair()` wrapper (syscall 65).

### Auditing

- ✅ **Audit logging** — Fixed-size ring buffer (256 entries) with `audit_log()` at capability denials, fork limit violations, sensitive syscalls (reboot/poweroff), and process exec/exit. `sys_audit_read` exposes entries to userspace.

### Security Hardening

- 🟡 **ASLR** — PIE binaries load at random base (0x40000000-0x60000000 using RDTSC); stack base randomized. Fixed-address EXEC binaries use deterministic addresses.
- ✅ **Secure boot** — Build-time SHA-256 hash whitelist of all embedded ELF binaries (10 known hashes). On exec, binary content is hashed and checked against whitelist; unknown binaries rejected with ERR_PERM. Default enabled; can be disabled via syscall. `secure_boot` syscall (67) for enable/disable/query.
- ✅ **setuid on exec** — `sys_execve` stats the ELF and sets `proc->euid` to file owner if `S_ISUID` is set (unless `no_new_privs` is active).

### Existing Security

- ✅ **Kernel/user isolation** — CPL3 user-mode cannot access CPL0 kernel pages; SMAP/SMEP enabled where the CPU supports them.
- ✅ **Read-only kernel text** — Kernel `.text` section mapped with write-protect bit; accidental overwrites of code produce a page-fault panic.
- ✅ **NX memory** — All data pages (heap, stack, user data) marked NX; execution from data regions triggers a protection fault.
- ✅ **Basic signals** — SIGKILL and SIGSEGV are delivered correctly; broader signal infrastructure is still under construction.

### Cryptography

- ✅ **SHA-256** — `sha256.h`/`sha256.c` with `sha256_init/update/final/sha256`. Verified against NIST FIPS 180-4 vectors (empty string and "abc").
- ✅ **CSPRNG** — `random.h`/`random.c`: SHA-256 in counter mode, seeded from RDTSC + timer jitter, periodic re-seeding. `sys_getrandom`(63) copies directly to user buffer.
- ✅ **Kernel crypto primitives** — SHA-256 as fundamental building block; used for PRNG.

### Exit Criteria

- [x] Unprivileged process cannot read kernel memory (SMAP/SMEP, CPL3 vs CPL0 enforced)
- [x] Capability check blocks an unauthorized syscall
- [x] Audit log records a privilege event
- [x] ASLR produces different load addresses across runs (PIE binaries)
- [x] Sandboxed process cannot fork beyond its policy
- [x] DAC permission check blocks non-root access (mode=0000 file)
- [x] Syscall filter blocks a blocked syscall
- [x] SHA-256 produces correct known digest
- [x] getrandom returns non-deterministic data
- [x] socketpair passes data round-trip and peer credentials
- [x] prctl(PR_SET_NO_NEW_PRIVS) disables setuid on exec
- [x] Secure boot rejects non-whitelisted binary at exec
- [x] Build-time hash generation covers all embedded ELF binaries

### New files
- `os/src/include/security.h` — Capability constants, audit event types, `audit_entry_t` struct
- `os/src/kernel/audit.c` — Audit ring buffer, `audit_log()`, `audit_read_next()`, `cap_check()`
- `os/src/kernel/sha256.h` / `os/src/kernel/sha256.c` — SHA-256 implementation
- `os/src/kernel/random.h` / `os/src/kernel/random.c` — CSPRNG using SHA-256 counter mode
- `os/src/kernel/unix.h` / `os/src/kernel/unix.c` — AF_UNIX socketpair + ring buffers
- `os/src/kernel/secure_boot.h` / `os/src/kernel/secure_boot.c` — Secure boot whitelist verification
- `os/scripts/gen_secure_boot_hashes.py` — Build-time hash generation for embedded ELFs

### Modified files
- `os/src/include/process.h` — Added `caps`, `fork_count`, `fork_limit`, `uid`, `gid`, `euid`, `egid`, `syscall_mask[4]`, `no_new_privs` fields
- `os/src/include/types.h` — `uid_t`, `gid_t` typedefs
- `os/src/kernel/process.c` — UID/GID/cap/fork-limit init in `process_create`; fork_count on exit; no_new_privs inheritance; audit logging
- `os/src/kernel/vfs.h` — `vfs_access_check` declaration; `vfs_stat_t` uid/gid fields; `S_IRWXU` etc.
- `os/src/kernel/vfs.c` — `vfs_access_check()`, DAC in `vfs_open`, uid/gid in stat
- `os/src/kernel/sfs.c` / `tmpfs.c` / `devfs.c` — uid/gid in stat calls
- `os/src/kernel/syscall.c` — 11 new syscalls (57-66); syscall filtering; setuid on exec with no_new_privs check; kernel_err_to_posix() return; socketpair + prctl handlers
- `os/src/include/syscall_defs.h` — SYS_GETUID..SYS_PRCTL (57-66); `SYSCALL_COUNT=67`
- `os/src/kernel/main.c` — `random_init()` after `cap_init()`
- `os/src/kernel/net.h` — SO_PEERCRED, AF_UNIX constants
- `os/src/kernel/net.c` — unix_init() in net_init(); AF_UNIX guard in socket_alloc
- `os/src/include/unistd.h` — uid/gid getter decls, getrandom, set_syscall_filter, socketpair, prctl, ucred_t
- `os/src/lib/libuser/unistd.c` — Userspace wrappers for all 11 new syscalls
- `os/src/include/test_framework.h` — ASSERT_FALSE, ASSERT_NE macros
- `os/src/kernel/security_test.c` — 18 tests total: cap, fork_limit, audit, uid/gid, DAC, syscall_filter, sha256, getrandom, socketpair, no_new_privs, secure_boot
- `os/src/kernel/process.c` — secure_boot_check in process_exec
- `os/src/kernel/main.c` — secure_boot_init call
- `os/Makefile` — secure boot hash generation target; include path update
- `os/src/include/syscall_defs.h` — SYS_SECURE_BOOT(67); SYSCALL_COUNT=68
- `os/src/kernel/syscall.c` — sys_secure_boot handler; secure_boot.h include

---

## STAGE 7 — SMP and Parallel Processing `~100%`

The symmetric multiprocessing (SMP) layer enables the kernel to utilise all
available CPU cores. It is built in ten phases, each depending on the
previous. The entire stage is gated on KVM or bare-metal — QEMU TCG does not
support SMP for this kernel (softmmu page-walk cache bug breaks APIC MMIO).

---

### Phase 7.1 — CPU Discovery & APIC Infrastructure

#### 7.1.1 ACPI MADT Parsing

- Scan the RSDP → XSDT → MADT (Multiple APIC Description Table) chain.
- Extract:
  - Processor Local APIC entries: APIC ID, flags (enabled/disabled), processor UID.
  - I/O APIC entries: APIC ID, address, global system interrupt base.
  - Interrupt Source Override entries (for legacy IRQ→GSI mapping).
- Store results in a `cpu_info_t` array (max 64 entries, statically allocated).
- Each entry: `apic_id`, `processor_uid`, `flags`, `stack_base` (allocated later).
- **Fallback:** If MADT is absent, assume single CPU (BSP only).
- Files: `os/src/kernel/acpi.c` / `acpi.h` (partial: RSDP/XSDT parsing exists,
  extend with MADT). `os/src/kernel/smp.c` / `smp.h` (new).

#### 7.1.2 Enabling xAPIC / x2APIC

- **Detect APIC version:** CPUID leaf 1, EDX bit 9 (APIC).
  - If x2APIC is available (CPUID leaf 1, ECX bit 21), prefer it.
- **Enable the local APIC:**
  - xAPIC: write `IA32_APIC_BASE_MSR (0x1B)` with bit 11 set.
    - Map APIC MMIO at `0xFEE00000` (physical) → kernel virtual address.
  - x2APIC: set bit 10 of `IA32_APIC_BASE_MSR` as well.
    - No MMIO needed; all APIC registers accessed via `IA32_X2APIC_*` MSRs.
- **Validate:** Read APIC ID register; it must match the MADT entry for BSP.
- Set `apic_mode` global: `APIC_MODE_XAPIC` or `APIC_MODE_X2APIC`.
- Files: `os/src/kernel/apic.c` / `apic.h` (extend existing stubs).

#### 7.1.3 Local APIC Timer

- **Program LAPIC timer:**
  - Divide configuration register (`0x3E` on xAPIC, `0x83E` on x2APIC):
    divide by 16.
  - Initial-count register (`0x38` / `0x838`): set to `cpu_mhz * 1000000 / 16 /
    HZ` to get ~1000 Hz tick.
  - LVT timer register (`0x32` / `0x832`):
    - Vector = 0x40 (IRQ0 replacement).
    - Delivery = Fixed.
    - Masked = 0.
- **Calibrate:** Use PIT or HPET to measure the time for a known LAPIC count,
  then derive `cpu_mhz`. Store as `cpu_khz[apic_id]` (per-CPU, for scheduler
  accounting).
- **APIC timer ISR:** Same handler as the PIT timer ISR (`sched_timer_tick`).
- **Vector reservation:** `0x20–0x2F` (PIC), `0x30–0x3F` (APIC + IPIs).
  - `0x30`: LAPIC timer.
  - `0x31`: IPI_RESCHEDULE.
  - `0x32`: IPI_TLB_SHOOTDOWN.
  - `0x33`: IPI_PANIC.
  - `0x34–0x3F`: Spare.
- **KVM detection:** Set LAPIC timer to one-shot mode; on KVM the LAPIC timer
  is emulated by the host and works with simple MSR writes.
- **QEMU TCG:** LAPIC timer stays disabled (use legacy PIT).

#### 7.1.4 I/O APIC

- **Map I/O APIC MMIO:** Base address from MADT I/O APIC entry (typically
  `0xFEC00000`).
- **Program I/O APIC redirection entries:**
  - For each ISA IRQ, set the corresponding I/O APIC redirection entry:
    - Vector = 0x20 + IRQ (same as PIC remap).
    - Delivery = Fixed.
    - Polarity = Active-high (or follow Interrupt Source Override).
    - Trigger = Edge (or Level for PCI IRQs).
  - Mask all entries initially; unmask as drivers register.
- **Disable legacy PIC:** After I/O APIC is set up, mask all PIC IRQs.
- Files: `os/src/kernel/apic.c` / `apic.h`.

---

### Phase 7.2 — Atomic Operations & Memory Barriers

**Goal:** A portable kernel API for lock-free concurrency on x86-64.

#### 7.2.1 atomic_t

- **Type:**
  ```c
  typedef struct { volatile int64_t counter; } atomic_t;
  typedef struct { volatile uint64_t counter; } atomic64_t;
  ```
- **Operations** (all use `lock`-prefixed x86 instructions):
  - `atomic_read(v)` — plain load (no barrier).
  - `atomic_set(v, i)` — plain store.
  - `atomic_add(i, v)` / `atomic_sub(i, v)` — `lock xadd`.
  - `atomic_inc(v)` / `atomic_dec(v)` — `lock inc` / `lock dec`.
  - `atomic_add_return(i, v)` — `lock xadd` + read result.
  - `atomic_sub_and_test(i, v)` — subtract and test zero (for refcounts).
  - `atomic_cmpxchg(v, old, new)` — `lock cmpxchg`.
  - `atomic_xchg(v, new)` — `lock xchg`.
  - `atomic_test_and_set_bit(nr, addr)` — `lock bts`.
  - `atomic_clear_bit(nr, addr)` — `lock btr`.
- **Implementation:** Inline assembly macros in a new `os/src/include/atomic.h`.
  All operations carry a `memory` clobber (compiler barrier). No explicit
  `MFENCE` is required on x86 because `lock`-prefixed instructions are
  already full memory barriers.

#### 7.2.2 Memory Barriers

- **Macros:**
  - `mb()` — full memory barrier (`mfence`).
  - `rmb()` — read barrier (`lfence`).
  - `wmb()` — write barrier (`sfence`).
  - `smp_mb()` / `smp_rmb()` / `smp_wmb()` — barrier only on SMP builds
    (compile to `mfence`/`lfence`/`sfence` on SMP, empty on UP).
- **Use in spinlocks:** Existing `spinlock_acquire` uses `cli`/`sti` for UP
  mutual exclusion. For SMP, replace with `lock bts` (test-and-set) + `mfence`.
- **Placement:** `os/src/include/barrier.h`.

#### 7.2.3 Spinlock Rewrite for SMP

- **Current:** `spinlock_acquire` disables interrupts (`cli`) — works on UP
  because no other core can touch the data. **Broken on SMP.**
- **New implementation:**
  ```c
  typedef struct {
      volatile int locked; // 0 = free, 1 = held
  } spinlock_t;

  static inline void spinlock_init(spinlock_t* l) { l->locked = 0; }

  static inline void spinlock_acquire(spinlock_t* l) {
      while (__atomic_test_and_set(&l->locked, __ATOMIC_ACQUIRE))
          while (l->locked) cpu_relax(); /* pause instruction */
  }

  static inline void spinlock_release(spinlock_t* l) {
      __atomic_clear(&l->locked, __ATOMIC_RELEASE);
  }
  ```
- `cpu_relax()` → `rep; nop` (PAUSE instruction). Reduces power consumption
  and improves performance on hyperthreaded CPUs.
- Track holding CPU: `spinlock_acquire` stores `cpu_id()` in a debug field
  for lockdep/auditing.
- **Backward compatibility:** For UP builds (SMP=n), keep the old `cli`/`sti`
  approach (compiles to nothing). New `spinlock_t` definition must be
  compatible with existing code.

---

### Phase 7.3 — Per-CPU Infrastructure

**Goal:** Each CPU has its own data structures accessed without atomic
overhead.

#### 7.3.1 CPU Identification

- **`cpu_id()`:** Reads the APIC ID via `IA32_X2APIC_APICID` MSR (or MMIO for
  xAPIC). Returns a dense index (0, 1, 2, ...).
  - Lookup table `apic_id_to_cpu[256]` maps APIC IDs → dense CPU indices.
- **`nr_cpus`:** Number of logical CPUs discovered by MADT.
- **`cpu_present(cpu_id)`:** Bitmap of present CPUs.

#### 7.3.2 Per-CPU Data Region

- **Memory layout:** A contiguous virtual region `PER_CPU_START` →
  `PER_CPU_END`. For each CPU, a copy of the `per_cpu_data` struct.
  - Size: `sizeof(per_cpu_data_t) * nr_cpus`, allocated via PMM at boot.
  - Each copy is aligned to a cache line (64 bytes) to prevent false sharing.
- **Access macros:**
  - `DEFINE_PER_CPU(type, name)` — creates a per-CPU variable in a special
    `.data..percpu` ELF section.
  - `this_cpu_ptr(name)` — returns the CPU-local copy: `__per_cpu_offset[cpu_id()] + &name`.
  - `per_cpu(name, cpu)` — returns the copy for a specific CPU.
- **Segment register approach (alternative):** Use `GS.base` to point to the
  per-CPU area. Access via `%gs:offset`. Requires swapping `GS.base` on every
  context switch that migrates between CPUs.
  - Simpler: just use `__per_cpu_offset[cpu] + base` in C. No extra context
    switch overhead.
- **`per_cpu_data_t` struct:**
  ```c
  typedef struct {
      thread_t*    current_thread;      /* running thread on this CPU */
      run_queue_t  run_queue;           /* local run queue */
      spinlock_t   rq_lock;             /* per-CPU queue lock */
      uint64_t    cpu_khz;              /* calibrated frequency */
      uint64_t    irq_count;            /* total interrupts handled */
      uint64_t    tlb_shootdown_count;
      char         pad[0];              /* zero-length marker for end */
  } __attribute__((aligned(64))) per_cpu_data_t;
  ```
- Files: `os/src/include/percpu.h`.

#### 7.3.3 Converting Global State to Per-CPU

- **Scheduler:** `current_thread` becomes per-CPU. `run_queue` becomes per-CPU.
  - Remove global `run_queue_t ready_queue` in sched.c.
  - Replace with `this_cpu_ptr()->run_queue`.
- **Memory allocator:** PMM free-page list becomes per-CPU (at minimum, the
  magazine caches in the slab allocator become per-CPU).
- **Statistics:** IRQ counts, scheduler switches, context-switch counts.
- Files: `os/src/kernel/sched.c` (refactor), `os/src/kernel/pmm.c` (refactor).

---

### Phase 7.4 — Secondary CPU Boot

**Goal:** Wake up application processors (APs) from their reset state and
bring them into the kernel's 64-bit execution environment.

#### 7.4.1 AP Trampoline (16-bit → 64-bit)

- **Location:** A 4 KiB page below 1 MiB (e.g. at physical `0x8000`), copied
  from the kernel image at link time. Must be position-independent 16-bit
  real-mode code.
- **Sequence:**
  1. AP starts at `0xFFFFFFF0` (reset vector), jumps to BIOS, BIOS redirects
     to the SIPI vector (`0x8000`).
  2. **16-bit real mode:** Set up a minimal GDT with 32-bit code segment.
     Load CS via far jump. Enable A20 gate.
  3. **32-bit protected mode:** Set up page tables (identity-map first 4 MiB).
     Enable PAE and long mode. Load 64-bit GDT.
  4. **64-bit long mode:** Load kernel stack pointer (from per-AP stack
     allocated earlier). Load `cr3` (kernel page tables). Set up `GS.base`
     for per-CPU data. Jump to C entry: `ap_startup()`.
- **Trampoline code:** `os/src/kernel/trampoline.S` (NASM). Assembled into a
  `.boot` section that is placed at a known address and copied to `0x8000`.

#### 7.4.2 AP Bootstrap from BSP

- **BSP allocates resources for each AP:**
  - Kernel stack: 4 pages (16 KiB) from PMM. Virtual address stored in
    `cpu_info_t[cpu].stack_base`.
  - Per-CPU data page: from PMM. Zeroed.
  - IDT: each AP gets a copy of the BSP IDT (same handlers).
- **Sending INIT-SIPI:**
  1. Write `ICR` (Interrupt Command Register) with `INIT` (vector 0x500) to
     the target AP's APIC ID.
  2. Wait 10 ms.
  3. Write `ICR` with `SIPI` (vector = `0x8000 >> 12` = 0x08) to the same
     target.
  4. Wait 200 µs.
  5. Optionally send a second SIPI (some CPUs require it).
- **AP startup IPI handler:** In `apic.c`, `send_ipi(cpu_id, vector)` function.
  - Write APIC ICR: destination field = target APIC ID, shorthand = no shorthand,
    delivery mode = INIT/SIPI/STARTUP/FIXED.

#### 7.4.3 AP Initialisation (C Entry)

- `ap_startup(uint64_t cpu_id)`:
  1. Set `this_cpu_ptr()->current_thread = idle_thread_cpu[cpu]`.
  2. Calibrate LAPIC timer (measure against HPET/PIT).
  3. Program LAPIC timer to fire every 1 ms.
  4. Set GS.base to point to this CPU's per-CPU data.
  5. Stamp `cpu_present[cpu_id] = 1`.
  6. Enable interrupts.
  7. Enter `ap_idle_loop()`:
     - Spin on the CPU's run queue.
     - If the idle queue is non-empty, dequeue and run.
     - Otherwise, try to steal work from other CPUs.
     - Otherwise, HLT until IPI arrives.

#### 7.4.4 Synchronisation

- **BSP waits for APs:** Global atomic counter `ap_ready_count`. Each AP
  increments it after reaching `ap_idle_loop`. BSP polls until
  `ap_ready_count == nr_cpus`.
- **`sched_ap_start()`:** Called after all APs are ready. Unparks threads
  on AP run queues.

---

### Phase 7.5 — Inter-Processor Interrupts (IPI)

**Goal:** Cores signal each other for scheduling, TLB, and diagnostics.

#### 7.5.1 IPI Delivery

- `send_ipi(uint32_t apic_id, uint8_t vector, uint32_t delivery_mode)`:
  - xAPIC: write ICR low (`0x300`) with vector + delivery mode, then ICR
    high (`0x310`) with destination APIC ID. Poll for delivery status.
  - x2APIC: write `IA32_X2APIC_ICR` MSR (`0x830`) with both destination
    and vector in the same 64-bit write.
- `send_ipi_allbutself(uint8_t vector)` — broadcast to all other CPUs.
- `send_ipi_self(uint8_t vector)` — for testing.

#### 7.5.2 IPI Types and Vectors

| Vector | Name | Handler |
|--------|------|---------|
| `0x31` | `IPI_RESCHEDULE` | Set `need_reschedule` on target CPU; target checks on IRET. |
| `0x32` | `IPI_TLB_SHOOTDOWN` | Call `tlb_shootdown_handler()` — invalidate local TLB entries. |
| `0x33` | `IPI_PANIC` | Halt the target CPU immediately for debugging. |

#### 7.5.3 IPI Handlers

- **RESCHEDULE:** Write the target CPU's `need_reschedule` flag
  (`this_cpu_ptr(cpu)->need_reschedule = 1`). The target CPU checks this
  flag on every interrupt return and calls `schedule()` if set.
- **TLB_SHOOTDOWN:** The handler reads a per-CPU TLB flush queue
  (list of <start, end> ranges to invalidate). For each range, issue
  `INVLPG` for every page or write to `cr3` to flush the entire TLB.
- **PANIC:** `cli; hlt` loop.

---

### Phase 7.6 — SMP Scheduler

**Goal:** Threads can run on any CPU; load is balanced across cores.

#### 7.6.1 Per-CPU Run Queues

- Replace the global `ready_queue` with per-CPU `run_queue_t`.
- Each queue has its own `rq_lock` (per-CPU spinlock).
- `sched_add_thread(t)`:
  - Place `t` on the run queue of the CPU specified by `t->cpu_affinity`
    (default: round-robin across available CPUs, or the CPU where the
    parent thread ran).
- `schedule()`:
  - Called on each CPU independently.
  - Pick the next thread from `this_cpu_ptr()->run_queue`.
  - If the queue is empty, try work-stealing (see below).
  - If still empty, run the CPU's idle thread.

#### 7.6.2 CPU Affinity

- **`sched_setaffinity(pid_t pid, uint64_t mask)` syscall:**
  - `mask` is a bitmask of allowed CPUs (bit 0 = CPU 0, etc.).
  - Threads with restricted affinity are never migrated outside their mask.
  - Syscall number: 107.
- **Inheritance:** `fork` inherits parent's affinity. `exec` resets to all
  CPUs.
- **Default:** All threads can run on any CPU (`mask = (1 << nr_cpus) - 1`).

#### 7.6.3 Cross-CPU Wakeup

- When `sched_wake(wq)` is called and the first waiter was last scheduled
  on CPU N:
  - If running on CPU N: add to local queue.
  - If running on a different CPU: send `IPI_RESCHEDULE` to CPU N after
    adding to that CPU's queue using a per-CPU spinlock.
- Wakees added to the target CPU's `priority`-ordered ready queue.

#### 7.6.4 Load Balancing

- **Periodic load balancer:** Fires every 100 ticks on each CPU.
  - Compare `this_cpu_queue->count` with `avg = total_threads / nr_cpus`.
  - If `count > avg + 2` (overloaded), move up to `count - avg` threads
    to underloaded siblings.
  - Only migrates threads with affinity masks that include the target CPU.
- **Idle pull (work stealing):**
  - When a CPU's queue is empty and `schedule()` is called:
    - Scan siblings in random order.
    - If a sibling has `count >= 2`, steal one thread.
    - Send `IPI_RESCHEDULE` to the sibling only if it was also idle
      (wakes the sibling to rebalance properly).

#### 7.6.5 Scheduler Statistics

- Per-CPU counters: `context_switches`, `migrations`, `idle_ticks`, `busy_ticks`.
- Exposed via `/proc/stat` (requires Phase 8.2 but gather now).

---

### Phase 7.7 — TLB Shootdown

**Goal:** When one CPU modifies a page table, all other CPUs invalidate
cached translations within bounded time.

#### 7.7.1 Shootdown Protocol

- `tlb_shootdown_range(uint64_t start, uint64_t end)`:
  1. Fill per-CPU flush queue on the current CPU with the range.
  2. Send `IPI_TLB_SHOOTDOWN` to all other CPUs.
  3. On each target CPU, the IPI handler:
     - Reads its flush queue.
     - For each entry: `INVLPG` for every 4 KiB-aligned address in range
       (or if end - start > 32 pages, flush entire TLB via `mov cr3, val`).
  4. The initiating CPU waits for all target CPUs to acknowledge
     (via atomic counter `tlb_flush_ack[nr_cpus]`).
  5. Clear the flush queue on the initiating CPU.

#### 7.7.2 Integration with VMM

- `vmm_unmap_page`, `vmm_map_page`, `vmm_free_user_pages` call
  `tlb_shootdown_range` if SMP is active.
- `vmm_duplicate_user_pages` (fork) does not need shootdown (new pages
  are visible only to the child, which is not yet scheduled on another CPU).
- `munmap`, `mprotect`, `mmap(MAP_FIXED)` all trigger shootdown.

#### 7.7.3 Optimisation

- **Batching:** Multiple unmap operations during `munmap` of a large range
  are coalesced into a single IPI.
- **Local-only:** If the modified page is not mapped in any other CPU's
  address space (tracked per-page in the VMA), skip the IPI.
- **Lazy (future):** Instead of IPI, defer TLB invalidation until the
  target CPU performs a context switch. More complex but avoids IPI storms.

---

### Phase 7.8 — SMP-Safe Memory Allocators

**Goal:** The PMM and slab allocator support concurrent access from any CPU.

#### 7.8.1 PMM — Atomic Bitmap

- **Current:** Bitmap protected by a single spinlock; `pmm_alloc_pages` and
  `pmm_free_pages` lock the entire allocator.
- **SMP-safe version:**
  - Use `atomic_test_and_set_bit` and `atomic_clear_bit` for bitmap
    operations. Removes the need for a global lock on the hot path.
  - Per-CPU free-page lists: each CPU keeps a small cache of free pages
    (like the slab magazine). Pages freed on CPU X are added to CPU X's
    list. If CPU X runs out, it steals from a sibling's list.
  - Fallback: if per-CPU lists are empty, use the atomic bitmap as before.

#### 7.8.2 Slab Allocator — Per-CPU Magazines

- **Current:** Magazine caches exist but assume single-threaded access.
- **SMP-safe version:**
  - Each CPU has its own magazine (lock-free, size 8–16 objects).
  - `kmalloc` from the local magazine; if empty, refill from the shared
    slab with the slab lock held.
  - `kfree` to the local magazine; if full, flush half to the shared slab.
  - The shared slab (partial/full/empty lists) is protected by a spinlock
    that is only taken when the magazine is empty or full.
- **Result:** In the common case, allocation and freeing are local to the
  CPU with no lock contention.

#### 7.8.3 Lock-Free Memory Pools

- **MPSC (multi-producer, single-consumer) queue** for cross-CPU message
  passing. Used by IPI handlers to enqueue TLB flush requests.
- Implemented using `atomic_cmpxchg` on linked-list head pointer.
- Files: `os/src/include/lfq.h`, `os/src/kernel/lfq.c`.

---

### Phase 7.9 — SMP Synchronization Primitives

**Goal:** Beyond basic spinlocks, provide higher-level primitives suitable
for SMP.

#### 7.9.1 Reader/Writer Locks

- `rwlock_t` with:
  - `rwlock_init`, `rwlock_read_lock`, `rwlock_read_unlock`,
    `rwlock_write_lock`, `rwlock_write_unlock`.
  - Multiple concurrent readers; exclusive writer.
  - Writer starvation prevention: if a writer is waiting, new readers
    are blocked until the writer finishes.
- Implemented with an atomic counter (positive = read count, -1 = write held).

#### 7.9.2 Seqlocks

- `seqlock_t` for read-mostly data (e.g., `jiffies`, clock time).
- Writers increment the sequence counter, write data, increment again.
- Readers read the sequence counter before and after; if odd or changed,
  retry. No memory barrier needed on x86 (stores are ordered).
- **Target:** Replace the raw spinlock in `hal_timer_get_ns()` with a seqlock
  so that readers never block the clock interrupt handler.

#### 7.9.3 Lock Dependency Tracking (Lockdep)

- **Goal:** Detect potential deadlocks at boot by tracking lock acquisition
  order.
- Each lock has a `lock_class_key` (16-bit ID, auto-assigned).
- On `spinlock_acquire`:
  - Record the lock class in a per-CPU stack of held lock classes.
  - Check that no held lock is ordered after the new lock in the global
    lock-order graph (which would form a cycle).
- If a cycle is detected: `kprintf("LOCKDEP: possible deadlock: ...")`
  with a stack trace.
- File: `os/src/kernel/lockdep.c` / `lockdep.h`. Disabled in release builds.

#### 7.9.4 Contention Statistics

- Each spinlock tracks:
  - `acquire_count` — total number of `spinlock_acquire` calls.
  - `contention_count` — number of times the `while (locked)` loop spun.
  - `max_wait_ticks` — longest wait so far.
- Exposed via `/proc/lock_stat` (read-only text file).

---

### Phase 7.10 — SMP Validation & Stress Testing

#### 7.10.1 Self-Tests

- **Concurrent spinlock test:** N threads on N cores each spinlock-acquire
  a shared lock, increment, release. Verify the final count.
- **Concurrent PMM test:** Each CPU allocates and frees 100 pages in a loop.
  Verify no double-free or corruption via the slab poison checker.
- **IPI round-trip test:** Measure latency of `send_ipi_self`.
- **Affinity test:** Pin a thread to CPU 0, verify it never runs on CPU 1.
- **Parallel fork bomb:** Fork 200 processes across 4 CPUs, verify no crash.

#### 7.10.2 Stress Tests

- **Allocator storm:** 4 threads on 4 CPUs simultaneously allocate/free
  random sizes for 10 seconds. Verify no memory leaks (total free == initial).
- **Scheduler storm:** 16 CPU-bound threads with varying priorities
  on 4 CPUs. Verify no thread is starved for more than 100 ms.
- **Filesystem storm:** 4 threads concurrently creating/deleting files
  on a tmpfs mount. Verify no VFS corruption.

#### 7.10.3 Race Detection Infrastructure

- **KCSAN-light:** At compile time, annotate shared variables with
  `__shared` or similar. At runtime, the kernel records accesses and
  detects concurrent conflicting accesses.
- This is extremely invasive — deferred to a future optimisation pass.
  For now, rely on careful code review and stress testing.

---

### Exit Criteria

- [x] **7.1:** MADT parsed; APICs enabled; LAPIC timer fires at 1000 Hz.
- [x] **7.2:** `atomic_t` API complete; spinlocks use `lock cmpxchg` on SMP.
- [x] **7.3:** Per-CPU run queues and `this_cpu_ptr()` work.
- [x] **7.4:** At least one secondary CPU boots to `ap_idle_loop`.
- [x] **7.5:** `IPI_RESCHEDULE` forces a reschedule on a remote CPU.
- [x] **7.6:** Threads run on any CPU; load balancer moves threads between CPUs.
- [x] **7.7:** `tlb_shootdown_range` invalidates TLB on all CPUs.
- [x] **7.8:** Concurrent `kmalloc`/`kfree` and `pmm_alloc_pages`/`pmm_free_pages`
  survive 10 simultaneous threads without corruption.
- [x] **7.9:** Seqlock protects `hal_timer_get_ns()`; rwlock used in at least
  one filesystem path.
- [x] **7.10:** SMP self-tests pass; allocator storm test passes.
- [x] **NUMA:** SRAT/SLIT parsing, per-node PMM free lists, node-local kmalloc/VMM/sched allocations, expanded test_numa_basic().
- [ ] Kernel is stable under 4-CPU stress test for 5 minutes (validation, not implementation).
- [ ] All `cli`/`sti`-based spinlocks converted to `lock cmpxchg` spinlocks (deferred to optimization pass).

---

## STAGE 8 — Service Layer `0%`

The service layer is the **userspace runtime infrastructure** that turns a kernel
into a usable operating system. It is built in five phases, each depending on
the previous.

---

### Phase 8.1 — Inter-Process Communication (IPC)

The foundation: every higher-level service depends on processes being able to
exchange data efficiently.

#### 8.1.1 Shared Memory (System V + POSIX)

**Goal:** Two or more processes share a physical page range via their page tables.

- `shmget(key_t key, size_t size, int shmflg)` — create or find a shared memory
  segment identified by a numeric key. Returns a `shmid`.
  - Key **IPC_PRIVATE (0)** creates an anonymous segment not linked to any key.
  - `size` rounded up to page boundary; maximum 32 segments, each up to 64 MiB.
  - `SHM_R | SHM_W` permission bits checked at attach time.
  - Internal: `shm_seg_t` object (refcounted, tracked in kernel-global table).
  - Kernel allocates pages via PMM on creation (not on attach — real SysV
    allocates on attach, but pre-allocation is simpler and safe).
- `shmat(int shmid, const void* shmaddr, int shmflg)` — attach segment into
  the calling process's address space at `shmaddr` (or kernel-chosen if NULL).
  - `SHM_RDONLY` flag for read-only attachment.
  - Wired as VMA entries with `VM_SHARED` flag.
  - Page-table entries point to the segment's physical pages; no copy-on-write.
- `shmdt(const void* shmaddr)` — detach; decrements attach count.
  - Physically unmaps the VMA range in the calling process.
  - Last detach (`nattch == 0`) may be delayed until `shmctl(IPC_RMID)`.
- `shmctl(int shmid, int cmd, struct shmid_ds* buf)`:
  - `IPC_STAT`: copy segment metadata to userspace.
  - `IPC_SET`: update `uid`, `gid`, `mode`.
  - `IPC_RMID`: mark segment for deletion; actual free deferred until last detach.
  - `IPC_INFO`: return system limits.
- **Wrappers:** `shm.h` / `shm.c` (~400 lines). Syscall numbers 89–92.
- **shm_open/shm_unlink (POSIX):**
  - `shm_open(const char* name, int oflag, mode_t mode)` — create/open a
    shared-memory object under `/dev/shm/<name>`. Returns a file descriptor.
    - Implemented via a `tmpfs` mount at `/dev/shm`.
    - `O_CREAT | O_EXCL | O_RDWR` flags behave like `open()`.
  - `shm_unlink(const char* name)` — remove the object.
  - On the fd, `mmap(MAP_SHARED)` maps the object's pages into the process.
  - Syscall: shm_open maps to `open("/dev/shm/name", ...)`, shm_unlink maps
    to `unlink("/dev/shm/name")`. Can be entirely userspace if `/dev/shm`
    exists as a tmpfs mount.
- **mmap MAP_SHARED:**
  - `vma_t` flag `VM_SHARED` already exists from file-backed mmap.
  - For anonymous `MAP_SHARED | MAP_ANONYMOUS`: allocate physical pages,
    create VMA with `VM_SHARED`. Fork copies the VMA but shares physical pages.
  - For file-backed `MAP_SHARED`: write-back on munmap already implemented.
- **Files:** `os/src/kernel/shm.c`, `os/src/kernel/shm.h`, `os/src/include/sys/shm.h`.
- **Verify:** Two processes `shmget`/`shmat`, write in one, read in the other;
  `shmdt` cleanly unmaps; `IPC_RMID` + last detach frees pages.

#### 8.1.2 POSIX Message Queues

**Goal:** Processes send/receive tagged messages via named queues.

- `mq_open(const char* name, int oflag, mode_t mode, struct mq_attr* attr)` —
  create or open a named message queue.
  - Name is a path under `/dev/mqueue/<name>`.
  - `attr` specifies `mq_maxmsg` (max 128) and `mq_msgsize` (max 4096).
  - Internal: `mq_t` with a fixed-size ring buffer (pre-allocated message slots).
  - Permissions checked via mode bits.
- `mq_close(mqd_t mqdes)` — close the descriptor; last close frees the queue.
- `mq_unlink(const char* name)` — remove the queue object from the namespace.
- `mq_send(mqd_t mqdes, const char* msg_ptr, size_t msg_len, unsigned msg_prio)` —
  enqueue a message. Priority 0–31 (higher = first). Blocks if full
  (`O_NONBLOCK` returns `EAGAIN`).
- `mq_receive(mqd_t mqdes, char* msg_ptr, size_t msg_len, unsigned* msg_prio)` —
  dequeue highest-priority message. Blocks if empty.
- `mq_notify(mqd_t mqdes, const struct sigevent* notification)` —
  register for async notification when a message arrives (signal or thread).
- `mq_getattr` / `mq_setattr` — query/tweak queue attributes.
- **Internal:** `mq.c` / `mq.h`. Kernel manages a hash table of active queues.
  Each queue has a spinlock, a wait queue of readers, and a wait queue of writers.
- **Syscall numbers:** 93–100.
- **Files:** `os/src/kernel/mq.c`, `os/src/kernel/mq.h`, `os/src/include/mqueue.h`.
- **Verify:** Two processes exchange messages; priority ordering; blocking
  send/receive with timeout; `mq_notify` delivers SIGUSR1.

#### 8.1.3 Eventfd, Signalfd, Timerfd

**Goal:** File-descriptor-based event notification for integration with `poll`/`epoll`.

- **eventfd** (`sys/eventfd.h`, syscall 101):
  - `eventfd(unsigned int initval, int flags)` — returns a new fd that acts
    as a 64-bit counter. `EFD_SEMAPHORE` flag enables semaphore behaviour.
  - `read(fd, &val, 8)` — reads the counter; blocks if 0 (or non-blocking
    returns `EAGAIN`).
  - `write(fd, &val, 8)` — adds `val` to the counter.
  - Poll: `POLLIN` when counter > 0; `POLLOUT` always.
  - Internal: `eventfd_ctx_t` in kernel (counter + wait queue).
- **signalfd** (`sys/signalfd.h`, syscall 102):
  - `signalfd(int fd, const sigset_t* mask, int flags)` — creates/updates an
    fd that delivers pending signals matching `mask`.
  - `read(fd, &siginfo, sizeof(siginfo))` — reads one dequeued signal.
  - Poll: `POLLIN` when a matching signal is pending.
  - Internal: hooks into `signal_process()` to enqueue on signalfd's list
    instead of (or in addition to) the process's pending signal set.
- **timerfd** (`sys/timerfd.h`, syscall 103):
  - `timerfd_create(int clockid, int flags)` — creates an fd that fires at
    specified intervals. `TFD_NONBLOCK` flag.
  - `timerfd_settime(int fd, int flags, const struct itimerspec* new_value,
    struct itimerspec* old_value)` — arms the timer.
  - `read` returns the number of expirations since last read (uint64_t).
  - Internal: uses kernel timer infrastructure (HPET/PIT) to schedule
    wakeups. On each expiry, increments the fd's counter and wakes readers.
- **Files:** `os/src/kernel/eventfd.c`/`.h`, `os/src/kernel/signalfd.c`/`.h`,
  `os/src/kernel/timerfd.c`/`.h`.
- **Verify:** `eventfd` ping-pong across two threads; `signalfd` catches
  SIGUSR1; `timerfd` fires at 10 ms intervals.

#### 8.1.4 I/O Multiplexing — epoll

**Goal:** Efficiently monitor multiple file descriptors for I/O readiness,
scalable beyond poll()'s O(n) scan.

- `epoll_create1(int flags)` — creates an epoll instance, returns an fd.
  `EPOLL_CLOEXEC` flag.
  - Internal: `epoll_t` struct with an interest list (red-black tree keyed by fd)
    and a ready list (doubly linked list of ready fds).
- `epoll_ctl(int epfd, int op, int fd, struct epoll_event* event)` — register
  / modify / unregister interest in `fd`.
  - `EPOLL_CTL_ADD`, `EPOLL_CTL_MOD`, `EPOLL_CTL_DEL`.
  - `event.events` is a bitmask: `EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLET`
    (edge-triggered) | `EPOLLONESHOT` | `EPOLLHUP`.
  - Internal: adds an `epitem` to the interest tree; registers a callback on
    the target fd's wait queue so that when the fd becomes ready, the epoll
    instance is notified and the fd is appended to the ready list.
- `epoll_wait(int epfd, struct epoll_event* events, int maxevents, int timeout)` —
  wait for readiness events. Returns number of ready fds.
  - Blocks on the epoll instance's wait queue if ready list is empty.
  - Edge-triggered: fd is removed from ready list after being returned once;
    re-armed only when new data arrives.
  - Level-triggered: fd stays in ready list as long as it remains readable/
    writable.
- **Integration:** Every fd in the system (socket, pipe, vfs, eventfd, timerfd,
  signalfd) has a wait queue. epoll hooks into that wait queue.
- **Files:** `os/src/kernel/epoll.c` / `os/src/kernel/epoll.h`,
  `os/src/include/sys/epoll.h`.
- **Syscall numbers:** 104–106.
- **Verify:** epoll instance monitors a pipe and an eventfd; both level-
  triggered and edge-triggered behaviour; timeout returns 0; EPOLLONESHOT
  fires exactly once.

---

### Phase 8.2 — Virtual Filesystems (procfs & sysfs)

A VFS-based window into kernel state, structured as ordinary files and
directories so that standard tools (`cat`, `ls`, `grep`) work without
special syscalls.

#### 8.2.1 procfs

**Goal:** Expose process and system information under `/proc`.

- **Mount point:** `proc_init()` creates a `procfs_t` superblock with
  a custom `vfs_fs_t` and `vfs_file_ops_t`.
  - Mounted at `/proc` during `vfs_init()` or at boot, before the shell starts.
  - Entries are synthetic: they do not correspond to disk blocks.
  - `readdir` enumerates process table entries (directories named by PID).
  - `open`/`read` on regular files call a generator function that fills a
    buffer on-the-fly.
- **Per-process directories (`/proc/<pid>/`):**
  - `cmdline` — command line (`args[0]`).
  - `status` — `Name:`, `State:`, `Pid:`, `Uid:`, `Gid:`, `VmSize:`, etc.
  - `mem` — raw memory access (size = entire address space).
  - `fd/` — symlinks to open file descriptors.
  - `cwd` — symlink to current working directory.
  - `root` — symlink to root directory.
  - `exe` — symlink to executable (if tracked).
  - `maps` — VMA list: address range, flags, offset, path.
- **System-wide files:**
  - `/proc/meminfo` — `MemTotal:`, `MemFree:`, `MemUsed:`, `SwapTotal:`,
    `SwapFree:`, slab usage.
  - `/proc/cpuinfo` — model name, features, cache, bogomips (per CPU).
  - `/proc/uptime` — seconds since boot + idle seconds.
  - `/proc/stat` — context switches, process count, CPU time.
  - `/proc/version` — kernel version string.
  - `/proc/self` — symlink to the reading process's `/proc/<pid>`.
  - `/proc/modules` — loaded kernel module list (empty until modules exist).
- **Implementation strategy:**
  - `procfs_node_t` wraps a union of `{pid, meminfo, cpuinfo, ...}`.
  - Read handlers check the node type and format output in a kernel buffer.
  - No writes allowed (all files are read-only).
  - Directories and symlinks are resolved lazily.
- **Files:** `os/src/kernel/procfs.c` / `os/src/kernel/procfs.h`.
- **Verify:** `cat /proc/meminfo` shows correct values; `ls /proc/1/`
  lists entries; `cat /proc/1/status` shows init's state.

#### 8.2.2 sysfs

**Goal:** Expose kernel objects (devices, drivers, classes) under `/sys`.

- **Mount point:** `sysfs_init()` creates a `sysfs_t` superblock mounted at `/sys`.
- **Top-level directories:**
  - `/sys/class/` — device classes: `net/`, `block/`, `tty/`, `input/`, etc.
  - `/sys/block/` — block devices (symlinks to `/sys/devices/...`).
  - `/sys/devices/` — device tree (platform, pci, virtio).
  - `/sys/kernel/` — kernel parameters: `ostype`, `osrelease`, `version`.
  - `/sys/module/` — loaded modules (empty until module infrastructure).
- **Per-class entries (e.g., `/sys/class/net/`):**
  - Each `${interface}/` directory contains:
  - `address` — MAC address string.
  - `mtu` — MTU value.
  - `operstate` — up/down.
  - `type` — Ethernet, loopback, etc.
  - `ifindex` — interface index.
  - `flags` — IFF_* flags.
- **Per-block entries (`/sys/block/<name>/`):**
  - `size` — block count.
  - `device/` — model, vendor.
- **Implementation:**
  - `sysfs_node_t` with a typed union and a read handler function pointer.
  - Callbacks registered by subsystems (net, block) during init.
  - All entries read-only; attribute store reserved for future `echo > value`.
- **Files:** `os/src/kernel/sysfs.c` / `os/src/kernel/sysfs.h`.
- **Verify:** `ls /sys/class/net/` shows `lo` and `eth0`;
  `cat /sys/class/net/eth0/address` shows the MAC.

---

### Phase 8.3 — Init System

**Goal:** A userspace process (PID 1) that manages service lifecycle, replaces
the current kernel-shell-as-init approach.

#### 8.3.1 /sbin/init ELF

- A new ELF binary `init.c` compiled and embedded (like other user programs).
- On boot, after mounting `/proc` and `/sys`, the kernel spawns `/sbin/init`
  as PID 1 instead of entering `shell_run()`.
- `init` is the ultimate parent of all processes; orphaned children are
  re-parented to init.
- **Responsibilities:**
  - Read `/etc/init.conf` (or `/etc/services/*.svc`) to discover services.
  - Start system services in dependency order.
  - Supervise running services (restart on crash, log output).
  - Handle SIGCHLD from child processes and reap zombie state.
  - On shutdown, send SIGTERM → SIGKILL to all children.
- **`init.c` structure:**
  - `main()` → parse config → resolve deps → launch services → event loop.
  - Uses `poll()` (or `epoll`) to wait on service exit signals and control
    channel.
  - Simple configuration: each service is a directory under `/etc/services/`
    containing `run` (ELF path), `env/` (environment variables), `depends`
    (list of prerequisite service names).
  - Service states: `STOPPED`, `STARTING`, `RUNNING`, `STOPPING`, `CRASHED`.
- **Files:** `os/src/boot/init.c`, built as embedded ELF `/sbin/init`.

#### 8.3.2 Service Definitions

**Goal:** Declarative service specification.

- Format: directory-based (not a single monolithic config) for simplicity.
  ```
  /etc/services/
    net/
      run       → /bin/networkd.elf
      depends   → (empty, no dependencies)
      env/      → (empty)
    httpd/
      run       → /bin/httpd.elf
      depends   → "net"
      env/
        PORT    → "8080"
  ```
- Fields:
  - `run` — path to the ELF executable.
  - `depends` — newline-separated list of prerequisite service names.
  - `env/*` — files whose filename=value set environment variables.
  - `restart` — policy string: `always`, `on-failure`, `never` (default: `on-failure`).
  - `respawn-delay` — seconds to wait between restarts (default: 1).
- Alternative (simpler): a single `/etc/init.conf` in INI format.
  ```ini
  [net]
  exec=/bin/networkd.elf
  depends=

  [httpd]
  exec=/bin/httpd.elf
  depends=net
  ```

#### 8.3.3 Service Launcher

**Goal:** Start a service as a child process with proper environment.

- `launch_service(svc_t* svc)`:
  - Fork a child process.
  - Child: set environment variables from `svc->env`, redirect stderr to
    the log pipe, exec the ELF.
  - Parent: record the child PID in the service table, set up a signal
    handler for SIGCHLD.
- Returns immediately; the child runs in the background.
- The child's exit is detected via SIGCHLD with `waitpid(-1, &status, WNOHANG)`.

#### 8.3.4 Service Manager (Supervisor)

**Goal:** Keep services running; restart on crash.

- A service state machine:
  ```
  STOPPED → STARTING → RUNNING → STOPPING → STOPPED
                            ↓ (crash)
                         CRASHED → STARTING (if restart policy allows)
  ```
- On any child exit, `init` receives SIGCHLD:
  - `waitpid()` reaps the zombie.
  - If the service is supposed to be running and restart policy matches:
    - Increment restart counter; if `restart_count > max_restarts` (default 5
      in 60s), mark as `CRASHED` permanently.
    - Otherwise: transition to `STARTING` and call `launch_service()` after
      `respawn_delay` seconds.
- Service stop:
  - Send SIGTERM; wait 5 seconds; if still alive, send SIGKILL.
  - On shutdown, reverse dependency order: stop dependents first.

#### 8.3.5 Dependency Resolution

**Goal:** Start services in correct order.

- Build adjacency list from `depends` fields.
- Topological sort to determine start order.
- Cycle detection: if a cycle is found, report and refuse to start the
  affected services.
- Service A "depends on" B means B must be RUNNING before A starts.

#### 8.3.6 Logging Infrastructure

**Goal:** Collect stdout/stderr from services.

- Each service gets a pipe for its stdout/stderr.
- `init` reads from all service pipes in its event loop and writes to
  a central log file (`/var/log/messages`) or a ring buffer accessible
  via `logread`.
- `logread` command reads the ring buffer.
- **Advanced (future):** syslog daemon replaces the built-in logging.

- **Files:** `os/src/boot/init.c`, `os/src/kernel/shell.c` (logread command).

#### 8.3.7 Shutdown/Reboot Handling

**Goal:** Cleanly stop services and unmount filesystems.

- `init` catches SIGTERM (sent by `shutdown` or `reboot` command).
- Sends SIGTERM to all services in reverse dependency order.
- Waits up to 10 seconds for all services to stop.
- Sends SIGKILL to stragglers.
- Syncs all filesystems.
- Calls the appropriate kernel syscall (`sys_reboot` or `sys_pwrdown`).

- **Verify:** `init` starts 3 services; one crashes and is restarted;
  `init` handles SIGTERM and shuts down cleanly.

---

### Phase 8.4 — System Events

**Goal:** A kernel-level publish/subscribe bus that delivers structured events
to userspace listeners.

#### 8.4.1 Kernel Event Bus

- **Design:**
  - A global event bus with typed event channels.
  - Each channel has a set of subscribed file descriptors.
  - When an event fires, the bus writes a structured `event_t` record to
    each subscriber's read-end.
  - Subscription uses a new syscall:
    `event_subscribe(int fd, uint32_t event_type, uint32_t flags)`.
    - `fd` — usually an eventfd or a special event bus fd.
    - `event_type` — `EVENT_PROCESS_EXIT`, `EVENT_DEVICE_ATTACH`, etc.
  - Events delivered via `read()` on the event bus fd.
- **Event types:**
  - `EVENT_PROCESS_EXIT` — pid, exit_code.
  - `EVENT_PROCESS_FORK` — parent_pid, child_pid.
  - `EVENT_DEVICE_ATTACH` — device name, type.
  - `EVENT_DEVICE_DETACH` — device name, type.
  - `EVENT_FS_MODIFY` — path, change type (create/delete/modify).
  - `EVENT_OOM` — killed pid.
  - `EVENT_POWER` — battery low, power connected (on hardware).
- **Event record format:**
  ```c
  typedef struct {
      uint64_t type;      // EVENT_*
      uint64_t timestamp; // ns since boot
      uint64_t data[6];   // type-specific payload
  } event_t;
  ```
- **Files:** `os/src/kernel/eventbus.c` / `eventbus.h` (extends existing
  eventbus skeleton, adds typed events + userspace delivery).

#### 8.4.2 Userspace Event Dispatch

- A `kerneld` daemon (or built into `init`) subscribes to the bus and
  dispatches events to interested userspace services via AF_UNIX or
  shared memory.
- Alternatively: a `/dev/eventbus` character device that any process can
  `read()` for the next event.
- **Verify:** Subscribe to `EVENT_PROCESS_EXIT`, kill a child process,
  observe the event on the subscribed fd.

---

### Phase 8.5 — Package Management

**Goal:** A userspace tool `pkg` that installs, removes, updates, and manages
software packages.

#### 8.5.1 Package Format

- **Container:** A tarball (ustar format) with `.pkg` extension.
- **Metadata:** First file in the tarball is `metadata.ini`:
  ```ini
  [package]
  name=busybox
  version=1.36.0
  desc=BusyBox multi-call binary
  license=GPL-2.0
  depends=glibc

  [files]
  bin/busybox.elf=/usr/bin/busybox
  share/doc/readme=/usr/share/doc/busybox/readme
  ```
- **File layout:**
  - `metadata.ini` — package descriptor.
  - `data/` — files to extract, relative to root.
  - `scripts/` — optional `pre-install`, `post-install`, `pre-remove`,
    `post-remove` executables.
- Package repodir: `/var/pkg/cache/<name>-<version>.pkg`.

#### 8.5.2 Package Database

- **Location:** `/var/pkg/db/`.
- **Per-package file:** `/var/pkg/db/<name>/manifest` — list of installed
  files with hashes.
- **Index:** `/var/pkg/db/index` — key-value pairs:
  ```
  busybox=1.36.0
  glibc=2.38
  ```
- **Format:** Plain text, line-oriented, sorted by name.
- **Lock file:** `/var/pkg/db/.lock` — prevents concurrent `pkg` operations.

#### 8.5.3 pkg Command (Userspace)

- `pkg install <name>.pkg`:
  1. Lock DB.
  2. Read metadata; check depends against index.
  3. Extract files; verify hashes.
  4. Run `post-install` script if present.
  5. Update index.
  6. Unlock DB.
- `pkg remove <name>`:
  1. Lock DB.
  2. Read manifest; remove all files.
  3. Run `post-remove` script if present.
  4. Remove from index.
  5. Unlock DB.
- `pkg update <name>.pkg`:
  1. Remove old version, install new version.
- `pkg list`:
  1. Print all entries from index.
- `pkg info <name>`:
  1. Print metadata for installed package.
- **Files:** `os/src/boot/pkg.c` (userspace ELF, built and embedded).
  `/usr/bin/pkg.elf`.

#### 8.5.4 Repository Support

- **Config:** `/etc/pkg/repos.conf`:
  ```
  [main]
  url=http://repo.oper-tur.org/pkg
  ```
- `pkg update-repo`:
  1. Download `repo.index` from repository.
  2. Compare with local index.
  3. List available updates.
- `pkg install <name>` (from repo):
  1. Download `<name>-<version>.pkg` from repo URL.
  2. Verify GPG signature (future).
  3. Install as above.
- **Downloading:** Uses the kernel's TCP + DNS + socket API. Requires the
  network service to be running.

---

### Exit Criteria

- [x] **8.1:** Two processes share memory via `shmget`/`shmat`; message queue
  round-trip works; eventfd triggers POLLIN; epoll monitors 10 fds
  simultaneously.
- [x] **8.2:** `cat /proc/meminfo` returns valid numbers; `ls /sys/class/net`
  shows network interfaces.
- [x] **8.3:** `init` reads `/etc/init.conf`, starts 3 services, restarts
  a crashed one, and shuts down cleanly on SIGTERM.
- [x] **8.4:** Kernel event bus delivers `EVENT_PROCESS_EXIT` to a subscriber.
- [x] **8.5:** `pkg install foo.pkg` extracts files, updates index;
  `pkg remove foo` removes them.
- [ ] All components are self-tested via in-kernel or userspace tests.
- [ ] System boots with `init` as PID 1; shell is a service.
- [ ] No stub or placeholder implementations remain in any Stage 8 component.

---

## STAGE 9 — Quality, Testing and Scalability `~20%`

### Tooling

- ❌ **GDB stub** — No remote debugging protocol; cannot attach GDB over serial or network.
- ❌ **Trace viewer** — No structured event trace capture or visualisation.
- ❌ **Module loading** — Kernel is monolithic; no loadable kernel module infrastructure.
- ✅ **Test harness** — `test_framework.h` with assertion macros; kernel self-tests (kernel_test.c), SFS/VFS tests (sfs_test.c), process tests (process_test.c), security tests (security_test.c). `make test-all` runs 81 tests.
- ❌ **Crash dumps** — No core dump on kernel panic or process crash; debug info is lost on reboot.

### Testing

- 🟡 **Stress testing** — Manual stress scripts exercise the allocator and scheduler under load; no automated repeat or CI integration.
- ❌ **Fuzzing** — No syscall fuzzer or filesystem fuzzer in place.
- ✅ **Regression tests** — 81-unit test suite across 6 subsystems (net, storage, kernel, SFS, process, security). Single `make test-all` command runs all tests in one QEMU boot.
- ❌ **Benchmarks** — No reproducible throughput or latency benchmarks.

### Scalability

- 🟡 **Locking discipline** — Major subsystems use coarse locks; per-object or per-CPU locking needed for SMP scaling.
- 🟡 **Shared-state cleanup** — Some global structures lack clear ownership; ongoing refactor to eliminate implicit shared state.
- ❌ **Resource ownership model** — No formal ownership or reference-counting discipline for kernel objects.

### Existing Infrastructure

- ✅ **Compiler toolchain** — GCC cross-compiler targeting `x86-64-elf`; build system generates kernel ELF and userspace binaries.
- ✅ **Assembler/linker** — NASM for assembly stubs; linker script controls final kernel memory layout and section placement.
- ✅ **Memory inspection** — Kernel shell command to dump physical/virtual memory regions, PMM bitmap, and slab stats.
- 🟡 **Symbol maps** — `nm`-generated symbol map loaded at boot for panic backtraces; not yet used for live kernel introspection.
- ✅ **Boot self-tests** — Early boot runs sanity checks on the PMM, slab, and page-table code before launching init.
- ✅ **Error codes** — Unified errno-compatible error code set used throughout kernel and returned to userspace.
- 🟡 **Performance counters** — Basic cycle and instruction counters read via RDTSC; no PMU event programming yet.
- ✅ **Code consistency** — Coding style enforced by a clang-format config; reviewed for consistent naming and error-handling patterns.

### Exit Criteria

- [ ] GDB stub connects and reports register state
- [ ] Regression test suite runs and reports pass/fail
- [ ] Fuzzer executes 10 000 syscall sequences without kernel panic
- [ ] Benchmark produces reproducible throughput numbers
- [ ] Kernel module loads and unloads cleanly

---

## STAGE 10 — GUI Foundation `~5%`

### Graphics

- ❌ **Framebuffer** — No linear framebuffer initialised; UEFI GOP is not yet used for pixel output.
- ❌ **GPU driver** — No 2D or 3D GPU command submission; all rendering would need to be CPU-side.
- ❌ **Graphics API** — No drawing primitives (blit, fill, line) exposed to userspace.

### Input

- 🟡 **Keyboard/mouse stack** — PS/2 keyboard scan-code translation works; USB HID and mouse movement are not yet handled.
- ❌ **Cursor handling** — No hardware or software cursor; no mouse pointer rendered on screen.

### Desktop

- ❌ **Window manager** — No window placement, focus, or decoration logic.
- ❌ **Display server** — No Wayland/X11-compatible protocol for client window surfaces.
- ❌ **Compositor** — No compositing pipeline to blend window surfaces.
- ❌ **GUI event loop** — No main-thread event pump delivering input events to GUI clients.

### Rendering

- ❌ **Font rendering** — No TrueType/OpenType rasteriser; text output is limited to a bitmapped console font.
- ❌ **Image decoding** — No PNG/JPEG decoder for loading assets.
- ❌ **Shared rendering memory** — No zero-copy shared buffer protocol between renderer and compositor.

### Desktop Features

- ❌ **Clipboard** — No clipboard abstraction or copy/paste mechanism.
- ❌ **GUI sandboxing** — No per-window permission model or content isolation.

### Exit Criteria

- [ ] Framebuffer displays a filled rectangle
- [ ] Keyboard input reaches a GUI client
- [ ] Window manager places and focuses two windows
- [ ] Font rendering draws legible text on screen
- [ ] Compositor blends two overlapping windows correctly

---

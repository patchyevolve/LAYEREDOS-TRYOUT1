# OPERtur/TRY1 OS — Implementation Progress

Legend: ✅ Implemented | 🟡 Partial | ❌ Not implemented

## Core boot and hardware
- ✅ Stable boot path and higher-half mapping
- ✅ GDT, IDT, TSS, and interrupt stubs
- 🟡 PIC/APIC interrupt routing (xAPIC MMIO mapping skipped on QEMU TCG via `hal_is_qemu_tcg()` detection — falls back to legacy PIC. APIC timer active on KVM/bare metal; on TCG, PIT used for scheduling tick)
- ✅ UART/serial console
- 🟡 PIT and HPET timer (HPET MMIO mapping skipped on QEMU TCG via `hal_is_qemu_tcg()` detection — falls back to PIT for scheduling tick. HPET ns-resolution timekeeping available on KVM/bare metal)
- ✅ Basic CPU feature detection (SMAP, MWAIT via CPUID)
- ✅ QEMU TCG boot stability (HPET/APIC MMIO mapping skipped as workaround for TCG softmmu cache issue; kernel boots to shell cleanly)
- ✅ Panic path with useful debug output (stack trace, register dump)
- ✅ Watchdog / health checking

## Memory management
- ✅ Physical memory manager (page-level alloc/free, bitmap, OOM kill)
- ✅ Virtual memory manager (4-level page tables, map/unmap/walk, TLB flush)
- ✅ Kernel heap allocator (slab allocator, large alloc fallback)
- ✅ Page fault handling (page fault handler with user/kernel split)
- ✅ Demand-safe user/kernel address split (top-half kernel, bottom-half user)
- ✅ Copy-from-user and copy-to-user helpers (SMAP-aware, range-checked)
- ✅ OOM handling (EV_OOM_KILL event, kills current process)
- ✅ Swap support (RAM-backed swap store, slot bitmap, PTE encoding, swap-in on page fault)
- ✅ Double-free / corruption detection (in kfree and pmm_free_page)
- ✅ Better allocation fast paths (O(1) alloc_page, O(N) alloc_pages)
- ✅ Per-page permission flags: user, kernel, read, write, execute (PAGE_USER, PAGE_WRITE, PAGE_NX)

## Scheduler and threading
- ✅ Preemptive scheduler (timer interrupt-driven, O(1) priority pick)
- ✅ Thread control blocks (TCB with state, priority, stack, name)
- ✅ Context switch correctness (assembly save/restore in ctx.S)
- ✅ Sleep and wake queues (sleep on timer, wake queues for sync/pipe/UART)
- ✅ Mutexes (with timeout support: try/lock-forever/lock-ms)
- ✅ Condition variables (wait/signal/broadcast)
- ✅ Priority inheritance (mutex boosts lower-priority owner)
- ✅ Priority aging / starvation avoidance (boosts ready threads every 50ms)
- ✅ Lightweight thread creation / SYS_CLONE
- ✅ Process abstraction (process table, PID allocation)
- ✅ fork, exec, exit, wait (syscall implementations)
- ✅ Per-process kernel stack handling (separate kernel stack per thread)
- ✅ Per-process address spaces (separate PML4 per process)
- ✅ Signal delivery with sigreturn (sigframe_t on user stack, trampoline at SIGNAL_TRAMPOLINE_ADDR, SYS_SIGRETURN restores context)
- ✅ Thread/process cleanup and reaping (zombie reaper, process exit frees user pages)
- ✅ Work queues (work_queue_t with spinlock-protected list, system_wq for kernel-wide scheduling)
- ✅ Deferred tasks (deferred_task_t one-shot timer API, polls on system work queue)
- ✅ Kernel worker threads (kworker processes system work queue at THREAD_DEF_PRIO)

## Syscall and userspace
- ✅ Syscall gateway (int 0x80, 90 syscalls 0–89: sockets 38–52, capabilities 54–55, audit 56, UID/GID 57–62, CSPRNG 63, syscall filter 64, socketpair 65, prctl 66, secure boot 67, netns 68–71, fs 72–88, sched_setaffinity 89)
- ✅ Syscall argument validation (user-range + mapped + SMAP checks)
- ❌ Userspace ABI stability (no formal ABI)
- ✅ Userspace C library (full libc: stdio/printf, stdlib/malloc, string, unistd syscall wrappers, signal, errno, crt0)
- ✅ ELF loading (ELF64, PT_LOAD segments, PT_INTERP, NX support)
- ✅ Dynamic linker/loader (ld.so: ET_DYN PIE loaded by kernel via PT_INTERP, ELF parsing, symbol resolution, RELA/PLT relocations, shared library loading via mmap, aux-vector setup, init/fini arrays)
- ✅ Standard file descriptor table (32 FD slots, pre-allocated 0/1/2)
- ✅ Standard input/output/error wiring (fd 0/1/2 wired to TTY via VFS; dup2 supported)
- ✅ Minimal init process (pid 1, runs shell)
- ✅ Basic service launcher (/etc/rc startup script sourced by shell at boot)

## Storage and filesystem
- ✅ PCI bus enumeration (config space, bus scanning, bridge recursion)
- ✅ ATA/AHCI/NVMe driver (ATA PIO IRQ-driven + AHCI DMA + NVMe admin/I/O queues; all three registered as block devices)
- ✅ IRQ-based block I/O — ATA PIO uses `sched_block`/`sched_wake` on per-drive wait queues; IRQ handlers for IRQs 14/15; timeout watchdog
- ✅ Block device abstraction layer (register, find, read, write)
- ✅ Sector cache — LRU write-back cache (64 entries, dirty tracking, eviction flushes to device)
- ✅ Journaling/WAL — circular buffer of DATA+COMMIT entries (63 slots); recover replays committed transactions; checkpoint on commit; mid-txn commit-and-restart on full journal
- ✅ Writable filesystem (SFS: superblock, bitmaps, inodes, data blocks)
- ✅ VFS core (node tree, path walking, fd operations)
- ✅ Directory creation (vfs_mkdir → sfs_vfs_create with DIR type)
- ✅ File creation and deletion (vfs_create, vfs_unlink)
- ✅ File writes and truncation (vfs_write, vfs_ftruncate)
- ✅ File reads and seeking (vfs_read, vfs_lseek)
- ✅ mkdir (shell built-in)
- ✅ rm (shell built-in, multi-arg support)
- ✅ edit (simple line-based editor built-in: l, e, d, a, w, q commands)
- ✅ cat (shell built-in)
- ✅ ls (shell built-in, -la flags, readdir-based)
- ✅ stat (shell built-in)
- ✅ chmod (shell built-in, octal mode, e.g. 0644)
- ✅ touch (shell built-in, multi-arg support)
- ✅ Multi-mount VFS (mount table, prefix matching, per-FS path dispatch)
- ✅ Cross-block dirent handling (SFS_BLOCK_SIZE=512 × sizeof(sfs_dirent_t)=68 → entries span block boundaries; readdir/lookup/remove use byte-offset block math with 2×SFS_BLOCK_SIZE buffer for reassembly)
- ✅ tmpfs (RAM-backed filesystem mounted at /tmp, all VFS ops)
- ✅ devfs (device filesystem mounted at /dev: null, zero, random, full)
- ✅ Journaling/WAL — crash recovery via write-ahead log (DATA+COMMIT entries, recover replays committed transactions)
- ✅ File permissions and ownership (mode enforced on open-for-write, default 0644, chmod shell command)
- ✅ File locking (advisory, lock/unlock shell commands, SFS inode flag)

## Terminal and shell
- ✅ Real TTY/terminal layer (kernel/tty.c + kernel/tty.h: line discipline, canonical/raw mode, echo, signal generation. Ctrl-C SIGINT, Ctrl-Z SIGTSTP, Ctrl-\ SIGQUIT, Ctrl-D EOF, Ctrl-U/K/W kill. ISR feeds TTY raw buffer; ISR-level signal detection delivers signals to fg pgid via work queue. Termios ioctl, PTY subsystem, SIGTTIN/SIGTTOU job control all implemented.)
- ✅ Line editing (readline-style: arrows, home/end, backspace/del, Ctrl-U/K/W/A/E/C)
- ✅ Command history (64-entry circular, up/down arrows)
- ✅ Tab completion (commands + aliases + VFS paths)
- ✅ Pipes (up to 8 stages, internal pipe buffers)
- ✅ Redirection (>, >>, <)
- ✅ Environment variables ($NAME expansion, set/unset/printenv)
- ✅ Scripting support (source command, up to 8 nesting levels)
- ✅ Job control (bg/fg/jobs commands, process stop/continue via signals)
- ✅ Shell builtins (47 commands)
- ✅ Core userland commands (ls, cat, cp, mv, echo, etc.)
- ✅ File utilities (cp, mv, rm, mkdir, rmdir, touch, writefile, stat)
- ✅ Process utilities (ps, kill, nice, top)
- ✅ Basic text tools (head, tail, wc, grep)

## Networking
- ✅ NIC driver support (E1000: PCI 0x8086:0x100E detection, BAR0 MMIO, descriptor rings, send/poll, IRQ 11)
- ✅ Ethernet frame handling (eth.c/eth.h: frame encode/decode, EtherType dispatch, handler registration for ARP/IPv4/IPv6, wired into E1000 IRQ handler via eth_rx_poll)
- ✅ ARP (cache, packet construction, resolution, IPv4→MAC)
- ✅ NDP (neighbor solicitation/advertisement, cache, link-local address resolution)
- ✅ IPv4 (packet routing, send/recv, address assignment, broadcast handling)
- ✅ IPv6 (packet routing, send/recv, link-local EUI-64, solicited-node multicast)
- ✅ ICMPv4 (echo reply/ping)
- ✅ ICMPv6 (echo reply, NS/NA, RS/RA)
- ✅ IGMPv2 (IPv4 multicast group management, membership reports/queries)
- ✅ MLDv1 (IPv6 multicast listener discovery, group reports/queries)
- ✅ E1000 MTA (multicast table array programming for IPv4/IPv6 multicast groups)
- ✅ UDP (sendto/recvfrom, checksum validation, dual-stack, endpoint datagram queue)
- ✅ TCP (full state machine: CLOSED/LISTEN/SYN_SENT/SYN_RECEIVED/ESTABLISHED/FIN_WAIT1/FIN_WAIT2/CLOSE_WAIT/CLOSING/LAST_ACK/TIME_WAIT, retransmission, RTO, RST generation for both IPv4/IPv6, ACK tracking, snd_una/snd_wnd, TIME_WAIT 2MSL timer, TCP_NODELAY/Nagle, SO_RCVTIMEO/SO_SNDTIMEO, poll, IPv4-mapped IPv6)
- ✅ DNS resolver (kernel-level dns_resolve, A/AAAA queries, response parsing, configurable resolver)
- ✅ DHCP client (kernel-level DORA, option parsing, lease management)
- ✅ SLAAC (RS/RA exchange, prefix parsing, EUI-64 global address formation)
- ✅ NTP client (kernel-level NTPv4, request/response, wall clock, clock_gettime syscall)
- ✅ Sockets API (socket/bind/connect/listen/accept/send/recv/sendto/recvfrom/close, fd dispatch, AF_INET/AF_INET6, IPV6_V6ONLY, IP_ADD/DROP_MEMBERSHIP, setsockopt/getsockopt, error propagation via kernel_err_to_posix)
- ✅ poll() syscall (POLLIN/POLLOUT/POLLERR, implemented for TCP and UDP sockets)
- ✅ Socket-level multicast (IP_ADD_MEMBERSHIP/IP_DROP_MEMBERSHIP, IPV6_JOIN_GROUP/IPV6_LEAVE_GROUP)
- ❌ TLS support (plan: docs/7_TLS_PLAN.md — userspace libtls.a wrapping TCP sockets with TLS 1.3)
- ✅ Network namespaces (net_ns_t struct, unshare CLONE_NEWNET, veth pairs, sys_netconfig/sys_veth_move, cross-namespace TCP test)

## Security and isolation
- ✅ Capability system (4 caps: CAP_SYS_BOOT, CAP_KILL, CAP_NET_RAW, CAP_SYS_ADMIN; capget/capset syscalls; enforced at reboot/kill/setpgid)
- ✅ Permission model (POSIX DAC: owner/group/other rwx bits; vfs_access_check; uid/gid per process)
- ✅ User/group identity (uid_t/gid_t; euid/egid; init uid 0; getuid/geteuid/getgid/getegid/setuid/setgid syscalls)
- ✅ Sandboxing (fork_limit enforced; syscall filtering per process; no_new_privs via prctl)
- ✅ Audit logging (256-entry ring buffer; audited at capability denials, fork rejections, sensitive syscalls, exec/exit)
- ✅ Kernel/user memory isolation enforcement (user range limited, SMAP/SMEP)
- ✅ Read-only kernel text (cleared PAGE_WRITE on .text and .rodata after boot)
- ✅ NX / non-executable memory (NX bit on user stack/data segments)
- ✅ Basic signals (SIGTERM/SIGKILL/SIGSTOP/SIGCONT/SIGTSTP, sys_kill/sys_sigaction, default actions)
- 🟡 ASLR (PIE binaries load at random base using RDTSC; fixed-address EXEC binaries deterministic)
- ✅ Secure boot chain (build-time SHA-256 hash whitelist of all embedded ELFs; rejected on exec; sys_secure_boot enable/disable/query)
- ✅ Signed binaries (hash whitelist per build; gen_secure_boot_hashes.py generates whitelist)
- ✅ Syscall filtering or policy hooks (syscall_mask[4] = 256 bits per process; sys_set_ssf drops bits only; reset on exec)
- ❌ Privilege separation for services (deferred to Stage 8 init system)
- ✅ Secure IPC (AF_UNIX socketpair: 4KB ring buffers, SO_PEERCRED returns peer uid/gid/pid; sys_socketpair)
- ✅ Secure random subsystem (SHA-256 counter mode CSPRNG; seeded from RDTSC + timer jitter; sys_getrandom)
- ✅ Hashing framework (SHA-256: init/update/final/sha256; verified against NIST FIPS 180-4 vectors)
- ✅ Kernel crypto primitives (SHA-256 as fundamental building block; used for PRNG and secure boot)

## Filesystem and data integrity
- ✅ Better inode model (direct + singly-indirect + doubly-indirect; max ~8 MB)
- ✅ Metadata timestamps (atime/mtime/ctime in inode and stat output)
- ✅ Hard links (ln command, ref-counted inodes, nlink field)
- ✅ Symbolic links
- ✅ Sparse files (reads zero-fill holes; writes skip unallocated blocks)
- ✅ Append mode (O_APPEND support, shell >> redirection, write-through cache still safe on append)
- ✅ Rename support (atomic rename via vfs_rename/sfs_vfs_rename, mv uses it with cp+unlink fallback)
- ✅ Atomic file update semantics (rename overwrites target atomically)
- ✅ Crash recovery (journaling/WAL + fsck with repair)
- ✅ Snapshot/rollback (point-in-time full-device snapshots with rollback via snap take/rollback commands)
- ✅ Backup/restore support (full recursive archive format, save/restore shell commands via backup.c)

## GUI-capable foundation
- ❌ Framebuffer driver
- ❌ GPU driver
- 🟡 Input stack for keyboard and mouse (PS/2 keyboard only, no mouse)
- ❌ Cursor handling
- ❌ Window manager or display server
- ❌ Graphics API for clients
- ❌ Font rendering
- ❌ Image decoding
- ❌ Basic compositor
- ❌ GUI app event loop
- ❌ Shared memory for rendering
- ❌ Clipboard support
- ❌ GUI app sandboxing

## Developer tooling
- ✅ Compiler toolchain support (GCC + ld, freestanding, standard ELF)
- ✅ Assembler and linker support (GAS + ld, linker script)
- ❌ Debugger hooks (no GDB stub)
- ✅ Memory inspection commands (meminfo, stats)
- ❌ Trace/log viewer
- 🟡 Kernel symbol map (kernel ELF has symbols for GDB debugging)
- ❌ Module loading (no kernel modules)
- ✅ Test harnesses (test_framework.h: assertion macros; kernel_test.c/h: 37 kernel tests; sfs_test.c/h + process_test.c/h + security_test.c/h; make test-all runs 81 tests)
- ✅ Boot-time self-tests (ELF load, user process spawn, 81 in-kernel tests)
- ❌ Crash dump collection (panic halts, no dump)

## Quality and scalability
- 🟡 Locking discipline (spinlocks for critical sections, but not pervasive)
- 🟡 No hidden shared mutable state (mounted_fs still global but most ops dispatch through per-node fs)
- ❌ Clear resource ownership rules (implicit)
- ✅ Proper error codes (ERR_* errno-style constants)
- 🟡 Cleanup on partial failure (sfs_add_dirent cleans up alloc failure)
- 🟡 Performance counters (basic: switch/yield count, free pages)
- 🟡 Stress testing (mutex contention test, compute threads)
- ❌ Fuzzing for syscalls and filesystem
- ✅ Regression tests (make test-all: 81 tests across 6 suites; make test-net/test-kernel/test-sfs/test-process/test-security for individual suites)
- ❌ Benchmarking tools
- ✅ Code style consistency (uniform style across kernel)
- ✅ NUMA awareness (SRAT/SLIT parsing, per-node PMM free lists, node-local allocation for kmalloc/vmm/sched)
- ❌ NUMA-aware scheduler (no topology-aware thread placement — deferred to Stage 9 optimization)
- ✅ NUMA-aware memory allocation (per-node free lists, fallback chain: local → nearest → any → steal → OOM)

## Service layer and IPC
- ❌ Shared memory between processes (no MAP_SHARED)
- ❌ Message queues (no POSIX or custom message queue)
- ❌ Event queues (no epoll/kqueue-style multiplexing)
- ❌ Kernel event bus exposed to userspace
- ❌ Service launcher (no supervised start/stop)
- ❌ Service manager (no liveness tracking or restart policy)
- ❌ Dependency-ordered service startup
- ❌ Process event notifications (start/exit/state change)
- ❌ Filesystem event notifications (no inotify equivalent)
- ❌ Device hotplug events to userspace
- ❌ procfs (no /proc filesystem)
- ❌ sysfs (no /sys filesystem)
- ❌ Process statistics export via virtual filesystem
- ❌ Device statistics export via virtual filesystem
- ❌ Runtime kernel statistics export via virtual filesystem

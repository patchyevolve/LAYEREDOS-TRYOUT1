# OPERtur / TRY1 OS — Staged Development Roadmap

**Legend:** ✅ Implemented · 🟡 Partial · ❌ Not implemented

---

## STAGE 1 — Core Kernel Foundation `~100%`

### Boot and Hardware

- ✅ **Stable boot path and higher-half mapping** — Bootstraps from bootloader into a higher-half virtual layout; kernel lives above `0xFFFFFFFF80000000`, leaving the lower address space free for userspace.
- ✅ **GDT, IDT, TSS, and interrupt stubs** — GDT sets up kernel/user segments; IDT registers handlers for all 256 vectors; TSS supplies the kernel stack pointer on privilege transitions.
- ✅ **PIC/APIC interrupt routing** — Legacy 8259 PIC active (remapped to `0x20–0x2F`) and xAPIC local APIC enabled with LINT0=ExtINT (PIC passthrough), LINT1=NMI. APIC timer replaces PIT for scheduling ticks. SMP delivery pending I/O APIC discovery.
- ✅ **UART/serial console** — 16550-compatible UART driver provides early-boot logging and a kernel debug console before any framebuffer is available.
- ✅ **PIT and HPET timer** — HPET detected at MMIO `0xFED00000`, 3 timers, 100 MHz. HPET used for nanosecond-precision timekeeping via `hal_timer_get_ns()`. PIT still provides the scheduling tick (IRQ0), masked when APIC timer is active.
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

## STAGE 2 — Userspace Foundation `~100%`

### Syscall Layer

- ✅ **Syscall gateway** — int 0x80 fast path with full register save; syscall table maps 34 syscall numbers (0–33) to kernel handlers.
- ✅ **Syscall validation** — All pointer arguments validated against user address range via copy_from_user/copy_to_user; invalid addresses return `-EFAULT` before any kernel state is touched.
- ✅ **Stable userspace ABI** — 34 syscalls (0–33) with stable numbers; mmap/munmap/mprotect added for dynamic linker support. Syscall numbers are stable within the build.

### Userspace Runtime

- 🟡 **Userspace libc** — Minimal syscall wrappers in `libuser/user.c`; `printf`, `puts`, `snprintf`, `malloc`/`free`, `calloc`, `atexit`/`exit`, `__libc_init` implemented. Statically linked into user binaries.
- ✅ **ELF loading** — Static ELF64 binaries parsed and loaded; PT_LOAD segments mapped with correct permissions; PIE support with ASLR.
- ✅ **Dynamic loader support** — ld.so dynamic linker/loader (ET_DYN PIE) loaded by kernel via PT_INTERP; ELF parsing, symbol resolution, RELA/PLT relocations, shared library loading via mmap, aux-vector stack setup, init/fini array calling.

### Process Environment

- ✅ **File descriptor table** — Per-process FD table (32 slots) with reference-counted file descriptions; `dup`/`dup2`/`close`/`open` all operate correctly.
- ✅ **stdin/stdout/stderr wiring** — File descriptors 0/1/2 are wired to the serial console; dup2 syscall enables custom FD wiring. Proper TTY line discipline is not yet implemented.
- ✅ **Init process** — PID 1 is launched at boot; it reaps orphaned children.
- ✅ **Service launcher** — /etc/rc startup script sourced at boot by the shell; shell runs rc file before showing the prompt. Services can be started from the rc script with `&` for background execution.

### Exit Criteria

- [x] Syscall gateway rejects invalid pointers with EFAULT
- [x] Static ELF binary loads and runs to completion
- [x] FD table handles open/dup/close correctly
- [x] Init process (PID 1) reaps orphaned children

---

## STAGE 3 — Storage and Filesystem `~100%`

### Storage

- ✅ **PCI enumeration** — Full PCI/PCIe configuration-space scan at boot; BAR mapping implemented.
- 🟡 **ATA/AHCI/NVMe support** — ATA PIO read/write with LBA28; IRQ-driven. AHCI/NVMe not implemented.
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
- ❌ **Backup/restore** — No built-in archive or backup tooling.

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

## STAGE 4 — Terminal and User Environment `~95%`

### Shell

- 🟡 **Real TTY layer** — Line discipline (canonical/raw mode, echo, signal generation) implemented. Ctrl-C → SIGINT, Ctrl-Z → SIGTSTP, Ctrl-\ → SIGQUIT, Ctrl-D → EOF. ISR feeds TTY raw buffer; shell reads raw bytes via `tty_getchar()`; user-space processes read processed lines via `tty_vfs_read()`. FDs 0/1/2 wired to console TTY through VFS. `/dev/ttyS0` registered in devfs. **ISR-level signal detection**: signal chars detected in UART ISR and delivered via work queue (process context) — CPU-bound processes can be interrupted immediately without needing a TTY read. Process groups (`pgid`) added to `process_t`; `tty_signal_fg()` signals all members of the foreground group. Shell `cmd_run`/`cmd_fg` set `fg_pgid` for proper job control. Termios ioctl, PTY support, and SIGTTIN/SIGTTOU not yet implemented.
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

## STAGE 5 — Networking `0%`

### Link Layer

- ❌ **NIC driver support** — No Ethernet or virtual NIC driver exists. Network hardware is detected by PCI scan but has no driver attached.
- ❌ **Ethernet** — No frame transmit/receive path; the link layer is entirely absent.

### Network Layer

- ❌ **ARP** — No ARP cache or packet construction; IP-to-MAC resolution unavailable.
- ❌ **IPv4** — No IPv4 packet routing, fragmentation, or address assignment.
- ❌ **IPv6** — No IPv6 stack.
- ❌ **ICMP** — No ping or ICMP error-message handling.

### Transport Layer

- ❌ **UDP** — No UDP socket or datagram multiplexing.
- ❌ **TCP** — No TCP connection state machine, flow control, or retransmit logic.

### User Networking

- ❌ **DNS** — No resolver or stub DNS client.
- ❌ **Sockets API** — No BSD socket syscalls (`socket`/`bind`/`connect`/`send`/`recv`).
- ❌ **TLS** — No TLS implementation or crypto library.

### Isolation

- ❌ **Network namespaces** — No per-process network stack isolation.

### Network Services

- ❌ DHCP client
- ❌ NTP client

### Exit Criteria

- [ ] NIC detected by driver
- [ ] DHCP lease obtained
- [ ] Ping works
- [ ] DNS resolves
- [ ] TCP connection succeeds
- [ ] HTTPS request succeeds

---

## STAGE 6 — Security and Service Infrastructure `~35%`

### Identity

- ❌ **User/group identity** — No UID/GID concept; all processes run as a single implicit superuser.
- ❌ **Permission model** — No DAC or MAC enforcement beyond basic filesystem rwx bits.
- ❌ **Capability system** — No fine-grained capability splitting of root privileges.

### Isolation

- ❌ **Sandboxing** — No namespace or seccomp-style process confinement.
- ❌ **Privilege separation** — No mechanism to drop privileges after startup.
- ❌ **Secure IPC** — No authenticated or access-controlled IPC channel.

### Auditing

- ❌ **Audit logging** — No record of security-relevant events (file opens, privilege changes, syscall patterns).

### Security Hardening

- 🟡 **ASLR** — PIE binaries load at random base (0x40000000-0x60000000 using RDTSC); stack base randomized. Fixed-address EXEC binaries use deterministic addresses.
- ❌ **Secure boot** — No verified boot chain; any binary placed on disk will be executed.
- ❌ **Signed binaries** — No code-signing check on ELF load.
- ❌ **Syscall filtering** — No syscall allowlist/denylist enforcement per process.

### Existing Security

- ✅ **Kernel/user isolation** — CPL3 user-mode cannot access CPL0 kernel pages; SMAP/SMEP enabled where the CPU supports them.
- ✅ **Read-only kernel text** — Kernel `.text` section mapped with write-protect bit; accidental overwrites of code produce a page-fault panic.
- ✅ **NX memory** — All data pages (heap, stack, user data) marked NX; execution from data regions triggers a protection fault.
- ✅ **Basic signals** — SIGKILL and SIGSEGV are delivered correctly; broader signal infrastructure is still under construction.

### Cryptography

- ❌ Secure random subsystem
- ❌ Hashing framework
- ❌ Kernel crypto primitives

### Exit Criteria

- [x] Unprivileged process cannot read kernel memory (SMAP/SMEP, CPL3 vs CPL0 enforced)
- [ ] Capability check blocks an unauthorized syscall
- [ ] Audit log records a privilege event
- [x] ASLR produces different load addresses across runs (PIE binaries)
- [ ] Sandboxed process cannot fork beyond its policy

---

## STAGE 7 — SMP and Parallel Processing `0%`

### CPU Bring-up

- ✅ **APIC support** — xAPIC detected via CPUID, MSR-based enable, MMIO-mapped registers. Local APIC initialized with spurious vector, LVT entries configured. SMP delivery pending I/O APIC discovery and CPU topology enumeration.
- ✅ **Local APIC timer** — LAPIC timer configured for periodic mode (~1000 Hz) with programmable divider and initial count. Currently drives scheduling tick alongside PIT; foundation for per-CPU scheduler preemption.
- ❌ **IPI support** — No inter-processor interrupt delivery; cores cannot signal each other for TLB shootdowns, reschedules, or cross-core wakeups.
- ❌ **Secondary CPU startup** — No INIT/SIPI sequence to bring application processors out of reset; only the bootstrap processor (BSP) runs.
- ❌ **Multi-core boot sequence** — No AP trampoline code or per-AP GDT/IDT/stack setup to bring secondary cores into 64-bit protected mode.
- ❌ **CPU enumeration** — No MADT parsing to discover the number of logical CPUs, their APIC IDs, or NUMA topology.

### Scheduler SMP Support

- ❌ **Per-CPU run queues** — Scheduler uses a single global run queue with a coarse lock; contention will scale poorly beyond 1 core.
- ❌ **CPU affinity** — No mechanism to pin or prefer a thread to a specific core; all scheduling decisions are core-agnostic.
- ❌ **Cross-core wakeups** — Waking a thread that last ran on a different CPU requires an IPI; without IPI support this is not yet possible.
- ❌ **Cross-core scheduling** — No logic to migrate a runnable thread from an overloaded core to an idle one.
- ❌ **Load balancing** — No periodic or event-driven rebalancing of run-queue depths across cores.
- ❌ **Work stealing** — No idle-core work-stealing loop to pull tasks from a busy sibling's queue.

### Memory Consistency

- ❌ **SMP-safe page allocator** — PMM uses a non-atomic bitmap; concurrent frame allocation from multiple cores will corrupt the bitmap.
- ❌ **SMP-safe heap allocator** — Slab allocator assumes single-CPU access in the slow path; cross-CPU freeing via the magazine layer is not yet safe.
- ❌ **TLB shootdowns** — No IPI-driven TLB invalidation broadcast; page-table modifications on one core are not reflected on others until a context switch.
- ❌ **Cross-core page invalidation** — `INVLPG` is only issued on the local core; remote cores retain stale TLB entries for unmapped or remapped pages.
- ❌ **Memory barriers** — No systematic use of `MFENCE`/`SFENCE`/`LFENCE` in shared data paths; correct only by accident on TSO x86.
- ❌ **Atomic operations framework** — No kernel-wide API (`atomic_t`, `cmpxchg`, `fetch_add`) wrapping x86 lock-prefixed instructions for lock-free primitives.

### Synchronization

- ❌ **Spinlock auditing** — No tracking of which spinlock is held on which CPU; impossible to detect accidental sleep-while-spinning or lock inversion.
- ❌ **Reader/writer locks** — No `rwlock` primitive; all shared data structures protected by exclusive locks regardless of read/write ratio.
- ❌ **Seqlocks** — No sequence-counter lock for read-mostly data that must not block writers (e.g. clock, jiffies).
- ❌ **Lock ordering rules** — No documented or enforced global lock hierarchy; lock-order inversions between subsystems are possible.
- ❌ **Deadlock detection** — No runtime cycle-detection in the lock dependency graph (lockdep equivalent).
- ❌ **Contention statistics** — No per-lock wait-time or contention counters to guide optimisation.

### Cache Coherency and Performance

- ❌ **False-sharing mitigation** — Frequently written per-CPU fields are not padded to cache-line boundaries; adjacent fields on different CPUs will bounce the same cache line.
- ❌ **Cache-line alignment** — Hot kernel structures (run queues, allocator magazines) not annotated with `__cacheline_aligned`; layout is left to the compiler.
- ❌ **Per-CPU data structures** — No `DEFINE_PER_CPU` / `this_cpu_*` abstraction; per-CPU state is accessed via indexed arrays with no NUMA locality guarantee.
- ❌ **Lock-free queues** — No MPSC/SPSC queue primitive for high-throughput cross-CPU data passing without a lock.
- ❌ **Scalable memory allocation** — No NUMA-aware slab or per-core allocation arena; all allocation goes through one global path.

### SMP Validation

- ❌ **Parallel scheduler tests** — No test that spawns threads across cores and verifies correct scheduling decisions and preemption under load.
- ❌ **Race-condition testing** — No tooling (e.g. KCSAN equivalent) to detect data races on shared kernel state at runtime.
- ❌ **Stress testing across cores** — No sustained multi-core workload to surface livelock, starvation, or cache-coherence bugs.
- ❌ **Concurrent filesystem tests** — No test exercising simultaneous VFS operations from multiple cores to validate locking correctness.
- ❌ **Concurrent memory tests** — No parallel allocation/free stress test to validate SMP-safety of the PMM and slab allocator.

### SMP Diagnostics

- ❌ Lock dependency validator
- ❌ Scheduler tracing
- ❌ TLB shootdown tracing
- ❌ Cross-core event tracing

### Future Scalability

- ❌ NUMA awareness
- ❌ NUMA-aware scheduler
- ❌ NUMA-aware memory allocation

### Asynchronous Execution

- ✅ Work queues — `work_queue_t` with spinlock-protected item list; system work queue initialized at boot with dedicated kworker thread.
- ✅ Deferred tasks — `deferred_task_t` one-shot timer API scheduling functions on the system work queue; ~5 ms polling granularity.
- ✅ Kernel worker threads — `kworker` thread processes system work queue; `thread_create()` spawns worker at `THREAD_DEF_PRIO`.

### Exit Criteria

- [ ] Secondary CPUs boot
- [ ] Per-CPU scheduler operational
- [ ] TLB shootdowns work
- [ ] Parallel stress test stable

---

## STAGE 8 — Service Layer `0%`

### IPC

- ❌ **Shared memory** — No shared-memory regions between processes; `mmap` with `MAP_SHARED` not yet implemented.
- ❌ **Message queues** — No POSIX-style or custom message queue primitive.
- ❌ **Event queues** — No epoll/kqueue-style multiplexed event delivery.
- ❌ **Service IPC** — No privileged kernel-to-service communication channel.

### Service Management

- ❌ **Service launcher** — No mechanism to start, stop, or enumerate named services.
- ❌ **Service manager** — No supervisor tracking service liveness or resource consumption.
- ❌ **Dependency handling** — No dependency graph to order service startup.
- ❌ **Restart policies** — No automatic restart on failure or health-check failure.

### System Events

- ❌ **Kernel event bus** — No publish/subscribe bus for kernel-generated events.
- ❌ **Process events** — No notification when processes start, exit, or change state.
- ❌ **Filesystem events** — No inotify-equivalent; no change notification on filesystem objects.
- ❌ **Device events** — No hotplug or device state-change events exposed to userspace.
- ❌ **Memory pressure events** — No low-memory notification to userspace applications.
- ❌ **User activity events** — No input-idle or session-state event source.

### System Introspection

- ❌ procfs
- ❌ sysfs
- ❌ Process information export
- ❌ Device information export
- ❌ Runtime kernel statistics export

### Exit Criteria

- [ ] IPC message send/receive round-trip works
- [ ] Shared memory region visible across two processes
- [ ] Named service starts, stops, and restarts on failure
- [ ] Kernel event bus delivers a process-exit event to a subscriber
- [ ] procfs exposes process list
- [ ] sysfs exposes at least one device entry

---

## STAGE 9 — Quality, Testing and Scalability `~20%`

### Tooling

- ❌ **GDB stub** — No remote debugging protocol; cannot attach GDB over serial or network.
- ❌ **Trace viewer** — No structured event trace capture or visualisation.
- ❌ **Module loading** — Kernel is monolithic; no loadable kernel module infrastructure.
- ❌ **Test harness** — No in-kernel unit test framework or userspace test runner.
- ❌ **Crash dumps** — No core dump on kernel panic or process crash; debug info is lost on reboot.

### Testing

- 🟡 **Stress testing** — Manual stress scripts exercise the allocator and scheduler under load; no automated repeat or CI integration.
- ❌ **Fuzzing** — No syscall fuzzer or filesystem fuzzer in place.
- ❌ **Regression tests** — No automated suite to catch regressions across builds.
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

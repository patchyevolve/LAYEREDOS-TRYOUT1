# OPERtur/TRY1 OS — Implementation Progress

Legend: ✅ Implemented | 🟡 Partial | ❌ Not implemented

## Core boot and hardware
- ✅ Stable boot path and higher-half mapping
- ✅ GDT, IDT, TSS, and interrupt stubs
- ✅ PIC/APIC interrupt routing (xAPIC enabled, LINT0=ExtINT for PIC passthrough, APIC timer active)
- ✅ UART/serial console
- ✅ PIT and HPET timer (HPET 100 MHz detected, nanosecond precision timekeeping, APIC timer for scheduling)
- ✅ Basic CPU feature detection (SMAP, MWAIT via CPUID)
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
- ✅ Syscall gateway (int 0x80, 34 syscalls: SYS_MMAP/SYS_MUNMAP/SYS_MPROTECT added)
- ✅ Syscall argument validation (user-range + mapped + SMAP checks)
- ❌ Userspace ABI stability (no formal ABI)
- 🟡 Userspace C library (two tiny embedded programs, getcwd/chdir/dup2 wrappers needed)
- ✅ ELF loading (ELF64, PT_LOAD segments, PT_INTERP, NX support)
- ✅ Dynamic linker/loader (ld.so: ET_DYN PIE loaded by kernel via PT_INTERP, ELF parsing, symbol resolution, RELA/PLT relocations, shared library loading via mmap, aux-vector setup, init/fini arrays)
- ✅ Standard file descriptor table (32 FD slots, pre-allocated 0/1/2)
- 🟡 Standard input/output/error wiring (fd 0/1/2 reserved, sys_read/sys_write for UART)
- ✅ Minimal init process (pid 1, runs shell)
- ✅ Basic service launcher (/etc/rc startup script sourced by shell at boot)

## Storage and filesystem
- ✅ PCI bus enumeration (config space, bus scanning, bridge recursion)
- ✅ ATA/AHCI/NVMe driver (ATA PIO, identify, read/write sectors, LBA48)
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
- ✅ tmpfs (RAM-backed filesystem mounted at /tmp, all VFS ops)
- ✅ devfs (device filesystem mounted at /dev: null, zero, random, full)
- ✅ Journaling/WAL — crash recovery via write-ahead log (DATA+COMMIT entries, recover replays committed transactions)
- ✅ File permissions and ownership (mode enforced on open-for-write, default 0644, chmod shell command)
- ✅ File locking (advisory, lock/unlock shell commands, SFS inode flag)

## Terminal and shell
- 🟡 Real TTY/terminal layer (kernel/tty.c + kernel/tty.h: line discipline, canonical/raw mode, echo, signal generation. Ctrl-C SIGINT, Ctrl-Z SIGTSTP, Ctrl-\ SIGQUIT, Ctrl-D EOF. ISR feeds TTY raw buffer; ISR-level signal detection delivers signals to fg pgid immediately via work queue. Process groups with `pgid` field. Shell `cmd_run`/`cmd_fg` set `fg_pgid`. /dev/ttyS0 in devfs. No termios ioctl, no PTY, no SIGTTIN/SIGTTOU.)
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
- ❌ Network stack
- ❌ NIC driver support
- ❌ Ethernet frame handling
- ❌ ARP
- ❌ IPv4
- ❌ IPv6
- ❌ ICMP
- ❌ UDP
- ❌ TCP
- ❌ DNS resolver
- ❌ Sockets API
- ❌ TLS support
- ❌ Network namespaces or isolation
- ❌ DHCP client
- ❌ NTP client

## Security and isolation
- ❌ Capability system
- ❌ Permission model
- ❌ User/group identity
- ❌ Sandboxing
- ❌ Audit logging
- ✅ Kernel/user memory isolation enforcement (user range limited, SMAP/SMEP)
- ✅ Read-only kernel text (cleared PAGE_WRITE on .text and .rodata after boot)
- ✅ NX / non-executable memory (NX bit on user stack/data segments)
- ✅ Basic signals (SIGTERM/SIGKILL/SIGSTOP/SIGCONT/SIGTSTP, sys_kill/sys_sigaction, default actions)
- 🟡 ASLR (PIE binaries load at random base using RDTSC; fixed-address EXEC binaries deterministic)
- ❌ Secure boot chain
- ❌ Signed binaries
- ❌ Syscall filtering or policy hooks
- ❌ Privilege separation for services
- ❌ Secure IPC
- ❌ Secure random subsystem
- ❌ Hashing framework (no kernel crypto hash primitives)
- ❌ Kernel crypto primitives

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
- ❌ Backup/restore support

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
- ❌ Test harnesses (no automated test framework)
- ✅ Boot-time self-tests (ELF load, user process spawn)
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
- ❌ Regression tests (no automated regression suite)
- ❌ Benchmarking tools
- ✅ Code style consistency (uniform style across kernel)
- ❌ NUMA awareness (single NUMA domain assumed throughout)
- ❌ NUMA-aware scheduler (no topology-aware thread placement)
- ❌ NUMA-aware memory allocation (no per-node allocator)

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

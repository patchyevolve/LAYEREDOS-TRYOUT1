# Kernel Invariants — Read Before Any Kernel Change

## Stack & Memory Safety
- [ ] **Stack allocation < 16 KB**: No single automatic variable may exceed ~8 KB (kernel stacks are 16 KB total). Heap-allocate or use static buffers for anything larger.
- [ ] **No tcp_conn_t on stack**: `sizeof(tcp_conn_t) > 2 KB` — allocate from `tcp_conns[]` array only.
- [ ] **No udp_endpoint_t on stack**: Contains `udp_dgram_t queue[16]` (~24 KB total) — pass flat params to `udp_bind_endpoint()` instead.
- [ ] **Page_owner invariant**: Any page in a per-CPU PMM cache must have `page_owner = PAGE_OWNER_CACHE` (bitmap SET, counted as in-use). Steal path must update it.

## Concurrency & Locking
- [ ] **Lock ordering documented**: Every new lock must declare its ordering relative to existing locks. Check DEPENDENCIES.md.
- [ ] **No lock held across `tcp_send_pkt()`**: `tcp_lock` must be released before calling `tcp_send_pkt()` — NDP/ARP resolution may re-enter TCP via `eth_rx_poll()`.
- [ ] **`sched_queue_lock` → `join_queue.lock` is forbidden**: This ordering would ABBA with the existing `join_queue.lock` → `sched_queue_lock` path in `thread_join`. Use `sched_kill_thread` pattern (release `sched_queue_lock` before `sched_wake`).
- [ ] **Per-CPU data accessed only via `sched_pcp()`**: Never index `per_cpu_data[]` with a cached cpu_id across a lock drop — the thread may have migrated. Re-read `smp_cpu_id()` after re-acquiring locks.
- [ ] **RDTSC-based timers must use per-CPU state**: Shared `static uint64_t last_xxx` values race when multiple vCPUs call `schedule()` concurrently. Store in `per_cpu_data_t`.
- [ ] **`apic_write()` / `apic_read()` require mapped MMIO**: if `apic_x2apic == 0` and `apic_mmio == NULL`, these are silent no-ops. Guarantee `apic_init()` succeeded before any APIC ICR/SVR/timer access.

## Scheduler Invariants
- [ ] **Every THREAD_READY thread is in exactly one run queue**: `rq_counts[cpu][prio]` must match the actual linked-list walk of `rq_heads[cpu][prio]`. The mid-list removal bug violated this — 29 count vs 13 walkable.
- [ ] **No thread in >1 run queue**: `sched_remove_thread_locked()` must be called before `sched_add_thread_locked()` for the same thread. Double-queuing corrupts both queues.
- [ ] **`pick_next()` never returns idle when runnable threads exist**: `schedule()` must re-add `current_thread` to the run queue BEFORE calling `pick_next()`.
- [ ] **`next->cr3` must be a valid PML4**: GP fault at `sched.c:619` means `pick_next()` returned a thread with a corrupted or freed page table. Caused by shared run queue corruption on TCG.
- [ ] **`sched_reap_zombies()` skips threads with `join_queue.count > 0`**: Otherwise the TCB is freed while a joiner holds `join_queue.lock` reading `exit_code`.
- [ ] **`check_sleepers()` moves state to `THREAD_READY` after acquiring `sched_queue_lock`**: Moving state before the lock means the thread could be in `THREAD_READY` with no run queue entry if lock contention fails.

## Process / VMM Invariants
- [ ] **PML4[255] = PID stamp**: `process_create()` writes `proc->pid` to PML4[255]. `process_exit()` clears it before `vmm_free_user_pages()`. Must be exactly (pid) to avoid misinterpretation as a present page-table entry.
- [ ] **No PML4 self-reference**: `vmm_free_user_pages()` must skip entries that point to the PML4 page itself (infinite loop otherwise).
- [ ] **`kernel_cr3` (PML4 of kernel) never freed**: Kernel page tables live forever. Only user PML4s are freed on process exit.
- [ ] **VMA list protected by `proc->vma_lock`**: All VMA add/find/remove/split/cleanup must hold this lock. No lock-free traversal.

## Network Stack Invariants
- [ ] **`tcp_find_conn()` matches all 5 tuple fields**: Missing IPv6 match caused the same connection to match different 5-tuples. Both `AF_INET` and `AF_INET6` paths must set `match`.
- [ ] **`recv_data` must not be a pointer into `eth_rx_poll()`'s stack buffer**: That buffer (`buf[1518]`) is invalidated when `eth_rx_poll()` returns. Copy to `recv_buf[TCP_MSS]` in `tcp_input()`.
- [ ] **`tcp_conn_close()` sends FIN but does NOT destroy**: `tcp_conn_destroy()` is separate — called only after the FIN handshake completes naturally. Premature destroy leaks RST.
- [ ] **`udp_endpoint_dequeue()` with `timeout_ms=0` must check queue at least once**: Use `do { } while()` loop, not `for()` or `while()`.
- [ ] **Socket fd table protected by `sockets_lock`**: `sock_register`, `sock_unregister`, `sock_lookup` must all acquire it. Concurrent fd-table races cause UAF.
- [ ] **`af_to_user()` must translate kernel AF (4/6) → POSIX AF (2/10)**: All getsockname/getpeername/recvfrom paths must translate before writing to userspace.
- [ ] **`tcp_sock_close` + `sock_unregister` before `socket_release`**: Otherwise `net_sockets[fd]` is a dangling pointer and a subsequent `sys_sendto` to the same fd crashes.

## APIC / SMP Invariants
- [ ] **APIC MMIO mapped before any APIC register write**: `apic_init()` maps `apic_mmio` at `0xFFFFFFFFFFFFE000`. Without this, `apic_write()` and `apic_read()` are no-ops.
- [ ] **x2APIC vs xAPIC path**: On TCG, x2APIC is NOT enabled (MSR bit 10=0), so all accesses go through MMIO. On bare metal/KVM, x2APIC may be enabled and uses MSRs. `smp_cpu_id()` handles both.
- [ ] **IPI delivery requires APIC enabled on target**: If the target CPU's APIC is disabled (SVR not written), IPIs are lost. `apic_enable()` must be called on every CPU.
- [ ] **`smp_cpu_id()` must return unique values**: The APIC ID → CPU index mapping in `cpu_info[]` must be one-to-one. Two CPUs with the same ID alias onto one `per_cpu_data[]` entry, corrupting the scheduler.

## DMA / Device Invariants
- [ ] **E1000 RDT must keep N-2 buffers ahead of RDH**: Writing RDT = `(rx_cur + N - 2) % N` guarantees QEMU always has descriptors available. Writing RDT = `idx` (the consumed descriptor) causes QEMU to stop delivering packets.
- [ ] **PIO transfer must hold `ata_global_lock` for the entire sequence**: IRQ wait disables interrupts; a concurrent PIO transfer on another drive corrupts the shared ATA command block.
- [ ] **`eth_rx_poll()` may be called from multiple contexts**: Both the NIC poll thread and blocking socket APIs call it. `eth_lock` and per-NIC TX locks must protect all shared state.

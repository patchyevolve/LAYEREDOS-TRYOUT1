# Kernel Dependency & Callgraph Map

> **Read this file before editing any module.** Cross-module dependencies,
> shared state, and lock ordering are documented here to prevent the AI
> from introducing invariant violations.

---

## Legend

| Field | Meaning |
|-------|---------|
| **imports** | Modules whose functions/globals this file uses |
| **exports** | Functions/globals exposed to other modules |
| **locks (defined)** | Lock variables defined in this file |
| **locks (uses)** | Lock variables from other modules that this file acquires |
| **order** | Lock acquisition ordering (→ means "before") |
| **shared** | Global state modified by this file (extern or static) |
| **inits after** | Initialization ordering requirement |

---

## Core — hal.c

| Field | |
|-------|-|
| **imports** | sched.h, process.h, hpet.h, apic.h, vmm.h, pmm.h, vma.h, smp.h, watchdog.h, eventbus.h, syscall.h |
| **exports** | `hal_timer_get_ticks()`, `hal_timer_get_ns()`, `hal_timer_get_hz()`, `hal_save_irq()`, `hal_restore_irq()`, `hal_irq_register()`, `hal_set_kernel_stack()`, `hal_reboot()`, `hal_poweroff()`, `hal_is_qemu_tcg()`, `hal_get_mmap_entries()`, `hal_idt_reload()`, `hal_init_cpu_gdt_tss()`, `hal_init()`, `tsc_khz`, `timer_hz`, `timer_ticks` |
| **locks (defined)** | `static spinlock_t irq_reg_lock` |
| **locks (uses)** | none (irq_reg_lock alone) |
| **order** | `irq_reg_lock` alone |
| **shared** | `irq_handlers[48]`, `timer_ticks`, `timer_hz`, `tsc_khz`, `idt[256]`, `gdt[8]`, `tss` |
| **notes** | Panic/fault handlers call into sched.c (watchdog, check_sleepers) and vma.c (page fault handler). `tsc_khz` is read by many callers to compute RDTSC intervals. |

---

## Core — sched.c

| Field | |
|-------|-|
| **imports** | pmm.h, vmm.h, kmalloc.h, hal.h, watchdog.h, process.h, smp.h, apic.h, rcu.h, eventbus.h |
| **exports** | `thread_create()`, `thread_exit()`, `thread_yield()`, `thread_sleep()`, `thread_join()`, `sched_add_thread()`, `sched_remove_thread()`, `schedule()`, `sched_init()`, `sched_init_ap()`, `sched_pcp()`, `pick_next()`, `sched_timer_tick()`, `check_sleepers()`, `sched_reap_zombies()`, `sched_balance_push()`, `sched_find_thread()`, `sched_kill_thread()`, `sched_set_priority()`, `sched_set_thread_affinity()`, `sched_block()`, `sched_wake()`, `sched_place_thread()`, `sched_get_switch_count()`, `current_thread` (via getter), `next_thread_id`, `sched_queue_lock` |
| **locks (defined)** | `static spinlock_t all_threads_lock`, `spinlock_t sched_queue_lock` |
| **locks (uses)** | none acquired from other modules |
| **order** | `all_threads_lock` → `sched_queue_lock` (reap_zombies); `sched_queue_lock` → (must NOT → `join_queue.lock`) |
| **shared** | `next_thread_id`, `current_thread_global`, `all_threads_head/tail/count`, `sched_switch_count`, `sched_yield_count`, `sched_running`, `kernel_cr3`; per-CPU: `rq_heads/tails/counts[256]`, `priority_bitmap[4]`, `need_reschedule` |
| **inits after** | pmm_init, vmm_init, hal_init, smp_init |
| **notes** | ABBA fixed: `sched_kill_thread` releases `sched_queue_lock` before `sched_wake()` (which acquires `join_queue.lock`). Never re-introduce the old ordering. |

---

## PMM — pmm.c

| Field | |
|-------|-|
| **imports** | hal.h, eventbus.h, sched.h, process.h, work.h, kmalloc.h, sync.h, acpi.h, vmm.h |
| **exports** | `pmm_alloc_page()`, `pmm_alloc_pages()`, `pmm_alloc_node_pages()`, `pmm_free_page()`, `pmm_free_pages()`, `pmm_total_pages()`, `pmm_free_pages_count()`, `pmm_numa_init()`, `pmm_init()`, `pmm_current_node()`, `pmm_page_owner()` |
| **locks (defined)** | `static spinlock_t pmm_global_lock`, `static spinlock_t cpu_cache_lock[MAX_CPUS]` |
| **locks (uses)** | acquires `cpu_cache_lock[my_cpu]` first (fast path), then optionally `pmm_global_lock` (slow path) |
| **order** | `cpu_cache_lock[my]` → `pmm_global_lock` (fast→slow). Steal: `pmm_global_lock` → `cpu_cache_lock[victim]` → `cpu_cache_lock[my]`. **Potential ABBA**: if two CPUs steal from each other simultaneously, opposite ordering. Deadlock avoided by try-lock in steal path. |
| **shared** | `node_free_lists[MAX_NUMA_NODES]`, `node_free_counts[]`, `total_page_count`, `list_free_count`, `total_memory`, `bitmap_base`, `bitmap_pages`, `used_bitmap`, `page_owner[]`, `cpu_cache[MAX_CPUS][32]`, `cpu_cache_count[MAX_CPUS]`, `oom_scheduled`, `oom_work_item` |
| **inits after** | hal_init, vmm_init, acpi_init |
| **notes** | Per-CPU cache pages have bitmap SET (counted in-use). `page_owner = PAGE_OWNER_CACHE` for cached pages. `pmm_alloc_pages()` flushes all caches before scanning bitmap. |

---

## VMM — vmm.c

| Field | |
|-------|-|
| **imports** | pmm.h, hal.h, smp.h, acpi.h |
| **exports** | `vmm_map_page()`, `vmm_unmap_page()`, `vmm_walk_pagetable()`, `vmm_free_user_pages()`, `vmm_duplicate_user_pages()`, `vmm_get_kernel_pml4()`, `vmm_flush_tlb_page()`, `vmm_init()` |
| **locks (defined)** | none |
| **locks (uses)** | calls `smp_tlb_shootdown_safe()` which acquires `tlb_lock` in smp.c |
| **order** | n/a (lock-free beyond tlb_shootdown) |
| **shared** | `kernel_pml4` (static — kernel's top-level page table) |
| **inits after** | pmm_init |
| **notes** | Page table walks are lock-free — only the current CPU's view matters. TLB shootdowns are best-effort on KVM (IPI may not deliver to remote CPUs). |

---

## KMALLOC — kmalloc.c

| Field | |
|-------|-|
| **imports** | pmm.h, vmm.h, sync.h, smp.h |
| **exports** | `kmalloc()`, `kfree()`, `kmalloc_init()`, `kmalloc_compact()`, `kmalloc_bytes_used()`, `kmalloc_bytes_total()` |
| **locks (defined)** | `static spinlock_t kmalloc_lock`, `static spinlock_t kmag_lock[MAX_CPUS]` |
| **locks (uses)** | none from other modules |
| **order** | `kmalloc_lock` → `kmag_lock[cpu]` (slow path: flush_half/flush). `kmag_lock[cpu]` alone (fast path: magazine push/pop). |
| **shared** | `slabs[SLAB_COUNT]`, `kmalloc_bytes_used`, `kmalloc_bytes_total`, `kmag_ptrs[MAX_CPUS][SLAB_COUNT][KMAG_SIZE]`, `kmag_count[MAX_CPUS][SLAB_COUNT]` |
| **inits after** | pmm_init, vmm_init |

---

## Process — process.c

| Field | |
|-------|-|
| **imports** | pmm.h, vmm.h, sched.h, sync.h, elf.h, hal.h, vfs.h, kmalloc.h, vma.h, net_ns.h |
| **exports** | `process_create()`, `process_exec()`, `process_exit()`, `process_fork()`, `process_kill()`, `process_setpgid()`, `process_find_by_pid()`, `process_init()` |
| **locks (defined)** | `static spinlock_t process_lock` |
| **locks (uses)** | `proc->vma_lock` (embedded), `proc->pt_lock` (embedded), `proc->signal_lock` (embedded) |
| **order** | `process_lock` alone (protects process_table). Per-process: `proc->vma_lock` → `proc->pt_lock` (in vma_unmap_range). No cross-ordering documented with sched_queue_lock. |
| **shared** | `process_table[MAX_PROCESSES]`, `next_pid`, `process_list` |
| **inits after** | pmm_init, vmm_init, sched_init, kmalloc_init |
| **notes** | PID stamp at PML4[255] must be cleared before `vmm_free_user_pages`. Process exit calls `vma_cleanup()`, `net_ns_release()`, `vfs_close_all()`. |

---

## APIC — apic.c

| Field | |
|-------|-|
| **imports** | hal.h, pmm.h, vmm.h, hpet.h, smp.h |
| **exports** | `apic_init()`, `apic_enable()`, `apic_disable()`, `apic_eoi()`, `apic_timer_init()`, `apic_send_ipi()`, `apic_send_ipi_self()`, `apic_send_ipi_allbutself()`, `apic_send_nmi_allbutself()`, `apic_send_init_ipi()`, `apic_send_sipi_ipi()`, `apic_read()`, `apic_write()`, `apic_disable_pic()`, `apic_ioapic_init()`, `apic_present`, `apic_x2apic`, `apic_id` |
| **locks (defined)** | none |
| **locks (uses)** | none |
| **order** | n/a |
| **shared** | `apic_present` (non-static), `apic_x2apic` (non-static), `apic_id` (non-static), `apic_mmio` (static) |
| **inits after** | pmm_init, vmm_init, hpet_init |
| **notes** | CRITICAL: `apic_write()` is a no-op if `apic_x2apic == 0 && apic_mmio == NULL` (return early). Without x2APIC, all IPI and SVR writes silently fail. `smp_cpu_id()` reads MSR 0x802 when x2APIC is enabled, or `apic_read(APIC_REG_ID)` otherwise—the latter requires `apic_mmio != NULL`. |

---

## ACPI — acpi.c

| Field | |
|-------|-|
| **imports** | vmm.h, pmm.h |
| **exports** | `acpi_init()`, `acpi_get_cpu_node()`, `acpi_available`, `cpu_info[MAX_CPUS]`, `nr_cpus`, `io_apic_count`, `io_apics[]`, `iso_count`, `isos[]`, `numa_available`, `numa_node_count`, `numa_distance[][]`, `numa_cpu_to_node[]` |
| **locks (defined)** | none |
| **shared** | ALL exports are globals — heavily shared across SMP, APIC, PMM, NUMA consumers |
| **inits after** | vmm_init (to map ACPI tables), pmm_init |
| **notes** | RSDP scan covers 0–1MB. MADT parsed for CPU/IO-APIC/ISO entries. SRAT parsed for NUMA distances. Sets `cpu_info[].apic_id` which `smp_cpu_id()` searches. |

---

## SMP — smp.c

| Field | |
|-------|-|
| **imports** | pmm.h, vmm.h, apic.h, acpi.h, sched.h, hal.h, hpet.h, kmalloc.h |
| **exports** | `smp_init()`, `smp_init_aps()`, `smp_cpu_id()`, `smp_send_reschedule()`, `smp_tlb_shootdown()`, `smp_tlb_shootdown_safe()`, `smp_cpu_offline()`, `smp_cpu_online()`, `smp_enabled`, `per_cpu_data[MAX_CPUS]`, `cpu_state[MAX_CPUS]` |
| **locks (defined)** | `static spinlock_t tlb_lock` |
| **locks (uses)** | `sched_queue_lock` (from sched.c — for thread migration during CPU offline) |
| **order** | `tlb_lock` alone; `sched_queue_lock` alone (acquired in smp_handle_offline IPI handler) |
| **shared** | `smp_enabled`, `smp_flags`, `smp_ipi_works`, `ap_ipi_test_counter`, `per_cpu_data[MAX_CPUS]`, `ap_ready_count`, `cpu_state[MAX_CPUS]` |
| **inits after** | hal_init, pmm_init, vmm_init, acpi_init, apic_init, sched_init |
| **notes** | `smp_cpu_id()` reads IA32_APIC_BASE MSR bit 10 to choose x2APIC vs xAPIC path. AP entry passes `per_cpu_data_t* pcp` directly (does not call `smp_cpu_id()`). IPI delivery status wait skipped for cross-CPU IPIs (never clears on KVM). |

---

## Network Namespace — net_ns.c

| Field | |
|-------|-|
| **imports** | sched.h, process.h, kmalloc.h, sync.h |
| **exports** | `net_ns_alloc()`, `net_ns_release()`, `net_ns_copy()`, `net_ns_init()`, `init_net_ns` |
| **locks (defined)** | none (initializes `sockets_lock`, `tcp_lock`, `udp_lock`, `arp_lock`, `ndp_lock`, `route_lock`, `igmp_lock` via spinlock_init in net_ns_t) |
| **shared** | `init_net_ns` — the default namespace (global) |
| **inits after** | kmalloc_init, sched_init |
| **notes** | Every per-namespace lock (tcp_lock, udp_lock, etc.) is a field of `net_ns_t`. Macros like `get_current_ns()->tcp_lock` expand to namespace-specific instances. Dynamic namespaces allocate arrays on heap; init_net_ns uses static globals. |

---

## Socket Layer — net.c

| Field | |
|-------|-|
| **imports** | tcp.h, udp.h, ipv4.h, ipv6.h, igmp.h, unix.h, process.h, sched.h, kmalloc.h, net_ns.h |
| **exports** | `sock_socket()`, `sock_bind()`, `sock_connect()`, `sock_listen()`, `sock_accept()`, `sock_send()`, `sock_recv()`, `sock_sendto()`, `sock_recvfrom()`, `sock_setsockopt()`, `sock_getsockopt()`, `sock_getsockname()`, `sock_getpeername()`, `sock_poll()`, `sock_close()`, `socket_alloc()`, `socket_release()`, `sock_register()`, `sock_unregister()`, `sock_lookup()`, `net_init()` |
| **locks (defined)** | uses `sockets_lock` from current namespace (net_ns_t field) |
| **order** | `sockets_lock` → tcp/udp ops (which may acquire per-protocol locks). `sockets_lock` → `unix_named_lock` (via AF_UNIX bind). |
| **shared** | per-namespace: `net_sockets[32]`, `sockets_lock`, `socket_count` |
| **inits after** | tcp_init, udp_init, route_init, arp_init, ndp_init, unix_init |
| **notes** | `sock_close()` calls `sock_unregister(s->fd)` before `socket_release()` to prevent dangling pointer in fd table. `af_to_user()` translates kernel AF values (4,6) to POSIX values (2,10). |

---

## TCP — tcp.c

| Field | |
|-------|-|
| **imports** | ipv4.h, ipv6.h, route.h, nic.h, ndp.h, eth.h, hal.h, sched.h, sync.h, net_ns.h |
| **exports** | `tcp_conn_connect()`, `tcp_conn_listen()`, `tcp_conn_accept()`, `tcp_conn_close()`, `tcp_conn_destroy()`, `tcp_conn_bind()`, `tcp_send()`, `tcp_input()`, `tcp_handle_common()`, `tcp_tick()`, `tcp_init()`, `tcp_find_conn()`, `tcp_conn_get_state()` |
| **locks (defined)** | uses `tcp_lock` from current namespace |
| **order** | CRITICAL: `tcp_lock` must be RELEASED before `tcp_send_pkt()` (which may re-enter TCP via NDP resolution → eth_rx_poll → tcp_input). Deadlock if held. |
| **shared** | per-namespace: `tcp_conns[MAX_TCP_CONNS]`, `tcp_ephemeral_port`, `tcp_lock` |
| **inits after** | ipv4_init, ipv6_init, route_init, ndp_init |
| **notes** | `tcp_find_conn()` must compare all 5 tuple fields. `tcp_input()` copies payload to `recv_buf[TCP_MSS]` rather than pointing into caller's stack. RST handler does NOT set `used=0` (slot stays allocated for tuple matching). |

---

## UDP — udp.c

| Field | |
|-------|-|
| **imports** | ipv4.h, ipv6.h, route.h, nic.h, eth.h, sched.h, hpet.h, sync.h, net_ns.h |
| **exports** | `udp_init()`, `udp_input()`, `udp_sendto()`, `udp_bind_endpoint()`, `udp_unbind_endpoint()`, `udp_endpoint_dequeue()` |
| **locks (defined)** | uses `udp_lock` from current namespace |
| **order** | `udp_lock` alone |
| **shared** | per-namespace: `udp_endpoints[MAX_UDP_ENDPOINTS]`, `udp_lock` |
| **inits after** | ipv4_init, ipv6_init, route_init |
| **notes** | `udp_bind_endpoint()` takes flat params (not a pointer to a stack-allocated endpoint — the struct is ~24 KB). `udp_endpoint_dequeue()` uses `do {} while()` for timeout=0 case. |

---

## ARP — arp.c

| Field | |
|-------|-|
| **imports** | eth.h, nic.h, ipv4.h, hal.h, sched.h, sync.h, net_ns.h, work.h |
| **exports** | `arp_init()`, `arp_resolve()`, `arp_find_cache()`, `arp_set()`, `arp_del()`, `arp_handler()` |
| **locks (defined)** | `static spinlock_t arp_reply_lock` (for deferred reply ring buffer) |
| **locks (uses)** | `arp_lock` from current namespace |
| **order** | `arp_reply_lock` alone; `arp_lock` (net_ns) alone |
| **shared** | `arp_cache` (via net_ns), `arp_reply_queue[8]`, `arp_reply_head/tail`, `arp_reply_work_item` |
| **inits after** | eth_init, ipv4_init, route_init |

---

## NDP — ndp.c

| Field | |
|-------|-|
| **imports** | icmpv6.h, nic.h, eth.h, hal.h, sched.h, sync.h, net_ns.h |
| **exports** | `ndp_init()`, `ndp_cache_lookup()`, `ndp_cache_update()`, `ndp_resolve()`, `ndp_cache_clear()` |
| **locks (defined)** | uses `ndp_lock` from current namespace |
| **order** | `ndp_lock` alone |
| **shared** | per-namespace: `ndp_cache[]`, `ndp_lock` |
| **inits after** | ipv6_init, icmpv6_init, eth_init |

---

## IPv4 — ipv4.c

| Field | |
|-------|-|
| **imports** | eth.h, arp.h, route.h, nic.h, igmp.h, net_ns.h |
| **exports** | `ipv4_init()`, `ipv4_send()`, `ipv4_send_from()`, `ipv4_dispatch_pkt()`, `ipv4_register_handler()`, `ipv4_set_addr()`, `ipv4_get_addr()`, `ipv4_set_addr_prefix()`, `ipv4_get_prefix_len()`, `ipv4_addr_equal()` |
| **locks (defined)** | none |
| **shared** | per-namespace: `OUR_IPV4`, `ipv4_initialized`, `ipv4_next_id`, `ipv4_dispatch` |
| **inits after** | eth_init, arp_init, route_init |

---

## IPv6 — ipv6.c

| Field | |
|-------|-|
| **imports** | eth.h, ndp.h, route.h, nic.h, e1000.h, icmpv6.h, net_ns.h |
| **exports** | `ipv6_init()`, `ipv6_send()`, `ipv6_dispatch_pkt()`, `ipv6_register_handler()`, `ipv6_set_lladdr()`, `ipv6_get_lladdr()`, `ipv6_set_global_addr()`, `ipv6_has_global_addr()`, `ipv6_addr_equal()`, `ipv6_mcast_join()`, `ipv6_mcast_leave()`, `ipv6_mcast_is_member()`, `ipv6_mcast_update_mta()`, `ipv6_mcast_report_all()` |
| **locks (defined)** | none |
| **shared** | per-namespace: `GLOBAL_IPV6`, `LINK_LOCAL`, `ipv6_initialized`, `ipv6_dispatch`, `ipv6_mcast_groups` |
| **inits after** | eth_init, ndp_init, route_init, icmpv6_init |

---

## ICMP — icmp.c

| Field | |
|-------|-|
| **imports** | ipv4.h, eth.h, hal.h, sched.h |
| **exports** | `icmp_init()`, `icmp_handler()`, `icmp_ping4()` |
| **locks (defined)** | none (uses per-endpoint sync via sched_block/wake) |
| **shared** | `icmp_next_id`, `icmp_ping_wait` |
| **inits after** | ipv4_init |

---

## ICMPv6 — icmpv6.c

| Field | |
|-------|-|
| **imports** | ipv6.h, ndp.h, eth.h, nic.h, hal.h, sched.h, net_ns.h |
| **exports** | `icmpv6_init()`, `icmpv6_handler()`, `icmpv6_send_ns()`, `icmpv6_send_na()`, `icmpv6_send_rs()`, `icmpv6_send_echo_reply()`, `icmpv6_ping6()`, `icmpv6_set_ra_callback()`, `mldv1_send_report()`, `mldv1_send_done()` |
| **locks (defined)** | none |
| **shared** | per-namespace: `icmpv6_ra_callback` |
| **inits after** | ipv6_init, eth_init |

---

## IGMP — igmp.c

| Field | |
|-------|-|
| **imports** | ipv4.h, e1000.h, route.h, net_ns.h, sync.h |
| **exports** | `igmp_init()`, `igmp_v4_handler()`, `igmp_mcast_join()`, `igmp_mcast_leave()` |
| **locks (defined)** | uses `igmp_lock` from current namespace |
| **order** | `igmp_lock` alone |
| **shared** | per-namespace: `igmp_groups`, `igmp_group_used`, `igmp_lock` |
| **inits after** | ipv4_init, route_init |

---

## E1000 — e1000.c

| Field | |
|-------|-|
| **imports** | nic.h, eth.h, pci.h, pmm.h, vmm.h, hal.h, apic.h |
| **exports** | `e1000_init()`, `e1000_send()`, `e1000_poll()`, `e1000_mta_set()` (populates `nic_t nic`) |
| **locks (defined)** | `static volatile int e1000_tx_lock` (raw flag, not spinlock_t) |
| **shared** | `e1000_regs`, `e1000_tx_ring/rx_ring`, `e1000_tx_bufs[]`, `e1000_rx_bufs[]`, `e1000_tx_head/tail`, `e1000_rx_cur`, `e1000_irq`, `e1000_present`, `e1000_tx_lock`, `e1000_next_vaddr` |
| **inits after** | pci_init, pmm_init, vmm_init, eth_init, apic_init |
| **notes** | `e1000_tx_lock` is NOT a proper spinlock_t — it's a raw volatile int with `__sync_bool_compare_and_swap`. Doesn't disable interrupts. |

---

## VFS — vfs.c

| Field | |
|-------|-|
| **imports** | security.h, hal.h, process.h, sched.h, sync.h, kmalloc.h |
| **exports** | `vfs_open()`, `vfs_close()`, `vfs_read()`, `vfs_write()`, `vfs_stat()`, `vfs_readdir()`, `vfs_mkdir()`, `vfs_rmdir()`, `vfs_unlink()`, `vfs_rename()`, `vfs_chdir()`, `vfs_getcwd()`, `vfs_dup2()`, `vfs_ioctl()`, `vfs_fcntl()`, `vfs_readlink()`, `vfs_writelink()`, `vfs_symlink()`, `vfs_link()`, `vfs_truncate()`, `vfs_chmod()`, `vfs_fchmod()`, `vfs_access()`, `vfs_init()`, `vfs_register_fs()`, `vfs_close_all()` |
| **locks (defined)** | `spinlock_t vfs_global_lock` (non-static, extern in vfs.h) |
| **order** | `vfs_global_lock` alone (one global lock for entire VFS — SMP contention point) |
| **shared** | `kernel_fd_table[VFS_MAX_FDS]`, `dentry_cache[64]`, `dentry_cache_age`, `dentry_cache_count`, `root_node`, `mount_table[8]`, `mount_count` |
| **inits after** | kmalloc_init, pmm_init |
| **notes** | Single global lock protects all VFS operations. `vfs_read`/`vfs_write` enforce fd access mode (O_WRONLY/O_RDONLY). `vfs_open` does lookup+fill under spinlock (TOCTOU fix). |

---

## Block — block.c

| Field | |
|-------|-|
| **imports** | sync.h |
| **exports** | `block_read()`, `block_write()`, `block_register()`, `block_count()`, `block_try_sync()` |
| **locks (defined)** | `static spinlock_t cache_lock` |
| **order** | `cache_lock` alone |
| **shared** | `block_devs[MAX_BLOCK_DEVICES]`, `block_dev_count`, `block_cache[64]`, `cache_hits/misses/writes/clock` |
| **inits after** | kmalloc_init |

---

## SFS — sfs.c

| Field | |
|-------|-|
| **imports** | block.h, journal.h, fsck.h, pmm.h, kmalloc.h, hal.h, sync.h |
| **exports** | `sfs_mount()`, `sfs_format()`, `sfs_unmount()`, SFS VFS operations (sfs_open/read/write/...) |
| **locks (defined)** | uses `fs_lock` (spinlock_t) and `bmap_lock` (mutex_t) from `sfs_fs_t` |
| **order** | `fs_lock` → `bmap_lock` (metadata lock before block bitmap mutex) |
| **notes** | Cross-block dirent: SFS_BLOCK_SIZE=512, sizeof(sfs_dirent_t)=68 → 7.5 entries/block. Every 8th entry spans 2 blocks. |

---

## VMA — vma.c

| Field | |
|-------|-|
| **imports** | process.h, pmm.h, vmm.h, kmalloc.h, hal.h |
| **exports** | `vma_add()`, `vma_find()`, `vma_remove()`, `vma_split()`, `vma_handle_fault()`, `vma_cleanup()`, `vma_dup()`, `vma_unmap_range()` |
| **locks (defined)** | uses `proc->vma_lock` and `proc->pt_lock` from process_t |
| **order** | `proc->vma_lock` → `proc->pt_lock` (nested in vma_unmap_range for MAP_FIXED replacement) |
| **inits after** | kmalloc_init, pmm_init |

---

## RCU — rcu.c

| Field | |
|-------|-|
| **imports** | sched.h, smp.h, sync.h, hal.h |
| **exports** | `rcu_init()`, `rcu_read_lock()`, `rcu_read_unlock()`, `rcu_quiescent_state()`, `call_rcu()`, `synchronize_rcu()` |
| **locks (defined)** | `static spinlock_t rcu_lock` |
| **order** | `rcu_lock` alone |
| **shared** | `rcu_pending`, `rcu_gp_list`, `rcu_gp_ctr`, `rcu_qs_ctr[MAX_CPUS]`, `rcu_gp_active`, `rcu_thread` |
| **notes** | RCU kthread pinned to CPU 0 (cpu_affinity=1) to prevent migration race. |

---

## init order (main.c)

| Step | Call |
|------|------|
| 1 | hal_init |
| 2 | acpi_init |
| 3 | hpet_init |
| 4 | apic_init, apic_ioapic_init |
| 5 | pci_init |
| 6 | e1000_init |
| 7 | smp_init, smp_init_aps |
| 8 | pmm_init |
| 9 | vmm_init |
| 10 | sched_init |
| 11 | work_queue_init |
| 12 | eventbus_init |
| 13 | kmalloc_init |
| 14 | process_init |
| 15 | vfs_init |
| 16 | tty_init |
| 17 | storage init (ramdisk, ata, ahci, nvme, gpt, sfs) |
| 18 | network init (eth, route, arp, ndp, ipv4, ipv6, icmp, icmpv6, igmp, tcp, udp, net, dns, ntp) |
| 19 | dhcp_configure, slaac_init/configure |
| 20 | swap_init, watchdog_init, rcu_init |
| 21 | secure_boot_init, random_init |

---

## Lock ordering summary

```
vfs_global_lock (alone)
cache_lock (alone)
process_lock (alone)
sched_queue_lock (alone after ABBA fix — never → join_queue.lock)
all_threads_lock → sched_queue_lock

rcu_lock (alone)
eventbus_lock (alone)
pci_lock (alone)
eth_lock (alone)
veth_lock (alone)

pmm_global_lock → cpu_cache_lock[victim]
kmalloc_lock → kmag_lock[cpu]
vfs_global_lock (alone)

tcp_lock (alone — released before tcp_send_pkt)
udp_lock (alone)
arp_lock (alone)
ndp_lock (alone)
igmp_lock (alone)
route_lock (alone)
sockets_lock (alone)
unix_named_lock (alone)

fs_lock → bmap_lock (SFS)
mmio_lock (alone)
ctrl->lock (AHCI/NVMe per-controller)

process (per-process):
  proc->vma_lock → proc->pt_lock
  proc->signal_lock (alone)

PPY/TTY:
  raw_lock → state_lock (TTY)
  pty_pool_lock → m_lock / s_raw_lock (PTY)
```

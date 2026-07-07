# Boot Call Tree — Complete Function Trace

## Entry: `kmain()` [main.c:133]

```
kmain(magic, mb_info)
├── hal_get_mem_size(mb_info)                      [hal.c:418]
├── hal_init(mb_info)                              [hal.c:608]
├── pmm_init(mem_size, mb_info)                    [pmm.c:436]
├── vmm_init()                                     [vmm.c:239]
├── vmm_protect_kernel_text()                      [vmm.c:253]
├── kmalloc_init()                                 [kmalloc.c:39]
├── hpet_init()                                    [hpet.c:50]
│   └── [if OK] hpet_timer_init()                  [hpet.c:131]
├── swap_init()                                    [swap.c:13]
├── apic_init()                                    [apic.c:81]
│   └── [if OK] apic_enable()                      [apic.c:130]
│       apic_timer_init(1000)                      [apic.c:163]
│       apic_disable_pic()                         [apic.c:185]
├── syscall_init()                                 [syscall.c:2143]
├── acpi_init(mb_info)                             [acpi.c:55]
├── smp_init()                                     [smp.c:73]
├── apic_ioapic_init()                             [apic.c:306]
├── sched_init()                                   [sched.c:793]
├── smp_init_aps()                                 [smp.c:200]
├── process_init()                                 [process.c:30]
├── work_init()                                    [work.c:118]
├── keyboard_init()                                [keyboard.c:77]
├── ata_init()                                     [ata.c:143]
├── pci_init()                                     [pci.c:110]
├── vfs_init()                                     [vfs.c:63]
├── tty_init()                                     [tty.c:46]
├── pty_init()                                     [pty.c:115]
├── ramdisk_init()                                 [ramdisk.c:60]
├── ramdisk_add_file(...) × 11                     [ramdisk.c:77]
├── ramdisk_blk_init()                             [ramdisk_blk.c:28]
├── gpt_scan()                                     [gpt.c:44]
├── nic_init()                                     [e1000.c:420]
├── net_ns_init()                                  [net_ns.c:83]
├── eth_init()                                     [eth.c:15]
├── arp_init()                                     [arp.c:228]
├── ndp_init()                                     [ndp.c:153]
├── route_init()                                   [route.c:37]
├── ipv4_init()                                    [ipv4.c:167]
├── ipv6_init()                                    [ipv6.c:189]
├── icmpv4_init()                                  [icmp.c:115]
├── icmpv6_init()                                  [icmpv6.c:352]
├── udp_init()                                     [udp.c:319]
├── tcp_init()                                     [tcp.c:683]
├── net_init()                                     [net.c:871]
├── dns_init()                                     [dns.c:218]
├── route_clear()                                  [route.c:8]
├── dhcp_configure()                               [dhcp.c:219]
│   └── [if fail] fallback IPv4 static setup
├── slaac_init()                                   [slaac.c:55]
├── slaac_configure()                              [slaac.c:61]
├── ntp_init()                                     [ntp.c:44]
│   (if tests enabled:)
├── net_self_test()                                [net_test.c:283]
├── storage_self_test()                            [storage_test.c:522]
├── kernel_self_test()                             [kernel_test.c:885]
├── sfs_format(block_find("ramdisk"))              [sfs.c:1204]
├── sfs_mount(block_find("ramdisk"))               [sfs.c:1257]
│   (if tests enabled:)
├── sfs_self_test()                                [sfs_test.c:252]
├── process_self_test()                            [process_test.c:118]
├── security_self_test()                           [security_test.c:857]
├── vfs_create("/hello.elf", 0)                    [vfs.c:559]
├── vfs_open("/hello.elf", O_WRONLY)               [vfs.c:402]
├── vfs_write(fd, data, sz)                       [vfs.c:518]
├── vfs_close(fd)                                  [vfs.c:467]
│   (repeated for: cat.elf, hello-c.elf, tcp_echo.elf,
│    udp_echo.elf, thread_test.elf, ld.so, libdyn.so,
│    hello-dyn.elf, version.txt, welcome.txt)
├── vfs_mkdir("/etc")                              [vfs.c:595]
├── vfs_create("/etc/rc", 0)                       [vfs.c:559]
├── tmpfs_mount(&tmpfs_vfs)                        [tmpfs.c:371]
├── devfs_mount()                                  [devfs.c:205]
├── eventbus_init()                                [eventbus.c:30]
├── watchdog_init()                                [watchdog.c:150]
├── hal_irq_register(0, watchdog_timer_handler, 0) [hal.c:333]
├── hal_uart_rx_init()                             [hal.c:375]
├── shell_init()                                   [shell.c:2503]
├── boot_complete = 1
├── hal_enable_irqs()
├── hal_sti()
├── vfs_find("/")                                  [vfs.c:350]
├── process_create("init-user", 1) + process_exec  [process.c:49,129]
├── process_create("test", 0) + elf_load           [process.c:49, elf.c:92]
├── thread_create(nic_poll_thread, ...)             [sched.c:495]
│   └── sched_add_thread(np)                       [sched.c:141]
├── tcp_listen(AF_INET, 80, cb)                    [tcp.c:429]
├── tcp_listen(AF_INET6, 9, cb)                    [tcp.c:429]
├── thread_create(udp_echo_server, ...)             [sched.c:495]
│   └── sched_add_thread(uth)                      [sched.c:141]
├── ndp_cache_update(ip, mac)                      [ndp.c:28]
└── shell_run()                                    [shell.c:2511]
```

---

## Layer 1 — HAL & Hardware Abstraction

### `hal_get_mem_size()` [hal.c:418]
```
hal_get_mem_size(mb_info_phys)
└── [leaf: reads multiboot mmap struct fields]
```

### `hal_init()` [hal.c:608]
```
hal_init(mb_info)
├── kmemset()                                      [leaf]
├── spinlock_init(&hal_lock, "hal")                [sync.c:6]
├── gdt_init()                                     [hal.c:613]
│   ├── gdt_set_entry() × 7                         [hal.c:170]
│   ├── gdt_set_tss()                              [hal.c:176]
│   ├── kmemset(&tss, 0, 104)                      [leaf]
│   └── asm volatile("lgdt; movw ...; ltr")         [leaf — assembly]
├── idt_init()                                     [hal.c:614]
│   ├── idt_set_gate() × 48                         [hal.c:268]
│   └── asm volatile("lidt")                       [leaf]
├── pic_remap()                                    [hal.c:615]
│   ├── outb(0x20, 0x11) × 2                       [leaf — I/O port]
│   ├── outb(0xA0, 0x11) × 2                      [leaf]
│   ├── outb(0x21, 0x20)                           [leaf]
│   ├── outb(0xA1, 0x28)                           [leaf]
│   ├── outb(0x21, 0x04)                           [leaf]
│   ├── outb(0xA1, 0x02)                           [leaf]
│   ├── outb(0x21, 0x01)                           [leaf]
│   ├── outb(0xA1, 0x01)                           [leaf]
│   ├── io_wait() × 2                               [hal.c:86 — outb(0x80,0)]
│   └── outb(0x21, 0x00) × 2                       [leaf]
├── uart_init()                                    [hal.c:616]
│   └── outb() × 6                                  [leaf]
├── hal_timer_init(1000)                           [hal.c:617]
│   └── outb() × 3                                  [leaf — PIT programming]
├── hal_enable_irqs()                              [hal.c:618]
│   ├── inb(0x21)                                  [leaf]
│   ├── inb(0xA1)                                  [leaf]
│   └── outb() × 2                                  [leaf]
├── hal_sti()                                      [leaf — asm("sti")]
├── calibrate_tsc()                                [hal.c:620]
│   ├── rdtsc()                                    [leaf — asm("rdtsc")]
│   └── asm volatile("pause")                      [leaf]
├── cpuid asm                                      [leaf]
├── asm volatile("mov %%cr4, ...")                 [leaf]
├── hal_smap_enabled()                             [hal.c:633 — cpuid]
├── asm volatile("mov ..., %%cr4")                 [leaf]
└── kputs()                                        [leaf]
```

---

## Layer 4 — PMM (Physical Memory Manager)

### `pmm_init()` [pmm.c:436]
```
pmm_init(mem_size, mb_info)
├── spinlock_init(&pmm_global_lock, "pmm")         [leaf]
├── spinlock_init(cpu_cache_lock[i]) × 64           [leaf]
├── kmemset(bitmap, 0, size)                       [leaf]
├── bitmap_clear() for all pages                    [leaf]
├── pmm_mark_region_used() × 7                      [pmm.c:222]
│   └── bitmap_set() / bitmap_test()                [leaf]
├── parse_mb_mmap(mb_info)                         [pmm.c:471]
│   ├── hal_get_mmap_entries()                     [hal.c:412]
│   └── add_region_to_free_list() × N              [pmm.c:407]
│       └── free_page_t->next = ...                 [leaf]
└── kprintf()                                       [leaf]
```

### `pmm_alloc_page()` [pmm.c:239] (leaf — common path)
```
pmm_alloc_page()
├── smp_cpu_id()                                   [smp.c:24]
│   ├── rdmsr IA32_APIC_BASE                       [leaf]
│   └── apic_read(APIC_REG_ID)                     [leaf — MMIO read]
├── spinlock_acquire(&cpu_cache_lock[cpu])          [sync.c:12]
│   └── [chain: hal_save_irq → __sync_lock_test_and_set → pause]
├── cache_try_pop()                                [pmm.c:85]
├── [if empty: spinlock_acquire(pmm_global_lock)]
│   ├── cache_steal()                              [pmm.c:167]
│   ├── cache_refill()                             [pmm.c:144]
│   └── pmm_oom_kill()                              [pmm.c:257] (rare)
├── spinlock_release()                              [sync.c:35]
├── kmemset(page, 0, PAGE_SIZE)                    [leaf]
└── return phys_addr
```

### `pmm_alloc_pages(n)` [pmm.c:295] (leaf)
```
pmm_alloc_pages(n)
├── spinlock_acquire(&pmm_global_lock)              [leaf]
├── [flush ALL per-CPU caches: bitmap_clear × n]    [leaf]
├── [scan bitmap for n contiguous free pages]        [leaf]
├── [mark pages used in bitmap]                     [leaf]
├── [prune free list]                               [leaf]
├── spinlock_release(&pmm_global_lock)              [leaf]
└── kmemset(pages) × N                              [leaf]
```

---

## Layer 4 — VMM (Virtual Memory Manager)

### `vmm_init()` [vmm.c:239]
```
vmm_init()
├── asm volatile("mov %%cr3, %0")                   [leaf]
└── kprintf()                                       [leaf]
```

### `vmm_protect_kernel_text()` [vmm.c:253]
```
vmm_protect_kernel_text()
├── vmm_walk_pagetable()                            [vmm.c:51]
├── asm volatile("invlpg")                          [leaf]
└── kprintf()                                       [leaf]
```

### `vmm_map_page(pml4, vaddr, paddr, flags)` [vmm.c:78] (leaf — common utility)
```
vmm_map_page(pml4, vaddr, paddr, flags)
├── vmm_get_kernel_pml4()                           [vmm.c:49 — return global]
├── get_entry(pml4v, vaddr, 1)                      [vmm.c:20]
│   ├── [walk PML4[4]->PDPT[?]->PD[?]]
│   └── vmm_alloc_page_table()                      [vmm.c:14]
│       ├── pmm_alloc_page()                        [leaf]
│       └── kmemset(page, 0, 4096)                  [leaf]
├── *pt_entry = paddr | flags
└── vmm_flush_tlb_page(vaddr)                      [vmm.c:87]
    ├── asm volatile("invlpg")                      [leaf]
    └── smp_tlb_shootdown_safe()                    [smp.c:337]
        ├── asm volatile("invlpg")                  [leaf]
        └── [if smp_ipi_works: apic_send_ipi() — rare]
```

---

## Kernel Heap (Slab Allocator)

### `kmalloc_init()` [kmalloc.c:39]
```
kmalloc_init()
├── kmemset(slab_classes, 0, sizeof)
└── spinlock_init(&kmalloc_lock, "kmalloc")
```

### `kmalloc(size)` [kmalloc.c:71] (leaf — common utility)
```
kmalloc(size)
├── spinlock_acquire(&kmalloc_lock)                 [leaf]
├── slab_index(size) → class                        [leaf]
├── [if slab page empty: slab_new_page(cl)]
│   ├── pmm_alloc_page()                            [leaf]
│   ├── kmemset(page bitmap)                        [leaf]
│   └── [init free list within page]                [leaf]
├── [pop from free list]                            [leaf]
├── spinlock_release(&kmalloc_lock)                 [leaf]
└── return ptr
```

### `kfree(ptr)` [kmalloc.c:114] (leaf)
```
kfree(ptr)
├── spinlock_acquire(&kmalloc_lock)                 [leaf]
├── [return to slab free list]                      [leaf]
├── [if slab page empty: pmm_free_page()]           [leaf]
└── spinlock_release(&kmalloc_lock)                 [leaf]
```

---

## HPET Timer

### `hpet_init()` [hpet.c:50]
```
hpet_init()
├── hal_is_qemu_tcg()                               [hal.c:96 — cpuid]
├── vmm_map_page(HPET_BASE_VADDR, HPET_MMIO, ...)   [vmm.c:78]
├── hpet_read(HPET_GEN_CAP)                         [leaf — MMIO read]
├── hpet_write(HPET_MAIN_CNT, 0)                   [leaf]
└── kprintf()                                       [leaf]
```

### `hpet_timer_init()` [hpet.c:131]
```
hpet_timer_init()
├── hpet_disable()                                  [hpet.c:137]
├── hpet_read(HPET_GEN_CAP)                         [leaf]
├── hpet_write(HPET_T0_CONF, ...)                   [leaf]
├── hpet_write(HPET_T0_COMP, cmp)                   [leaf]
├── hpet_enable()                                   [hpet.c:94]
├── kprintf()                                       [leaf]
├── [registers hpet_timer_isr in hal_irq_register]  [hal.c:333]
```

---

## Swap

### `swap_init()` [swap.c:13]
```
swap_init()
├── pmm_alloc_pages(4)                              [pmm.c:295 — 4 pages for swap]
├── spinlock_init(&swap_lock, "swap")               [leaf]
├── kmemset(swap_bitmap, 0xFF, pages * PAGE_SIZE)   [leaf]
├── [clear bits for reserved slots]                  [leaf]
└── kprintf()                                       [leaf]
```

---

## APIC / Interrupt Controller

### `apic_init()` [apic.c:81]
```
apic_init()
├── cpuid_has_apic()                                [leaf — cpuid]
├── apic_read_msr(IA32_APIC_BASE)                   [leaf — rdmsr]
├── vmm_map_page(APIC_VADDR, APIC_MMIO_PHYS, ...)   [vmm.c:78]
├── apic_write_msr(IA32_APIC_BASE, new_base)         [leaf — wrmsr]
├── apic_read(APIC_REG_VERSION)                     [leaf — MMIO read]
├── apic_read(APIC_REG_ID)                          [leaf]
└── kprintf()                                       [leaf]
```

### `apic_enable()` [apic.c:130]
```
apic_enable()
├── apic_read(APIC_REG_SVR)                         [leaf]
└── apic_write(APIC_REG_SVR, svr_ptr)               [leaf]
```

### `apic_timer_init(hz)` [apic.c:163]
```
apic_timer_init(1000)
├── apic_write(APIC_REG_TIMER_DIV, 0x03)            [leaf]
├── apic_write(APIC_REG_LVT_TIMER, lvt)             [leaf]
├── apic_calibrate_init_count()                     [apic.c:176]
│   ├── apic_write(APIC_REG_TIMER_INIT, probe)     [leaf]
│   ├── hpet_ns() × 2                                [hpet.c:120]
│   └── apic_read(APIC_REG_TIMER_CUR)               [leaf]
├── apic_write(APIC_REG_TIMER_INIT, init_count)     [leaf]
└── kprintf()                                       [leaf]
```

### `apic_disable_pic()` [apic.c:185]
```
apic_disable_pic()
├── apic_read(APIC_REG_LVT_LINT0)                   [leaf]
├── apic_write(APIC_REG_LVT_LINT0, masked)          [leaf]
├── apic_read(APIC_REG_LVT_LINT1)                   [leaf]
├── apic_write(APIC_REG_LVT_LINT1, masked)          [leaf]
├── inb(0x21)                                       [leaf]
├── outb(0x21, mask)                                [leaf]
└── kprintf()                                       [leaf]
```

### `apic_ioapic_init()` [apic.c:306]
```
apic_ioapic_init()
├── vmm_map_page(IOAPIC_VADDR, IOAPIC_BASE, ...)    [vmm.c:78]
├── ioapic_read(IOAPIC_VADDR, IOAPIC_VER)           [leaf — MMIO]
├── vmm_unmap_page(IOAPIC_VADDR)                    [vmm.c:91]
├── kprintf()                                       [leaf]
├── [for each ISO: ioapic_write() × 2]              [leaf]
└── kprintf()                                       [leaf]
```

---

## Syscall

### `syscall_init()` [syscall.c:2143]
```
syscall_init()
└── kprintf()                                       [leaf]
```

---

## ACPI

### `acpi_init(mb_info)` [acpi.c:55]
```
acpi_init(mb_info)
├── acpi_find_rsdp()                                [acpi.c:56]
│   └── [scan physical 0-1MB for RSDP signature]     [leaf]
├── acpi_checksum(rsdp, length)                     [leaf — XOR sum loop]
└── kprintf()                                       [leaf]
```

---

## SMP

### `smp_init()` [smp.c:73]
```
smp_init()
├── acpi_scan_cpus()                                [acpi.c:235]
│   ├── acpi_parse_madt()                           [acpi.c:236]
│   │   ├── acpi_find_rsdp()                        [acpi.c:100]
│   │   ├── acpi_map_table(phys) → vmm_map_page()    [vmm.c:78]
│   │   ├── acpi_checksum()                          [leaf]
│   │   └── [MADT entry parsing loop — APIC/NMI/X2APIC] [leaf]
│   └── [fallback: rdmsr for BSP APIC ID]            [leaf]
├── smp_alloc_per_cpu(cpu) × N                      [smp.c:87]
│   ├── pmm_alloc_pages(npages)                     [pmm.c:295]
│   ├── PHYS_TO_VIRT(phys)                           [leaf]
│   ├── kmemset(data, 0, npages*4096)               [leaf]
│   └── kmemset(priority_bitmap, 0, 32)             [leaf]
├── kprintf()                                       [leaf]
└── smp_enabled = 1
```

### `smp_init_aps()` [smp.c:200]
```
smp_init_aps()
├── PHYS_TO_VIRT(_trampoline_start)                  [leaf]
├── kmemcpy(tramp_dst, tramp_src, 0x200)             [leaf — copy to 0x4000]
├── spinlock_init(&tlb_lock, "tlb")                  [leaf]
├── asm volatile("mov %%cr3, %0")                   [leaf]
├── [for each AP:]
│   ├── [set up trampoline data at 0x4200]           [leaf]
│   ├── mb()                                         [barrier.h:9 — mfence]
│   ├── apic_send_init_ipi(apic_id)                 [smp.c:245]
│   │   └── [ICR write via apic_write × 2 + wait]   [leaf]
│   ├── thread_sleep(10)                            [sched.c:569]
│   │   └── [set wakeup_tick + thread_yield → schedule()]
│   ├── apic_send_sipi_ipi(apic_id, 0x04)           [smp.c:249]
│   │   └── [ICR write + wait]                      [leaf]
│   ├── thread_sleep(200)                           [sched.c:569]
│   ├── [check ap_ready_count, retry if needed]
│   └── kprintf()                                   [leaf]
├── kprintf()                                       [leaf]
└── smp_test_ipi()                                  [smp.c:272]
    ├── smp_cpu_id()                                 [leaf]
    ├── mb()                                         [leaf]
    ├── apic_send_ipi_self(IPI_VEC_RESCHEDULE)       [apic.c:227]
    ├── apic_read(APIC_REG_ICR0)                     [leaf]
    ├── smp_handle_tlb_shootdown()                   [smp.c:358]
    │   ├── asm volatile("mov %%cr3, ...")            [leaf]
    │   └── per_cpu_data->tlb_flush_pending = 0       [leaf]
    └── kprintf()                                   [leaf]
```

---

## Scheduler & Threading

### `sched_init()` [sched.c:793]
```
sched_init()
├── spinlock_init(&all_threads_lock, "all_threads")  [leaf]
├── spinlock_init(&sched_queue_lock, "sched_queue")  [leaf]
├── asm volatile("mov %%cr3, %0")                   [leaf]
├── thread_create(idle_thread, NULL, IDLE_PRIO, "idle") [sched.c:495]
│   ├── pmm_alloc_page()                            [leaf]
│   ├── PHYS_TO_VIRT → kmemset(tcb, 0, 4096)        [leaf]
│   ├── pmm_alloc_pages(4)                          [leaf]
│   ├── kmemset(kstack, 0, 16384)                   [leaf]
│   ├── [set up stack: thread_trampoline, func, arg] [leaf]
│   ├── wait_queue_init(&tcb->join_queue)            [sched.c:589]
│   │   └── spinlock_init(&wq->lock, "wq")          [leaf]
│   ├── kstrncpy(name, ...)                          [leaf]
│   └── all_threads_add(tcb)                        [sched.c:31]
│       ├── spinlock_acquire(&all_threads_lock)      [leaf]
│       └── spinlock_release(&all_threads_lock)      [leaf]
├── thread_create(NULL, NULL, DEF_PRIO, "init")     [sched.c:495 — same pattern]
├── set_current_thread(init_thread)                  [leaf — per-CPU ptr write]
├── hal_set_kernel_stack(init->kernel_stack + size)  [leaf — write TSS rsp[0]]
└── kprintf()                                       [leaf]
```

### `sched_init_ap()` [sched.c:777]
```
sched_init_ap()
├── thread_create(idle_thread, NULL, IDLE_PRIO, "idle") [sched.c:495]
├── per_cpu_data[cpu]->idle_thread = (void*)idle
├── per_cpu_data[cpu]->cpu_thread = (void*)idle
└── return ERR_OK
```

### `sched_add_thread(t)` [sched.c:141] (leaf — common path)
```
sched_add_thread(t)
└── sched_add_thread_to_cpu(t, smp_cpu_id())         [sched.c:109]
    ├── spinlock_acquire(&sched_queue_lock)          [leaf]
    ├── [enqueue t to per-CPU run queue by priority] [leaf]
    └── spinlock_release(&sched_queue_lock)          [leaf]
```

### `sched_remove_thread(t)` [sched.c:148] (leaf)
```
sched_remove_thread(t)
├── [guard: null check, queue consistency check]
├── spinlock_acquire(&sched_queue_lock)              [leaf]
├── [unlink from per-CPU run queue]                  [leaf]
└── spinlock_release(&sched_queue_lock)              [leaf]
```

### `pick_next()` [sched.c:368] (leaf)
```
pick_next()
├── bitmap_find_highest(pcp)                         [sched.c:192]
├── [if empty: sched_steal_thread() → return idle]   [sched.c:205]
│   └── sched_steal_thread()                         [sched.c:205]
│       ├── spinlock_acquire/release(sched_queue_lock) [leaf]
│       └── smp_send_reschedule(target)              [sched.c:276]
│           ├── per_cpu_data[cpu]->need_reschedule=1  [leaf]
│           └── apic_send_ipi(cpu)                   [apic.c:234]
├── if try_acquire(sched_queue_lock):                 [leaf]
│   ├── [dequeue highest-prio from per-CPU list]     [leaf]
│   └── spinlock_release(sched_queue_lock)           [leaf]
├── [else: sched_steal_thread() or return idle]
└── return thread
```

### `schedule()` [sched.c:422] (leaf — called from ISR)
```
schedule()
├── pcp->need_reschedule = 0                          [leaf]
├── hal_save_irq()                                    [leaf]
├── next = pick_next()                                [sched.c:368]
├── [if next==current: re-add and return]
├── [if current RUNNING: set READY, add to queue via try_acquire]
├── [switch CR3 if next has user page tables]
├── set_current_thread(next)                          [leaf]
├── next->state = THREAD_RUNNING                      [leaf]
├── hal_set_kernel_stack(next's stack)                [leaf]
└── switch_context(&old, &current_thread)             [ctx.S — assembly]
```

### `thread_sleep(ms)` [sched.c:569] (leaf)
```
thread_sleep(ms)
├── hal_timer_get_ticks()                             [leaf — read timer_ticks var]
├── hal_timer_get_hz()                                [leaf — read timer_hz var]
├── current_thread->wakeup_tick = now + delta         [leaf]
├── current_thread->state = THREAD_SLEEPING            [leaf]
└── thread_yield()                                    [sched.c:487]
    └── schedule()                                    [sched.c:422]
```

### `thread_create(func, arg, prio, name)` [sched.c:495] (leaf)
```
thread_create(func, arg, prio, name)
├── pmm_alloc_page() for TCB                          [leaf]
├── kmemset(tcb, 0, 4096)                             [leaf]
├── pmm_alloc_pages(4) for stack                      [leaf]
├── kmemset(kstack, 0, 16384)                         [leaf]
├── [set up stack with thread_trampoline, func, arg]
├── [set tcb fields: id, rsp, cr3, state=CYCLED,
│    priority, cpu_affinity=0xFF, kernel_stack, ...]
├── wait_queue_init(&tcb->join_queue)                 [sched.c:589]
└── all_threads_add(tcb)                              [sched.c:31]
```

---

## Process Manager

### `process_init()` [process.c:30]
```
process_init()
├── kmemset(process_table, 0, sizeof)                 [leaf]
├── list_init(&process_list)                          [leaf]
├── spinlock_init(&process_lock, "process")           [leaf]
├── asm volatile("mov %%cr3, %0")                    [leaf]
├── process_create("init", 0)                        [process.c:49]
│   ├── spinlock_acquire(&process_lock)               [leaf]
│   ├── [find free pid slot]                          [leaf]
│   ├── kmemset(proc, 0, sizeof(process_t))           [leaf]
│   ├── kstrncpy(proc->name, "init")                  [leaf]
│   ├── list_init(&proc->threads)                     [leaf]
│   ├── spinlock_init(&proc->signal_lock)             [leaf]
│   ├── spinlock_init(&proc->vma_lock)                [leaf]
│   ├── pmm_alloc_page() for PML4                     [leaf]
│   ├── kmemset(pml4, 0, 4096)                       [leaf]
│   ├── [copy kernel PML4 entries 256..511]           [leaf]
│   ├── pml4[255] = pid                               [leaf]
│   ├── list_add_tail(&process_list, ...)             [leaf]
│   └── spinlock_release(&process_lock)               [leaf]
└── kprintf()                                         [leaf]
```

### `process_create(name, ppid)` [process.c:49] (leaf)
```
process_create(name, ppid)
├── spinlock_acquire(&process_lock)
├── [find free slot in process_table]
├── kmemset(proc, 0, sizeof)
├── kstrncpy(proc->name, name)
├── list_init(&proc->threads)
├── spinlock_init(&proc->signal_lock)
├── spinlock_init(&proc->vma_lock)
├── pmm_alloc_page() for PML4
├── kmemset(pml4v, 0, 4096)
├── [copy kernel mappings PML4[256..511]]
├── pml4[255] = pid
├── [if ppid: inherit fd table from parent]
├── list_add_tail(&process_list)
├── spinlock_release(&process_lock)
└── return proc
```

### `process_exec(proc, elf_data, len)` [process.c:129]
```
process_exec(proc, elf_data, len)
├── [scan ELF for PT_INTERP header]
├── elf_load(proc, elf_data, len)                    [elf.c:92]
│   ├── [parse ELF header, PHDRs]
│   ├── [for each PT_LOAD: vmm_map_page(cr3, vaddr, ...)]
│   ├── [set proc->entry_point]
├── [if interpreter: vfs_open(interp) + read + elf_load_fixed at 0x7F000000]
├── pmm_alloc_page() for user stack                   [leaf]
├── [ASLR: random stack pos in 0x60000000 region]
├── vmm_map_page(cr3, vaddr, stack_phys, USER|WRITE) [leaf]
├── [set up auxvec/argv/envp on stack]               [leaf]
├── [secure_boot_check → sha256(elf_data, len)]       [secure_boot.c:16, sha256.c:58]
└── hal_jump_to_user(entry, stack_top)               [leaf — iretq to ring 3]
```

---

## Work Queue

### `work_init()` [work.c:118]
```
work_init()
├── work_queue_init(&system_wq, "system_wq")          [work.c:17]
│   ├── list_init(&wq->items)                        [leaf]
│   └── spinlock_init(&wq->lock, "system_wq")        [leaf]
├── thread_create(work_worker_thread, NULL, DEF_PRIO, "work") [sched.c:495]
├── list_init(&deferred_list)                         [leaf]
├── spinlock_init(&deferred_lock, "deferred")         [leaf]
└── work_queue_schedule(&system_wq, &deferred_item)   [work.c:27]
    ├── spinlock_acquire(&wq->lock)                   [leaf]
    ├── list_add_tail(&item->node, &wq->items)        [leaf]
    └── spinlock_release(&wq->lock)                   [leaf]
```

---

## I/O Subsystem

### `keyboard_init()` [keyboard.c:77]
```
keyboard_init()
├── inb(0x64) × 4                                    [leaf]
├── hal_irq_register(1, keyboard_isr, NULL)          [hal.c:333]
│   ├── spinlock_acquire(&irq_reg_lock)              [leaf]
│   └── spinlock_release(&irq_reg_lock)              [leaf]
└── kprintf()                                         [leaf]
```

### `ata_init()` [ata.c:143]
```
ata_init()
├── kmemset(drives, 0, sizeof)                       [leaf]
├── wait_queue_init() × 4                             [sched.c:589]
├── hal_irq_register() × 4                            [hal.c:333]
├── ata_identify_drive(0..3)                         [ata.c:95]
│   ├── outb() × 4                                    [leaf]
│   ├── inb()                                         [leaf]
│   ├── ata_wait_bsy()                                [ata.c:40 — inb loop]
│   ├── inb() × 4                                     [leaf]
│   └── inw() × 256                                   [leaf — read identify data]
└── kprintf()                                         [leaf]
```

### `pci_init()` [pci.c:110]
```
pci_init()
├── kmemset(pci_devices, 0, sizeof)                  [leaf]
├── pci_scan_bus(0, 0)                               [pci.c:105]
│   └── pci_scan_slot(0..31)                         [pci.c:89]
│       └── pci_scan_function(0..7)                  [pci.c:70]
│           ├── pci_config_read(vendor/device/class/BAR/IRQ)  [pci.c:19]
│           │   ├── spinlock_acquire(&pci_lock)                 [leaf]
│           │   ├── pci_out_config_addr(bus,slot,func,reg)     [pci.c:10 — outl]
│           │   ├── inl()                                      [leaf]
│           │   └── spinlock_release(&pci_lock)                [leaf]
│           ├── [if PCI-PCI bridge: pci_scan_bus(secondary)]
│           └── [store device in pci_devices[]]
├── kprintf() per device                              [leaf]
└── kprintf()                                         [leaf]
```

### `vfs_init()` [vfs.c:63]
```
vfs_init()
├── kmemset(vfs_fd_table, 0, sizeof)                  [leaf]
├── kstrncpy(vfs_root.name, "/")                      [leaf]
├── kmemset(dentry_cache, 0, sizeof)                  [leaf]
└── kprintf()                                         [leaf]
```

### `tty_init()` [tty.c:46]
```
tty_init()
├── spinlock_init() × 2                               [leaf]
├── wait_queue_init() × 2                              [sched.c:589]
├── list_init()                                       [leaf]
├── kmemset()                                         [leaf]
├── kstrncpy()                                        [leaf]
├── vfs_get_fd_table()                                 [vfs.c:26 — trivial]
├── process_find(1)                                   [process.c:466]
│   ├── spinlock_acquire(&process_lock)               [leaf]
│   └── spinlock_release(&process_lock)               [leaf]
├── kmemcpy()                                         [leaf]
└── kprintf()                                         [leaf]
```

### `pty_init()` [pty.c:115]
```
pty_init()
├── kmemset()                                         [leaf]
├── spinlock_init()                                   [leaf]
└── kprintf()                                         [leaf]
```

### `ramdisk_init()` [ramdisk.c:60]
```
ramdisk_init()
├── kmemset(fs_vfs, 0, sizeof)                        [leaf]
├── kstrncpy(name, "ramdisk")                         [leaf]
├── vfs_register_fs(&fs_vfs)                          [vfs.c:79 — pointer store]
└── kprintf()                                         [leaf]
```

### `ramdisk_add_file(name, data, len)` [ramdisk.c:77]
```
ramdisk_add_file(name, data, len)
├── kstrlen(name)                                     [leaf]
├── kstrncpy(file->name, name)                        [leaf]
├── pmm_alloc_page() for file data                     [leaf]
├── PHYS_TO_VIRT(data_phys)                            [leaf]
├── kmemset(data_page, 0, 4096)                       [leaf]
├── kmemcpy(data_page, data, len)                     [leaf]
├── vfs_find("/")                                     [vfs.c:350]
│   └── vfs_find_flags("/", 0)                        [vfs.c:154]
│       ├── vfs_fs_for_path(path)                     [vfs.c:41]
│       │   ├── kstrlen(path)                         [leaf]
│       │   └── kstrncmp(mount->path, path, len)       [leaf]
│       ├── dentry_lookup(parent, name)                [vfs.c:118]
│       │   ├── dentry_hash(name)                     [vfs.c:111 — loop hash]
│       │   └── kstrncmp(cache[i].name, name)          [leaf]
│       └── dentry_add(parent, child)                  [vfs.c:133]
│           ├── dentry_hash(name)                     [leaf]
│           └── kstrlen(name)                         [leaf]
└── [link file to root->children list]                 [leaf]
```

### `ramdisk_blk_init()` [ramdisk_blk.c:28]
```
ramdisk_blk_init()
├── pmm_alloc_pages(RAMDISK_BLK_SIZE / 4096)          [leaf]
├── PHYS_TO_VIRT(phys)                                 [leaf]
├── kmemset(ramdisk_blk_data, 0, size)                [leaf]
├── kstrncpy(bdev.name, "ramdisk")                     [leaf]
├── block_register(&bdev)                              [block.c:189]
│   └── [copy bdev into block_devs[count++]]           [leaf]
└── kprintf()                                          [leaf]
```

### `block_find("ramdisk")` [block.c:203]
```
block_find(name)
└── [linear scan block_devs[] for kstrcmp match]      [leaf]
```

### `gpt_scan()` [gpt.c:44]
```
gpt_scan()
├── block_count(dev)                                   [block.c:196 — leaf]
├── block_get(dev, 0)                                  [block.c:198 — leaf array access]
├── kmalloc(MBR_BUF + GPT_HDR + GPT_ENTRIES)           [kmalloc.c:71]
├── parent->read(MBR, 0, 1)                            [function pointer — leaf]
├── [check MBR signature 0xAA55]
├── parent->read(GPT header, 1, 1)                    [leaf]
├── [verify GPT signature "EFI PART"]
├── parent->read(entries, hdr->entries_lba, hdr->num_entries) [leaf]
├── [for each partition:]
│   ├── format_guid_part_name(guid)                    [gpt.c:24 — char conversion]
│   ├── kstrlen(name)                                  [leaf]
│   ├── kmemcpy(ptn_fields)                            [leaf]
│   └── block_register(&partition_bdev)                [block.c:189]
├── kfree(hdr + entries)                               [kmalloc.c:114]
└── kprintf()                                          [leaf]
```

---

## Network Stack

### `nic_init()` [e1000.c:420]
```
nic_init()
├── kmemset(&nic, 0, sizeof)                          [leaf]
├── pci_device_count()                                [pci.c:133 — leaf]
├── pci_get_device(device_num)                        [pci.c:135 — leaf]
├── [for each E1000 device:]
│   └── e1000_init_nic(dev)                           [e1000.c:140]
│       ├── pci_enable_bus_mastering(dev)              [pci.c:62]
│       │   ├── pci_config_read(dev, PCI_COMMAND)     [pci.c:19]
│       │   └── pci_config_write(dev, PCI_COMMAND, ...) [pci.c:19]
│       ├── vmm_map_page(PHYS_TO_VIRT(BAR0), BAR0, ...) [vmm.c:78]
│       ├── e1000_reset()                             [e1000.c:96]
│       │   ├── e1000_reg_write(E1000_CTRL, RST)     [leaf — MMIO write]
│       │   ├── hal_udelay(1)                         [leaf — TSC spin]
│       │   └── e1000_reg_read(E1000_CTRL)            [leaf — MMIO read]
│       ├── e1000_read_mac()                          [e1000.c:80]
│       │   ├── e1000_reg_read(E1000_RA)              [leaf]
│       │   └── e1000_eeprom_read()                   [e1000.c:66]
│       │       ├── e1000_reg_write(E1000_EECD, ...)  [leaf]
│       │       └── e1000_reg_read(E1000_EERD)        [leaf]
│       ├── e1000_init_rings()                        [e1000.c:109]
│       │   ├── e1000_alloc_page() × E1000_NUM_RX_DESC [e1000.c:48]
│       │   │   ├── pmm_alloc_page()                  [leaf]
│       │   │   ├── vmm_map_page(...)                  [leaf]
│       │   │   └── pmm_free_page()                    [leaf]
│       │   └── [set up TX+RX descriptor rings]        [leaf]
│       ├── e1000_reg_write(E1000_TDBAL/TDLEN/...)    [leaf × 10]
│       └── [e1000_reg_read(E1000_STATUS)]
├── hal_irq_register(E1000_IRQ, e1000_isr, NULL)     [hal.c:333]
├── inb(0x4D0)                                        [leaf]
├── outb(0x4D0, mask)                                 [leaf]
└── kprintf()                                         [leaf]
```

### `net_ns_init()` [net_ns.c:83]
```
net_ns_init()
├── kmemset(&init_net_ns, 0, sizeof)                  [leaf]
├── [copy static arrays to init_net_ns]               [leaf]
└── net_ns_initialized = 1
```

### `eth_init()` [eth.c:15]
```
eth_init()
├── kprintf()                                         [leaf]
└── eth_initialized = 1
```

### `arp_init()` [arp.c:228]
```
arp_init()
├── spinlock_init(&arp_lock, "arp")                   [leaf]
├── eth_register(ETHERTYPE_ARP, arp_handle)             [eth.c:99]
│   └── [store handler in slot by EtherType]
└── kprintf()                                         [leaf]
```

### `ndp_init()` [ndp.c:153]
```
ndp_init()
├── spinlock_init(&ndp_lock, "ndp")                   [leaf]
├── ndp_make_lladdr(ll, mac)                          [ndp.c:100 — leaf]
└── kprintf() × 2                                     [leaf]
```

### `route_init()` [route.c:37]
```
route_init()
├── route_initialized = 1
├── route_add_v4(10.0.2.0/24, gw=0)                   [route.c:48]
│   └── [set route table slot entries]
├── route_add_v4(0.0.0.0/0, gw=10.0.2.2)             [route.c:48]
└── kprintf()
```

### `ipv4_init()` [ipv4.c:167]
```
ipv4_init()
├── eth_register(ETHERTYPE_IPV4, ipv4_eth_handler)    [eth.c:99]
├── igmp_init()                                       [igmp.c:135]
│   ├── ipv4_register_handler(2, igmp_input)           [ipv4.c:30]
│   └── kprintf()
└── kprintf()
```

### `ipv6_init()` [ipv6.c:189]
```
ipv6_init()
├── eth_register(ETHERTYPE_IPV6, ipv6_eth_handler)
├── ndp_make_lladdr(ll, mac)                          [leaf]
└── kprintf()
```

### `icmpv4_init()` [icmp.c:115]
```
icmpv4_init()
├── ipv4_register_handler(1, icmpv4_input)
└── kprintf()
```

### `icmpv6_init()` [icmpv6.c:352]
```
icmpv6_init()
├── ipv6_register_handler(58, icmpv6_input)            [ipv6.c:21]
└── kprintf()
```

### `udp_init()` [udp.c:319]
```
udp_init()
├── spinlock_init(&udp_lock, "udp")
├── ipv4_register_handler(17, udp_input_v4)
├── ipv6_register_handler(17, udp_input_v6)
└── kprintf()
```

### `tcp_init()` [tcp.c:683]
```
tcp_init()
├── spinlock_init(&tcp_lock, "tcp")
├── ipv4_register_handler(6, tcp_input)
├── ipv6_register_handler(6, tcp_input)
└── kprintf()
```

### `net_init()` [net.c:871]
```
net_init()
├── unix_init()                                       [unix.c:16]
│   ├── kmemset(named_sockets, 0, sizeof)
│   └── unix_initialized = 1
├── kprintf()
└── net_initialized = 1
```

### `dns_init()` [dns.c:218]
```
dns_init()
├── kmemset(dns_buf, 0, sizeof)
├── kprintf()
└── dns_initialized = 1
```

### `route_clear()` [route.c:8]
```
route_clear()
└── [for loop: set route_table[i].used = 0]
```

### `dhcp_configure()` [dhcp.c:219]
```
dhcp_configure()
├── udp_bind_endpoint(AF_INET, INADDR_ANY, 68, ...)   [udp.c:97]
│   ├── spinlock_acquire(&udp_lock)
│   ├── [find free endpoint slot]
│   ├── [set af, addr, port, recv_timeout, send_timeout]
│   └── spinlock_release(&udp_lock)
├── udp_find_endpoint(AF_INET, 68)                    [udp.c:155]
├── [DISCOVER:]
│   ├── dhcp_build(DHCP_DISCOVER, ...)                [dhcp.c:119]
│   │   ├── kmemset(frame, 0, sizeof)
│   │   ├── dhcp_put_byte() × N                        [leaf]
│   │   └── dhcp_put_option() × N                     [leaf]
│   ├── udp_sendto(AF_INET, 0xFFFFFFFF, 67, 68, ...)  [udp.c:62]
│   │   ├── [udp header build + checksum]
│   │   └── ipv4_send_from()                          [ipv4.c:45]
│   │       ├── [IPv4 header build + checksum]
│   │       ├── eth_send()                            [eth.c:47]
│   │       │   ├── eth_try_veth()                    [eth.c:37]
│   │       │   └── nic.send() [e1000_send]            [leaf — TX descriptor write]
│   │       ├── route_lookup_v4()                     [route.c:80]
│   │       └── arp_resolve()                         [arp.c:176]
│   │           ├── [ARP cache lookup]
│   │           ├── arp_send_request()                 [arp.c:65]
│   │           └── eth_rx_poll()                     [eth.c:71]
│   └── [wait + recv: udp_endpoint_dequeue(timeout)]   [udp.c:200]
│       ├── eth_rx_poll()                             [eth.c:71]
│       └── [thread_yield on empty]
├── [OFFER → parse → dhcp_parse_options()]
├── [REQUEST:]
│   ├── dhcp_build(DHCP_REQUEST, ...)
│   └── udp_sendto + udp_endpoint_dequeue
├── [ACK → dhcp_parse_options() → get IP/subnet/gw]
├── udp_unbind_endpoint()                             [udp.c:170]
├── ipv4_set_addr(leased_ip)                          [ipv4.c:103]
├── route_add_v4(subnet/24, 0)                        [route.c:48]
├── route_add_v4(0.0.0.0/0, gw)                      [route.c:48]
└── kprintf()
```

### `slaac_init()` [slaac.c:55]
```
slaac_init()
├── icmpv6_set_ra_callback(slaac_on_ra)               [icmpv6.c:18]
└── kprintf()
```

### `slaac_configure()` [slaac.c:61]
```
slaac_configure()
├── icmpv6_send_rs()                                  [icmpv6.c:279]
│   ├── [build Router Solicitation packet]
│   ├── icmpv6_checksum()                             [leaf]
│   └── ipv6_send()                                  [ipv6.c:67]
│       ├── [IPv6 header build]
│       ├── ipv6_has_global_addr()                     [ipv6.c:120]
│       ├── ndp_resolve()                             [ndp.c:126]
│       │   ├── ndp_cache_lookup()                    [ndp.c:80]
│       │   ├── icmpv6_send_ns()                      [icmpv6.c:242]
│       │   │   └── ipv6_send() + eth_send()
│       │   ├── eth_rx_poll()                         [eth.c:71]
│       │   └── thread_sleep()                        [sched.c:569]
│       └── eth_send()                                [eth.c:47]
├── [loop: eth_rx_poll() + thread_yield(), timeout]
├── [on RA: slaac_make_global_addr()]                  [slaac.c:29]
│   └── [EUI-64 from MAC + prefix]
├── ipv6_set_addr(global_addr)                        [ipv6.c:115]
├── route_add_v6(prefix/64, 0)                        [route.c:64]
├── route_add_v6(::/0, ra_source)                     [route.c:64]
└── kprintf()
```

### `ntp_init()` [ntp.c:44]
```
ntp_init()
├── ntp_build_request(packet)                         [ntp.c:23 — leaf]
├── udp_bind_endpoint(AF_INET, INADDR_ANY, 123, ...) [udp.c:97]
├── udp_find_endpoint(AF_INET, 123)                   [udp.c:155]
├── udp_sendto(AF_INET, ntp_server, 123, 123, ...)   [udp.c:62]
│   └── [same path: ipv4_send_from → eth_send]
├── udp_endpoint_dequeue(ep, buf, ..., timeout)       [udp.c:200]
│   ├── eth_rx_poll()                                 [eth.c:71]
│   ├── hpet_ns()                                     [hpet.c:120]
│   └── thread_yield()                                [sched.c:487]
├── ntp_parse_response(buf)                           [ntp.c:29 — leaf]
├── udp_unbind_endpoint()                             [udp.c:170]
├── hal_timer_get_ns()                                [hal.c:356]
│   └── hpet_ns()                                     [hpet.c:120]
├── [compute boot_time = ntp_time - uptime]
└── kprintf()
```

### `eth_rx_poll()` [eth.c:71]
```
eth_rx_poll()
├── nic.poll() [→ e1000_poll]                         [leaf — RX descriptor read]
│   └── [if packet available: read from RX ring]
├── [for each received frame:]
│   ├── [parse Ethernet header (dst, src, type)]
│   └── eth_handlers[type].handler(type, payload)     [callback dispatch]
│       ├── arp_handle()                               [if ARP]
│       ├── ipv4_eth_handler()                         [if IPv4]
│       └── ipv6_eth_handler()                         [if IPv6]
└── [continue until no more packets]
```

---

## VFS Operations (Post-Boot)

### `vfs_create(path, mode)` [vfs.c:559]
```
vfs_create(path, mode)
├── [parse path: find last '/', get parent dir]
├── vfs_find(parent_path)                              [vfs.c:350]
│   └── vfs_find_flags()                              [vfs.c:154]
│       ├── vfs_fs_for_path()                         [vfs.c:41]
│       ├── dentry_lookup()                           [vfs.c:118]
│       │   ├── dentry_hash(name)                     [vfs.c:111]
│       │   └── kstrncmp(cache[i].name, name)          [leaf]
│       ├── dentry_add(child)                          [vfs.c:133]
│       │   ├── dentry_hash(name)                     [leaf]
│       │   └── kstrlen(name)                         [leaf]
│       └── fs->ops->readdir()                         [callback]
├── kmemcpy(name_copy, name, len)                     [leaf]
├── kstrlen(name)                                     [leaf]
└── parent->fs->ops->create(parent, name, mode)       [→ sfs_vfs_create]
```

### `vfs_open(path, flags)` [vfs.c:402]
```
vfs_open(path, flags)
├── [if O_CREAT: vfs_stat → vfs_create if not found]
├── vfs_find(path)                                    [vfs.c:350]
├── vfs_access_check(node, flags)                     [vfs.c:360]
├── [spinlock: find free fd slot]
├── node->fs->ops->open(node)                         [→ sfs_vfs_open]
├── [set fd table: node, offset=0, flags, refcount++]
└── return fd
```

### `vfs_write(fd, buf, len)` [vfs.c:518]
```
vfs_write(fd, buf, len)
├── [enforce O_RDONLY rejection]
├── vfs_get_fd_table()                                 [vfs.c:26 — trivial]
├── [if O_APPEND: seek to end]
└── node->fs->ops->write(node, buf, count, offset)    [→ sfs_vfs_write]
```

### `vfs_close(fd)` [vfs.c:467]
```
vfs_close(fd)
├── [spinlock: validate fd, get node, set used=0]
├── node->fs->ops->close(node)                        [→ sfs_vfs_close]
├── __sync_fetch_and_sub(&node->refcount, 1)
└── [if last ref && dynamic: kfree(node)]
```

### `vfs_find(path)` [vfs.c:350]
```
vfs_find(path)
└── vfs_find_flags(path, 0)                           [vfs.c:154]
    ├── vfs_fs_for_path(path)                         [vfs.c:41]
    ├── dentry_lookup(parent, name)                   [vfs.c:118]
    │   ├── dentry_hash(name)                         [vfs.c:111]
    │   └── kstrncmp(cache[i].name, name)             [leaf]
    ├── dentry_add(child)                             [vfs.c:133]
    │   ├── dentry_hash(name)                         [leaf]
    │   └── kstrlen(name)                             [leaf]
    ├── fs->ops->readdir(dir, &entry)                  [callback]
    └── fs->ops->readlink(node, buf, sz)               [callback]
```

### `vfs_mkdir(path)` [vfs.c:595]
```
vfs_mkdir(path)
└── vfs_create(path, 1)                               [vfs.c:559 — with is_dir=1]
```

### `tmpfs_mount(ptr)` [tmpfs.c:371]
```
tmpfs_mount(ptr)
├── kmalloc(sizeof(tmpfs_fs_t))                        [kmalloc.c:71]
├── kmemset(fs, 0, sizeof)                             [leaf]
├── tmpfs_create_file(TMPFS_TYPE_DIR)                  [tmpfs.c:9]
│   ├── kmalloc(sizeof(tmpfs_node_t))                  [kmalloc.c:71]
│   └── kmemset(node, 0, sizeof)                       [leaf]
├── spinlock_init(&fs->lock, "tmpfs")                  [leaf]
├── kstrncpy(fs->vfs_fs.name, "tmpfs")                 [leaf]
├── [set root node, ops=&tmpfs_ops]
├── vfs_mount("/tmp", &fs->vfs_fs)                     [vfs.c:90]
│   ├── kstrncpy(mnt->path, "/tmp")                    [leaf]
│   └── [store mount in VFS mount table]
├── kfree(fs) [on error]                               [kmalloc.c:114]
└── kprintf()                                          [leaf]
```

### `devfs_mount()` [devfs.c:205]
```
devfs_mount()
├── kmalloc(sizeof(devfs_fs_t))                        [kmalloc.c:71]
├── kmemset(fs, 0, sizeof)                             [leaf]
├── kstrncpy(fs->vfs_fs.name, "devfs")                 [leaf]
├── [set root node, ops=&devfs_ops]
├── vfs_mount("/dev", &fs->vfs_fs)                     [vfs.c:90]
├── kfree(fs) [on error]                               [kmalloc.c:114]
└── kprintf()                                          [leaf]
```

---

## SFS Filesystem

### `sfs_format(dev)` [sfs.c:1204]
```
sfs_format(dev)
├── kmemset(sb, 0, SFS_BLOCK_SIZE)                    [leaf]
├── journal_init(dev, journal_start)                   [journal.c:54]
│   ├── kmemset(&jsb, 0, sizeof)
│   ├── journal_checksum(&jsb)                         [journal.c:33 — XOR loop]
│   ├── bdev->write at JSB_BLOCK                       [leaf]
│   └── kprintf()                                     [leaf]
├── sfs_sb_finalize(sb)                                [sfs.c:36]
│   └── sfs_sb_checksum(sb)                            [sfs.c:19 — XOR loop]
├── kmemset(block_bitmap_buf, 0xFF, bitmap_size)      [leaf]
├── block_write(dev, ...) × N                          [block.c:104]
│   ├── spinlock_acquire(&cache_lock)                  [leaf]
│   ├── cache_lookup(dev, lba)                        [block.c:27]
│   ├── cache_evict(dev, lba) if full                  [block.c:38]
│   ├── kmemcpy(cache->data, buf)                     [leaf]
│   └── spinlock_release(&cache_lock)                 [leaf]
├── block_write(dev, sb_block)                         [block.c:104]
├── block_sync_dev(dev)                                [block.c:161]
│   ├── spinlock_acquire(&cache_lock)                  [leaf]
│   ├── [flush all dirty blocks: dev->write]           [leaf]
│   └── spinlock_release(&cache_lock)                 [leaf]
└── kprintf()                                          [leaf]
```

### `sfs_mount(dev)` [sfs.c:1257]
```
sfs_mount(dev)
├── kmalloc(sizeof(sfs_fs_t))                          [kmalloc.c:71]
├── kmemset(fs, 0, sizeof)                             [leaf]
├── mutex_init(&fs->lock)                              [sync.c:42]
│   ├── wait_queue_init()                              [sched.c:589]
│   └── [set locked=0, owner_tid=0]
├── journal_recover(dev, journal_start, &dirty)        [journal.c:69]
│   └── [read JSB, verify checksum, replay committed entries]
├── block_read(dev, sb_block, 1, &sb)                  [block.c:76]
│   ├── [cache lookup or dev->read]
├── sfs_init_block_owner(fs, dev)                      [sfs.c:166]
│   ├── kmalloc(bitmap_cache)
│   ├── block_read(dev, i, 1, cache)                   [block.c:76]
│   └── [iterate block groups]
├── sfs_sb_verify(&sb)                                 [sfs.c:27]
│   └── sfs_sb_checksum(&sb)                           [leaf]
├── sfs_fsck(fs, dev) [if journal dirty]               [fsck.c:276]
│   ├── kmalloc(imap + bmap)                           [kmalloc.c:71]
│   ├── block_read(dev, ...) × N                       [block.c:76]
│   ├── fsck_scan_inodes(fs)                           [fsck.c:23]
│   │   ├── sfs_read_inode(fs, inum, &inode)          [sfs.c:187]
│   │   │   ├── sfs_read_block(fs, blocknum, buf)     [sfs.c:41]
│   │   │   │   └── block_read(dev, lba, 1, buf)      [block.c:76]
│   │   │   └── kmemcpy(&inode, buf + offset, 64)     [leaf]
│   │   ├── sfs_read_data(fs, &inode, buf, size)      [sfs.c:207]
│   │   └── kfree(scan bitmaps)                        [kmalloc.c:114]
│   └── kfree(bitmaps) × 2                             [kmalloc.c:114]
├── [read root inode]
├── kstrncpy(fs->vfs_fs.name, "sfs")                   [leaf]
├── kmalloc(sfs_file_t for root dentry)
├── vfs_register_fs(&fs->vfs_fs)                       [vfs.c:79]
└── kprintf()                                          [leaf]
```

---

## EventBus, Watchdog, Shell

### `eventbus_init()` [eventbus.c:30]
```
eventbus_init()
├── kmemset(subscribers, 0, MAX_SUBSCRIBERS * sizeof)
├── kmemset(pending_events, 0, MAX_EVENTS * sizeof)
├── spinlock_init(&eventbus_lock, "eventbus")
└── bus_initialized = 1
```

### `watchdog_init()` [watchdog.c:150]
```
watchdog_init()
├── kmemset(watchdog_layers, 0, sizeof)
├── watchdog_register_layer(1, "HAL", hal_health_check) [watchdog.c:70]
│   └── [store in next slot]
├── watchdog_register_layer(2, "Scheduler", sched_health_check)
├── watchdog_register_layer(4, "Memory", pmm_health_check)
└── watchdog_running = 1
```

### `hal_irq_register(irq, handler, data)` [hal.c:333]
```
hal_irq_register(irq, handler, data)
├── spinlock_acquire(&irq_reg_lock)                   [leaf]
├── irq_handlers[irq].handler = handler               [leaf]
├── irq_handlers[irq].data = data                     [leaf]
├── __sync_synchronize()                               [leaf — mfence]
└── spinlock_release(&irq_reg_lock)                   [leaf]
```

### `hal_uart_rx_init()` [hal.c:375]
```
hal_uart_rx_init()
├── hal_irq_register(UART_IRQ, uart_rx_isr, NULL)    [hal.c:333]
├── outb(UART_IER, 0x01)                               [leaf — enable RX interrupt]
└── uart_rx_irq_active = 1
```

### `shell_init()` [shell.c:2503]
```
shell_init()
├── shell_setenv("OS", "OPERtur") × 3                 [shell.c:125]
│   ├── kstrcmp(name, env[i].name)                    [leaf]
│   ├── kstrncpy(env[i].name, name)                   [leaf]
│   └── kstrncpy(env[i].value, value)                 [leaf]
├── kstrncpy(cwd, "/", SHELL_LINE_BUF-1)              [leaf]
└── kprintf()                                         [leaf]
```

### `shell_run()` [shell.c:2511]
```
shell_run()
├── [print banner: \nOPERtur/TRY1 OS v0.2.0\n]
├── shell_source("/etc/rc")                           [shell.c read+exec]
│   ├── vfs_open / vfs_read / vfs_close               [VFS ops]
│   └── [parse + execute each line]
└── [main loop: shell_readline → parse → exec → loop]
    ├── shell_readline()                              [UART RX read + line buffer]
    ├── shell_parse(line, &pipeline)                   [parse_pipeline → parse_stage]
    └── process_line(pipeline)                         [exec_stage → exec...]
```

---

## Kernel Thread Post-Boot

### `nic_poll_thread()` [main.c:80] (runs forever)
```
nic_poll_thread(arg)
├── [loop:]
│   ├── eth_rx_poll()                                  [eth.c:71]
│   │   ├── nic.poll() → e1000_poll()                 [leaf]
│   │   └── [dispatch to ARP/IPv4/IPv6 handlers]
│   ├── tcp_tick()                                    [tcp.c:614]
│   │   ├── spinlock_acquire(&tcp_lock)
│   │   ├── [for each conn: check retransmit timers]
│   │   │   └── [if RTO expired: tcp_send_pkt(retrans)]
│   │   ├── [for each conn: check TIME_WAIT timer]
│   │   └── spinlock_release(&tcp_lock)
│   └── thread_sleep(10)                               [sched.c:569]
```

### `udp_echo_server()` [main.c:90] (runs forever)
```
udp_echo_server(arg)
├── udp_bind_endpoint(AF_INET6, NULL, 9999, ...)      [udp.c:97]
├── udp_find_endpoint(AF_INET6, 9999)                 [udp.c:155]
├── udp_bind_endpoint(AF_INET, NULL, 9999, ...)       [udp.c:97]
├── udp_find_endpoint(AF_INET, 9999)                  [udp.c:155]
├── [loop:]
│   ├── udp_endpoint_dequeue(ep6, buf, ..., 100)     [udp.c:200]
│   │   ├── eth_rx_poll()                             [eth.c:71]
│   │   ├── [spinlock: check queue count]
│   │   ├── [hpet_ns() timeout check]
│   │   └── thread_yield()                            [sched.c:487]
│   ├── [if timeout: udp_endpoint_dequeue(ep4, ...)]
│   └── [if data: udp_sendto(src, data)]              [udp.c:62]
```

---

## Leaf Function Reference

| Leaf Function | File | What it does |
|---|---|---|
| `kmemset()` | kernel.h / string.asm | Memory fill (rep stosb) |
| `kmemcpy()` | kernel.h / string.asm | Memory copy (rep movsb) |
| `kstrncpy()` | kernel.h / string.asm | Bounded string copy |
| `kstrlen()` | kernel.h / string.asm | String length |
| `kstrcmp()` | kernel.h / string.asm | String compare |
| `kstrncmp()` | kernel.h / string.asm | Bounded string compare |
| `kprintf()` | kernel.h / printf.c | Formatted serial output |
| `kputchar()` | klib.c | UART char out (CLI/STI guarded) |
| `spinlock_init()` | sync.c:6 | Set lock=0, name, holder=0 |
| `spinlock_acquire()` | sync.c:12 | hal_save_irq + test_and_set + pause loop |
| `spinlock_try_acquire()` | sync.c:23 | hal_save_irq + test_and_set (non-blocking) |
| `spinlock_release()` | sync.c:35 | sync + lock_release + hal_restore_irq |
| `hal_save_irq()` | hal.c:129 | pushfq; popq; cli |
| `hal_restore_irq()` | hal.c:135 | sti if flags & 0x200 |
| `hal_sti()` | hal.c:126 | asm("sti") |
| `mb()` | barrier.h:9 | asm("mfence") |
| `cpu_relax()` | barrier.h:35 | asm("pause") |
| `outb(port, val)` | kernel.h / io.h | asm("outb %0, %1") |
| `inb(port)` | kernel.h / io.h | asm("inb %1, %0") |
| `outl(port, val)` | kernel.h / io.h | asm("outl %0, %1") |
| `inl(port)` | kernel.h / io.h | asm("inl %1, %0") |
| `io_wait()` | hal.c:86 | outb(0x80, 0) |
| `rdtsc()` | hal.h:123 | asm("rdtsc") |
| `apic_read(reg)` | apic.c:60 | MMIO read or x2APIC rdmsr |
| `apic_write(reg, val)` | apic.c:67 | MMIO write + mfence or x2APIC wrmsr |
| `apic_read_msr()` | apic.c:17 | rdmsr |
| `apic_write_msr()` | apic.c:24 | wrmsr |
| `apic_eoi()` | apic.c:77 | apic_write(APIC_REG_EOI, 0) |
| `hpet_read(reg)` | hpet.c:38 | MMIO read |
| `hpet_write(reg, val)` | hpet.c:42 | MMIO write |
| `hpet_ns()` | hpet.c:120 | Read HPET counter → nanoseconds |
| `smp_cpu_id()` | smp.c:24 | rdmsr + apic_read(APIC_ID) |
| `bitmap_set/clear/test()` | pmm.c:67-74 | Bit operations on uint8_t array |
| `list_init()` | types.h:88 | node->next = node->prev = node |
| `list_add_tail()` | types.h:92 | Double-linked list insert |
| `list_del()` | types.h:99 | Double-linked list remove |
| `wait_queue_init()` | sched.c:589 | wq->waiters=NULL; count=0; spinlock_init |
| `switch_context()` | ctx.S | Save/restore regs, swap RSP |
| `thread_yield()` | sched.c:487 | Set slice=0 → schedule() |

---

## Memory Layout at Boot (approximate)

```
0x0000_0000_0000 — BIOS / reserved
0x0000_0000_4000 — SMP trampoline code
0x0000_0000_8000 — ... (free)
0x0000_0008_0000 — Kernel ELF load (bootloader)
0x0000_000F_5320 — ACPI RSDP
0x0000_000F_0000 — ACPI tables
...

0x0000_0100_0000 — Kernel PML4 (after paging init)
  ├── PML4[0..255] — user half (per-process)
  ├── PML4[256] — kernel text (KERNEL_VMA_BASE)
  │   ├── PDPT — PD — PT → .text, .rodata, .data, .bss
  ├── PML4[257..510] — kernel heap, MMIO maps
  └── PML4[255] — PID stamp (per-process)

KERNEL_VMA_BASE = 0xFFFF_8000_0000_0000
  ├── 0xFFFF_8000_0000_0000 — kernel .text
  ├── 0xFFFF_8000_0000_1000 — kernel .rodata
  ├── ... 
  ├── 0xFFFF_8000_0010_0000 — kernel .data / .bss
  ├── 0xFFFF_8000_0100_0000 — kernel heap (slab allocator)
  ├── 0xFFFF_8000_FEBC_0000 — E1000 MMIO BAR0
  └── 0xFFFF_8000_FEE0_0000 — Local APIC MMIO

User space:
  ├── 0x0000_0000_4000_0000 — mmap_brk start
  └── 0x0000_0000_6000_0000 — user stack (ASLR randomized)
```

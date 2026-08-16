#include "kernel.h"
#include "hal.h"
#include "pmm.h"
#include "vmm.h"
#include "sched.h"
#include "shell.h"
#include "eventbus.h"
#include "watchdog.h"
#include "rcu.h"
#include "lockdep.h"
#include "futex.h"
#include "epoll.h"
#include "shm.h"
#include "sysfs.h"
#include "kmalloc.h"
#include "syscall.h"
#include "process.h"
#include "keyboard.h"
#include "ata.h"
#include "ahci.h"
#include "nvme.h"
#include "pci.h"
#include "vfs.h"
#include "ramdisk.h"
#include "ramdisk_blk.h"
#include "sfs.h"
#include "block.h"
#include "tmpfs.h"
#include "devfs.h"
#include "procfs.h"
#include "tty.h"
#include "pty.h"
#include "elf.h"
#include "work.h"
#include "smp.h"
#include "hpet.h"
#include "apic.h"
#include "acpi.h"
#include "swap.h"
#include "security_test.h"
#include "nic.h"
#include "e1000.h"
#include "eth.h"
#include "arp.h"
#include "ndp.h"
#include "route.h"
#include "ipv4.h"
#include "ipv6.h"
#include "icmp.h"
#include "icmpv6.h"
#include "net_ns.h"
#include "udp.h"
#include "tcp.h"
#include "dns.h"
#include "ntp.h"
#include "net.h"
#include "net_test.h"
#include "storage_test.h"
#include "kernel_test.h"
#include "sfs_test.h"
#include "process_test.h"
#include "security_test.h"
#include "dhcp.h"
#include "slaac.h"
#include "gpt.h"

static uint64_t mb_info_phys = 0;
#define BOOT_TOTAL_STEPS 16
static int boot_step = 0;
volatile int boot_complete = 0;

#define XEN_HVM_START_MAGIC 0x33687578

static void boot_report(const char* desc) {
    boot_step++;
    kprintf("[BOOT] Step %d/%d: %s\n", boot_step, BOOT_TOTAL_STEPS, desc);
}

static void on_tcp_echo_recv(tcp_conn_t* c, const uint8_t* d, uint32_t l) {
    kprintf("[TCP] Echo recv %u bytes\n", l);
    tcp_send(c, d, l);
}
static void on_tcp_echo_connect(tcp_conn_t* c) {
    kprintf("[TCP] Echo client connected!\n");
    c->on_recv = on_tcp_echo_recv;
}

static void nic_poll_thread(void* arg) {
    (void)arg;
    kprintf("[NET] NIC poll thread started\n");
    while (1) {
        eth_rx_poll();
        tcp_tick();
        thread_sleep(10);
    }
}

static void udp_echo_server(void* arg) {
    (void)arg;
    /* Bind IPv6 */
    int e = udp_bind_endpoint(AF_INET6, NULL, 9999, 0, 0, 1);
    if (e != ERR_OK) {
        kprintf("[UDP] Echo server IPv6 bind failed: %d\n", e);
        return;
    }
    udp_endpoint_t* bound_ep6 = udp_find_endpoint(AF_INET6, 9999);
    if (!bound_ep6) {
        kprintf("[UDP] Echo server: IPv6 endpoint not found\n");
        return;
    }
    /* Bind IPv4 */
    e = udp_bind_endpoint(AF_INET, NULL, 9999, 0, 0, 1);
    if (e != ERR_OK) {
        kprintf("[UDP] Echo server IPv4 bind failed: %d\n", e);
        return;
    }
    udp_endpoint_t* bound_ep4 = udp_find_endpoint(AF_INET, 9999);
    if (!bound_ep4) {
        kprintf("[UDP] Echo server: IPv4 endpoint not found\n");
        return;
    }
    kprintf("[UDP] Echo server running on port 9999 (IPv4+IPv6)\n");
    uint8_t buf[1500];
    uint8_t src_ip[16];
    int src_af;
    uint16_t src_port;
    while (1) {
        int n = udp_endpoint_dequeue(bound_ep6, buf, sizeof(buf),
                                     &src_af, src_ip, &src_port, 100);
        if (n <= 0) {
            n = udp_endpoint_dequeue(bound_ep4, buf, sizeof(buf),
                                      &src_af, src_ip, &src_port, 100);
        }
        if (n > 0) {
            kprintf("[UDP] Echo server got %d bytes (AF=%d)\n", n, src_af);
            udp_sendto(src_af, src_ip, src_port, 9999, buf, (uint32_t)n);
        }
    }
}

void kmain(uint64_t magic, uint64_t mb_info) {
    if (magic == XEN_HVM_START_MAGIC)
        mb_info = 0;
    mb_info_phys = mb_info;

    uint64_t mem_size = hal_get_mem_size(mb_info_phys);
    hal_init(mb_info_phys);

    boot_report("Layer 1 (HAL) - GDT, IDT, UART, Timer, PIC");
    kprintf("[BOOT] Detected memory: %llu MB\n", mem_size / (1024 * 1024));

    boot_report("Layer 4 (PMM) - Physical Memory Manager");
    pmm_init(mem_size, mb_info_phys);

    boot_report("Layer 4 (VMM) - Virtual Memory Manager");
    vmm_init();
    vmm_protect_kernel_text();

    boot_report("Kernel Heap - Slab Allocator");
    kmalloc_init();

    boot_report("HPET Timer");
    if (hpet_init() == ERR_OK) {
        hpet_timer_init();
    } else {
        kprintf("[HPET] Not available, using PIT only\n");
    }

    boot_report("Swap - Page Backing Store");
    swap_init();

    boot_report("APIC Interrupt Controller");
    if (apic_init() == ERR_OK) {
        apic_enable();
        apic_timer_init(1000);
        apic_disable_pic();
    } else {
        kprintf("[APIC] Not available, using legacy PIC\n");
    }

    boot_report("Syscall Interface");
    syscall_init();

    boot_report("ACPI");
    acpi_init(mb_info_phys);

    boot_report("SMP - Symmetric Multi-Processing");
    smp_init();

    boot_report("I/O APIC");
    apic_ioapic_init();

    boot_report("VMM Identity-map split (workaround QEMU TCG 2MB-page bug)");
    vmm_split_identity_map();

    boot_report("Layer 2 (SCHED) - Scheduler & Threading");
    sched_init();

    /* Switch from the BSS-embedded boot stack to the init thread's own kernel
     * stack.  The boot stack sits inside the BSS section — right where the
     * kernel heap begins — so kmalloc from any context (timer, device)
     * would silently corrupt local variables if we stayed on the BSS stack
     * once interrupts are enabled. */
    if (current_thread && current_thread->kernel_stack) {
        uint64_t init_stack = (uint64_t)current_thread->kernel_stack
                            + current_thread->kernel_stack_size - 128;
        kprintf("[SCHED] Switching to init thread's kernel stack @ %p\n",
                (void*)init_stack);
        hal_switch_stack(init_stack);
    }

    boot_report("Lockdep — Lock Dependency Validator");
    lockdep_init();

    /* Bring up application processors (APs) after scheduler is ready */
    /* Bring up application processors (APs) after scheduler is ready */
    smp_init_aps();
    smp_test_cross_cpu_ipi();
    smp_test_ap_preemption();

    boot_report("Layer 3 (PROCESS) - Process Manager");
    process_init();

    boot_report("Work Queue & Deferred Tasks");
    work_init();

    boot_report("RCU - Read-Copy-Update");
    rcu_init();

    boot_report("I/O Subsystem - Keyboard, ATA, PCI, VFS, Ramdisk");
    keyboard_init();
    ata_init();
    pci_init();
    vfs_init();
    futex_init();
    epoll_init();
    tty_init();
    pty_init();
    ramdisk_init();

    ramdisk_add_file("version.txt", "OPERtur/TRY1 OS v0.2.0\nLayered x86-64 Kernel\n", 48);
    ramdisk_add_file("welcome.txt", "Welcome to OPERtur!\n", 20);
    ramdisk_add_file("readme.txt",  "This is a ramdisk test file.\n", 29);

    extern char _binary_build_user_program_elf_start[];
    extern char _binary_build_user_program_elf_end[];
    size_t hello_elf_size = (uint64_t)_binary_build_user_program_elf_end
                          - (uint64_t)_binary_build_user_program_elf_start;
    ramdisk_add_file("hello.elf", _binary_build_user_program_elf_start, hello_elf_size);

    extern char _binary_build_cat_program_elf_start[];
    extern char _binary_build_cat_program_elf_end[];
    size_t cat_elf_size = (uint64_t)_binary_build_cat_program_elf_end
                         - (uint64_t)_binary_build_cat_program_elf_start;
    ramdisk_add_file("cat.elf", _binary_build_cat_program_elf_start, cat_elf_size);

        extern char _binary_build_hello_c_elf_start[];
        extern char _binary_build_hello_c_elf_end[];
        size_t hello_c_elf_size = (uint64_t)_binary_build_hello_c_elf_end
                             - (uint64_t)_binary_build_hello_c_elf_start;
        ramdisk_add_file("hello-c.elf", _binary_build_hello_c_elf_start, hello_c_elf_size);

    extern char _binary_build_tcp_echo_c_elf_start[];
    extern char _binary_build_tcp_echo_c_elf_end[];
    size_t tcp_echo_elf_size = (uint64_t)_binary_build_tcp_echo_c_elf_end
                             - (uint64_t)_binary_build_tcp_echo_c_elf_start;
    ramdisk_add_file("tcp_echo.elf", _binary_build_tcp_echo_c_elf_start, tcp_echo_elf_size);

    extern char _binary_build_udp_echo_c_elf_start[];
    extern char _binary_build_udp_echo_c_elf_end[];
    size_t udp_echo_elf_size = (uint64_t)_binary_build_udp_echo_c_elf_end
                             - (uint64_t)_binary_build_udp_echo_c_elf_start;
    ramdisk_add_file("udp_echo.elf", _binary_build_udp_echo_c_elf_start, udp_echo_elf_size);

        extern char _binary_build_ld_so_start[];
        extern char _binary_build_ld_so_end[];
        size_t ld_so_size = (uint64_t)_binary_build_ld_so_end
                          - (uint64_t)_binary_build_ld_so_start;
        ramdisk_add_file("ld.so", _binary_build_ld_so_start, ld_so_size);

        extern char _binary_build_libdyn_so_start[];
        extern char _binary_build_libdyn_so_end[];
        size_t libdyn_so_size = (uint64_t)_binary_build_libdyn_so_end
                              - (uint64_t)_binary_build_libdyn_so_start;
        ramdisk_add_file("libdyn.so", _binary_build_libdyn_so_start, libdyn_so_size);

        extern char _binary_build_hello_dyn_elf_start[];
        extern char _binary_build_hello_dyn_elf_end[];
        size_t hello_dyn_elf_size = (uint64_t)_binary_build_hello_dyn_elf_end
                                  - (uint64_t)_binary_build_hello_dyn_elf_start;
        ramdisk_add_file("hello-dyn.elf", _binary_build_hello_dyn_elf_start, hello_dyn_elf_size);

    /* Initialize block device layer and writable SFS filesystem */
    boot_report("Block Device Layer & SFS Filesystem");
    ramdisk_blk_init();
    ata_blk_init();
    ahci_init();
    nvme_init();
    gpt_scan();
    nic_init();
    net_ns_init();
    eth_init();
    arp_init();
    ndp_init();
    route_init();
    ipv4_init();
    ipv6_init();
    icmpv4_init();
    icmpv6_init();
    udp_init();
    tcp_init();
    net_init();
    dns_init();

    /* Phase 10: DHCP + SLAAC */
    if (nic.present) {
        route_clear();
        if (dhcp_configure() != ERR_OK) {
            kprintf("[NET] DHCP failed, fallback to static IPv4\n");
            route_add_v4(ipv4_from_bytes(10, 0, 2, 0), 24,
                        ipv4_from_bytes(0, 0, 0, 0));
            route_add_v4(ipv4_from_bytes(0, 0, 0, 0), 0,
                        ipv4_from_bytes(10, 0, 2, 2));
            ipv4_set_addr(ipv4_from_bytes(10, 0, 2, 15));
        }
        slaac_init();
        if (slaac_configure() != ERR_OK) {
            kprintf("[NET] SLAAC failed (link-local still available)\n");
        }
        /* Drain any queued packets accumulated during DHCP/SLAAC */
        for (int _d = 0; _d < 5; _d++) {
            eth_rx_poll();
            thread_sleep(10);
        }
    }

    /* Phase 11: NTP clock synchronization */
    ntp_init();

#ifdef NET_SELF_TEST
    net_self_test();
#endif
#ifdef STORAGE_SELF_TEST
    storage_self_test();
#endif
    /* Probe for existing SFS on any block device (disk boot) */
    int booted_from_disk = 0;
    {
        block_dev_t* root_dev = NULL;
        int count = block_count();
        for (int i = 0; i < count; i++) {
            block_dev_t* d = block_get(i);
            if (kstrcmp(d->name, "ramdisk") == 0) continue;
            if (sfs_probe(d)) { root_dev = d; break; }
        }
        if (root_dev) {
            kprintf("[BOOT] Found SFS on '%s', mounting...\n", root_dev->name);
            sfs_mount(root_dev);
            booted_from_disk = 1;
        } else {
            kprintf("[BOOT] No SFS found on any block device, formatting ramdisk...\n");
            sfs_format(block_find("ramdisk"));
            sfs_mount(block_find("ramdisk"));
        }
    }

    /* Mount virtual filesystems before self-tests so procfs/devfs/tmpfs are available */
    {
        vfs_fs_t* tmpfs_vfs = NULL;
        tmpfs_mount(&tmpfs_vfs);
    }
    devfs_mount();
    procfs_mount();
    sysfs_mount();
    shm_init();

#ifdef KERNEL_SELF_TEST
    kernel_self_test();
#endif
#ifdef SFS_SELF_TEST
    sfs_self_test();
#endif
#ifdef PROCESS_SELF_TEST
    process_self_test();
#endif
#ifdef SECURITY_SELF_TEST
    security_self_test();
#endif

    /* Copy boot files from ramdisk into SFS (ramdisk fallback only;
       disk SFS images already contain these files at build time) */
    if (!booted_from_disk) {
    kprintf("[BOOT] Copy boot files from ramdisk into SFS...\n");
    kprintf("[BOOT] SFS copy: hello.elf\n");
    {
        extern char _binary_build_user_program_elf_start[];
        extern char _binary_build_user_program_elf_end[];
        size_t hello_sz = (uint64_t)_binary_build_user_program_elf_end
                        - (uint64_t)_binary_build_user_program_elf_start;
        vfs_create("/hello.elf", 0);
        int fd = vfs_open("/hello.elf", O_WRONLY);
        if (fd >= 0) {
            vfs_write(fd, _binary_build_user_program_elf_start, hello_sz);
            vfs_close(fd);
        }
    }

    kprintf("[BOOT] SFS copy: cat.elf\n");
    {
        extern char _binary_build_cat_program_elf_start[];
        extern char _binary_build_cat_program_elf_end[];
        size_t cat_sz = (uint64_t)_binary_build_cat_program_elf_end
                      - (uint64_t)_binary_build_cat_program_elf_start;
        vfs_create("/cat.elf", 0);
        int fd = vfs_open("/cat.elf", O_WRONLY);
        if (fd >= 0) {
            vfs_write(fd, _binary_build_cat_program_elf_start, cat_sz);
            vfs_close(fd);
        }
    }

    kprintf("[BOOT] SFS copy: hello-c.elf\n");
    {
        extern char _binary_build_hello_c_elf_start[];
        extern char _binary_build_hello_c_elf_end[];
        size_t hello_c_sz = (uint64_t)_binary_build_hello_c_elf_end
                          - (uint64_t)_binary_build_hello_c_elf_start;
        vfs_create("/hello-c.elf", 0);
        int fd = vfs_open("/hello-c.elf", O_WRONLY);
        if (fd >= 0) {
            vfs_write(fd, _binary_build_hello_c_elf_start, hello_c_sz);
            vfs_close(fd);
        }
    }

    kprintf("[BOOT] SFS copy: tcp_echo.elf\n");
    {
        extern char _binary_build_tcp_echo_c_elf_start[];
        extern char _binary_build_tcp_echo_c_elf_end[];
        size_t tcp_echo_sz = (uint64_t)_binary_build_tcp_echo_c_elf_end
                           - (uint64_t)_binary_build_tcp_echo_c_elf_start;
        vfs_create("/tcp_echo.elf", 0);
        int fd = vfs_open("/tcp_echo.elf", O_WRONLY);
        if (fd >= 0) {
            vfs_write(fd, _binary_build_tcp_echo_c_elf_start, tcp_echo_sz);
            vfs_close(fd);
        }
    }

    kprintf("[BOOT] SFS copy: udp_echo.elf\n");
    {
        extern char _binary_build_udp_echo_c_elf_start[];
        extern char _binary_build_udp_echo_c_elf_end[];
        size_t udp_echo_sz = (uint64_t)_binary_build_udp_echo_c_elf_end
                           - (uint64_t)_binary_build_udp_echo_c_elf_start;
        vfs_create("/udp_echo.elf", 0);
        int fd = vfs_open("/udp_echo.elf", O_WRONLY);
        if (fd >= 0) {
            vfs_write(fd, _binary_build_udp_echo_c_elf_start, udp_echo_sz);
            vfs_close(fd);
        }
    }

    kprintf("[BOOT] SFS copy: thread_test.elf\n");
    {
        extern char _binary_build_thread_test_c_elf_start[];
        extern char _binary_build_thread_test_c_elf_end[];
        size_t thread_test_sz = (uint64_t)_binary_build_thread_test_c_elf_end
                              - (uint64_t)_binary_build_thread_test_c_elf_start;
        vfs_create("/thread_test.elf", 0);
        int fd = vfs_open("/thread_test.elf", O_WRONLY);
        if (fd >= 0) {
            vfs_write(fd, _binary_build_thread_test_c_elf_start, thread_test_sz);
            vfs_close(fd);
        }
    }

    /* Copy dynamic linker + shared library + dynamic test to SFS */
    {
        extern char _binary_build_ld_so_start[];
        extern char _binary_build_ld_so_end[];
        size_t ld_sz = (uint64_t)_binary_build_ld_so_end
                     - (uint64_t)_binary_build_ld_so_start;
        vfs_create("/ld.so", 0);
        int fd = vfs_open("/ld.so", O_WRONLY);
        if (fd >= 0) {
            vfs_write(fd, _binary_build_ld_so_start, ld_sz);
            vfs_close(fd);
        }
    }

    {
        extern char _binary_build_libdyn_so_start[];
        extern char _binary_build_libdyn_so_end[];
        size_t so_sz = (uint64_t)_binary_build_libdyn_so_end
                     - (uint64_t)_binary_build_libdyn_so_start;
        vfs_create("/libdyn.so", 0);
        int fd = vfs_open("/libdyn.so", O_WRONLY);
        if (fd >= 0) {
            vfs_write(fd, _binary_build_libdyn_so_start, so_sz);
            vfs_close(fd);
        }
    }

    {
        extern char _binary_build_hello_dyn_elf_start[];
        extern char _binary_build_hello_dyn_elf_end[];
        size_t elf_sz = (uint64_t)_binary_build_hello_dyn_elf_end
                      - (uint64_t)_binary_build_hello_dyn_elf_start;
        vfs_create("/hello-dyn.elf", 0);
        int fd = vfs_open("/hello-dyn.elf", O_WRONLY);
        if (fd >= 0) {
            vfs_write(fd, _binary_build_hello_dyn_elf_start, elf_sz);
            vfs_close(fd);
        }
    }

    {
        vfs_create("/version.txt", 0);
        int fd = vfs_open("/version.txt", O_WRONLY);
        if (fd >= 0) {
            vfs_write(fd, "OPERtur/TRY1 OS v0.2.0\nLayered x86-64 Kernel\n", 48);
            vfs_close(fd);
        }
    }

    {
        vfs_create("/welcome.txt", 0);
        int fd = vfs_open("/welcome.txt", O_WRONLY);
        if (fd >= 0) {
            vfs_write(fd, "echo Welcome to OPERtur/TRY1 OS!\nversion\nls\n", 47);
            vfs_close(fd);
        }
    }
    kprintf("[BOOT] Copied boot files to SFS\n");
    } /* !booted_from_disk */

    /* Create /etc/rc startup script (idempotent — skip if already present) */
    if (!vfs_find("/etc/rc")) {
    vfs_mkdir("/etc");
    {
        const char* rc_content =
            "echo Loading services...\n"
            "echo Service launcher initialized\n";
        vfs_create("/etc/rc", 0);
        int rc_fd = vfs_open("/etc/rc", O_WRONLY);
        if (rc_fd >= 0) {
            vfs_write(rc_fd, rc_content, kstrlen(rc_content));
            vfs_close(rc_fd);
        }
    }
    } /* rc not present */

    boot_report("Cross-Cutting - EventBus & Watchdog");
    eventbus_init();
    watchdog_init();

    hal_irq_register(0, watchdog_timer_handler, NULL);
    hal_uart_rx_init();

    boot_report("Layer N-1 (SHELL) - User Interface");
    shell_init();

    kprintf("[BOOT] Boot Complete - All Layers Initialized.\n");

    boot_complete = 1;
    hal_enable_irqs();
    hal_sti();

    kprintf("[BOOT] Auto-test: listing ramdisk files...\n");
    vfs_node_t* root = vfs_find("/");
    if (root) {
        vfs_node_t* child = root->children;
        while (child) {
            kprintf("  FILE: %s (%llu bytes)\n", child->name, child->size);
            child = child->next;
        }
    }

    /* Spawn init-user early — this used to hang on SMP, but CLI in kputchar fixes it */
    {
        extern char _binary_build_user_program_elf_start[];
        extern char _binary_build_user_program_elf_end[];
        process_t* uproc = process_create("init-user", 1);
        if (uproc) {
            size_t elf_len = (uint64_t)_binary_build_user_program_elf_end
                           - (uint64_t)_binary_build_user_program_elf_start;
            err_t e = process_exec(uproc, _binary_build_user_program_elf_start, elf_len);
            if (e == ERR_OK) {
                kprintf("[BOOT] User process spawned, pid=%d\n", uproc->pid);
            }
        }
    }

    kprintf("[BOOT] Auto-test: ELF load test...\n");
    {
        extern char _binary_build_user_program_elf_start[];
        extern char _binary_build_user_program_elf_end[];
        process_t* test_proc = process_create("test", 0);
        if (test_proc) {
            size_t elf_len = (uint64_t)_binary_build_user_program_elf_end
                           - (uint64_t)_binary_build_user_program_elf_start;
            err_t e = elf_load(test_proc, _binary_build_user_program_elf_start, elf_len);
            if (e == ERR_OK) {
                kprintf("  ELF load OK, entry=%llx\n", test_proc->entry_point);
            } else {
                kprintf("  ELF load failed: %d\n", e);
            }
        }
    }

    /* ---- Start NIC poll thread before network services ---- */
    {
        thread_t* np = thread_create(nic_poll_thread, NULL, THREAD_DEF_PRIO, "nic-poll");
        if (np) {
            sched_set_thread_affinity(np, 1ULL); /* pin to CPU 0 — not SMP-safe */
            sched_add_thread(np);
            kprintf("[NET] NIC poll thread created\n");
        }
    }

    /* ---- Set up network services ---- */

    /* Set up TCP echo listener on port 80 (IPv4) */
    tcp_conn_t* ls = tcp_listen(AF_INET, 80, on_tcp_echo_connect);
    if (ls) {
        kprintf("[NET] TCP listening on port 80\n");
    }

    /* Set up IPv6 TCP echo listener on port 9 */
    tcp_conn_t* ls6 = tcp_listen(AF_INET6, 9, on_tcp_echo_connect);
    if (ls6) {
        kprintf("[NET] IPv6 TCP listening on port 9\n");
    }

    /* Start UDP echo server on port 9999 as a kernel thread */
    {
        thread_t* uth = thread_create(udp_echo_server, NULL, THREAD_DEF_PRIO, "udp-echo");
        if (uth) {
            sched_set_thread_affinity(uth, 1ULL); /* pin to CPU 0 — not SMP-safe */
            sched_add_thread(uth);
            kprintf("[NET] UDP echo server thread created\n");
        }
    }

    /* NIC status summary */
    if (nic.present) {
        uint32_t status = nic_reg_read(E1000_STATUS);
        uint32_t tpt = nic_reg_read(E1000_TPT);
        uint32_t gprc = nic_reg_read(E1000_GPRC);
        kprintf("[NET] NIC: %s %s SPEED=%s TPT=%u GPRC=%u\n",
               !!(status & E1000_STATUS_FD) ? "FD" : "HD",
               !!(status & E1000_STATUS_LU) ? "LINK_UP" : "LINK_DOWN",
               (status & 0x80) ? "1000M" : (status & 0x40) ? "100M" : "10M",
               tpt, gprc);

        uint8_t default_mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
        int is_default = (kmemcmp(nic.mac, default_mac, 6) == 0);
        if (is_default) {
            kprintf("[NET] Default MAC — listener mode\n");
            uint8_t conn_ip[16];
            kmemset(conn_ip, 0, 16);
            conn_ip[0] = 0xFE; conn_ip[1] = 0x80;
            conn_ip[8]  = 0x50; conn_ip[9]  = 0x54;
            conn_ip[10] = 0x00; conn_ip[11] = 0xFF;
            conn_ip[12] = 0xFE; conn_ip[13] = 0x12;
            conn_ip[14] = 0x34; conn_ip[15] = 0x57;
            uint8_t conn_mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x57};
            ndp_cache_update(conn_ip, conn_mac);
        } else {
            kprintf("[NET] Non-default MAC — connector mode\n");
            uint8_t peer_ip[16];
            kmemset(peer_ip, 0, 16);
            peer_ip[0] = 0xFE; peer_ip[1] = 0x80;
            peer_ip[8]  = 0x50; peer_ip[9]  = 0x54;
            peer_ip[10] = 0x00; peer_ip[11] = 0xFF;
            peer_ip[12] = 0xFE; peer_ip[13] = 0x12;
            peer_ip[14] = 0x34; peer_ip[15] = 0x56;
            uint8_t peer_mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
            ndp_cache_update(peer_ip, peer_mac);
        }
    } else {
        kprintf("[NET] SKIP: NIC not present\n");
    }

    kprintf("[BOOT] Starting shell...\n");
    shell_run();
    for (;;) asm volatile("cli; hlt");
}

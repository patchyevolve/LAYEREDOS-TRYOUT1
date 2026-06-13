#include "kernel.h"
#include "hal.h"
#include "pmm.h"
#include "vmm.h"
#include "sched.h"
#include "shell.h"
#include "eventbus.h"
#include "watchdog.h"
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
#include "tty.h"
#include "pty.h"
#include "elf.h"
#include "work.h"
#include "hpet.h"
#include "apic.h"
#include "swap.h"
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
#include "udp.h"
#include "tcp.h"
#include "dns.h"
#include "ntp.h"
#include "net.h"
#include "net_test.h"
#include "storage_test.h"
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

    boot_report("Layer 2 (SCHED) - Scheduler & Threading");
    sched_init();

    boot_report("Layer 3 (PROCESS) - Process Manager");
    process_init();

    boot_report("Work Queue & Deferred Tasks");
    work_init();

    boot_report("I/O Subsystem - Keyboard, ATA, PCI, VFS, Ramdisk");
    keyboard_init();
    ata_init();
    pci_init();
    vfs_init();
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
    kprintf("[BOOT] Formatting SFS on ramdisk...\n");
    sfs_format(block_find("ramdisk"));
    kprintf("[BOOT] Mounting SFS on ramdisk...\n");
    sfs_mount(block_find("ramdisk"));

    kprintf("[BOOT] Copy boot files from ramdisk into SFS...\n");
    /* Copy boot files from ramdisk into SFS */
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

    /* Create /etc directory */
    vfs_mkdir("/etc");

    /* Create /etc/rc startup script */
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

    /* Mount tmpfs at /tmp */
    vfs_fs_t* tmpfs_vfs = NULL;
    tmpfs_mount(&tmpfs_vfs);

    /* Mount devfs at /dev */
    devfs_mount();

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

    kprintf("[BOOT] Auto-test: ELF load test...\n");
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

    kprintf("[BOOT] Auto-test: spawning user mode process...\n");
    {
        extern char _binary_build_user_program_elf_start[];
        extern char _binary_build_user_program_elf_end[];
        process_t* uproc = process_create("init-user", 1);
        if (uproc) {
            size_t elf_len = (uint64_t)_binary_build_user_program_elf_end
                           - (uint64_t)_binary_build_user_program_elf_start;
            err_t e = process_exec(uproc, _binary_build_user_program_elf_start, elf_len);
            if (e == ERR_OK) {
                kprintf("  User process spawned, pid=%d\n", uproc->pid);
            }
        }
    }

    /* NOTE: Stress test moved after network tests (see below) */

    /* ---- Set up network services BEFORE auto-test so listeners are ready ---- */

    /* Start NIC poll thread to drain RX ring immediately */
    {
        thread_t* np = thread_create(nic_poll_thread, NULL, THREAD_DEF_PRIO, "nic-poll");
        if (np) {
            sched_add_thread(np);
            kprintf("[NET] NIC poll thread created\n");
        }
    }

    /* Drain any queued packets before setting up listeners */
    eth_rx_poll();
    thread_sleep(10);
    eth_rx_poll();

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
            sched_add_thread(uth);
            kprintf("[NET] UDP echo server thread created\n");
        }
    }

    /* Network auto-test (runs after listeners are active) */
    kprintf("[BOOT] Auto-test: network...\n");
    if (nic.present) {
        /* Drain any initial queued packets (DHCP offers etc from SLiRP) */
        {
            uint32_t drain = nic_reg_read(E1000_GPRC);
            kprintf("[NET] Drain: GPRC=%u RDH=%u\n", drain, nic_reg_read(E1000_RDH));
            uint32_t rdh = nic_reg_read(E1000_RDH);
            uint32_t rdt = nic_reg_read(E1000_RDT);
            kprintf("[NET] RDH=%u RDT=%u\n", rdh, rdt);
            /* Do a single poll to consume anything already in descriptors */
            eth_rx_poll();
            drain = nic_reg_read(E1000_GPRC);
            rdh = nic_reg_read(E1000_RDH);
            rdt = nic_reg_read(E1000_RDT);
            kprintf("[NET] After drain: GPRC=%u RDH=%u RDT=%u\n", drain, rdh, rdt);
        }

        /* Read various registers before test */
        uint32_t manc = nic_reg_read(E1000_MANC);
        kprintf("[NET] MANC=0x%08x\n", manc);
        uint32_t tpt_before = nic_reg_read(E1000_TPT);
        uint32_t gprc_before = nic_reg_read(E1000_GPRC);
        uint32_t tdh_before = nic_reg_read(E1000_TDH);
        uint32_t tdt_before = nic_reg_read(E1000_TDT);

        uint32_t status = nic_reg_read(E1000_STATUS);
        kprintf("[NET] STATUS=0x%08x (FD=%d LU=%d TXOFF=%d SPEED=%s)\n",
               status, !!(status & E1000_STATUS_FD), !!(status & E1000_STATUS_LU),
               !!(status & 4),
               (status & 0x80) ? "1000M" : (status & 0x40) ? "100M" : "10M");
        kprintf("[NET] Pre: TPT=%u GPRC=%u TDH=%u TDT=%u\n",
                tpt_before, gprc_before, tdh_before, tdt_before);

        /* Send a single ARP request directly, then check TX desc status */
        uint8_t target_mac[6];
        kprintf("[NET] Sending ARP request...\n");

        ipv4_addr_t target_ip = ipv4_from_bytes(10, 0, 2, 2);
        int ret = arp_resolve(target_ip, target_mac, 10000);
        kprintf("[NET] arp_resolve=%d\n", ret);

        /* Check descriptors and stats after */
        uint32_t tpt = nic_reg_read(E1000_TPT);
        uint32_t gprc = nic_reg_read(E1000_GPRC);
        uint32_t rdh = nic_reg_read(E1000_RDH);
        uint32_t rdt = nic_reg_read(E1000_RDT);
        uint32_t tdh = nic_reg_read(E1000_TDH);
        uint32_t tdt = nic_reg_read(E1000_TDT);
        uint32_t icr = nic_reg_read(0xC0);
        kprintf("[NET] Post: TPT=%u GPRC=%u TPT_delta=%d\n",
                tpt, gprc, (int)(tpt - tpt_before));
        kprintf("[NET] TDH=%u TDT=%u RDH=%u RDT=%u ICR=0x%x\n",
                tdh, tdt, rdh, rdt, icr);

        nic_dump_rx_ring();
        nic_dump_tx_ring();

        /* Test DNS resolution of real internet hostname */
        {
            uint8_t dns_result[16];
            int dns_af;
            kprintf("[NET] Testing DNS resolution of google.com...\n");
            int dns_ret = dns_resolve("google.com", dns_result, &dns_af, 5000);
            if (dns_ret == ERR_OK) {
                if (dns_af == AF_INET)
                    kprintf("[NET] google.com resolved to %d.%d.%d.%d\n",
                            dns_result[0], dns_result[1], dns_result[2], dns_result[3]);
                else
                    kprintf("[NET] google.com resolved to IPv6\n");
            } else {
                kprintf("[NET] google.com DNS resolution failed: %d\n", dns_ret);
            }
        }

        /* Test ICMPv4 ping to gateway */
        if (ret == ERR_OK) {
            kprintf("[NET] Testing ICMPv4 ping to 10.0.2.2...\n");
            int ping_ret = icmpv4_ping(ipv4_from_bytes(10, 0, 2, 2), 3000);
            kprintf("[NET] icmpv4_ping=%d\n", ping_ret);
        }

        /* Test UDP send path */
        {
            ipv4_addr_t gw = ipv4_from_bytes(10, 0, 2, 2);
            uint8_t udp_buf[] = {0x00, 0x00, 0x00, 0x00};
            kprintf("[NET] udp_sendto to 10.0.2.2:53...\n");
            int e = udp_sendto(AF_INET, &gw, 53, 12345, udp_buf, 4);
            eth_rx_poll();
            thread_sleep(100);
            kprintf("[NET] udp_sendto=%d (TPT=R/clr, not shown)\n", e);
        }

        /* IPv6 TAP test: send RS, resolve neighbor */
        kprintf("[NET] Sending Router Solicitation...\n");
        icmpv6_send_rs();

        /* Poll for RA from host */
        for (int p = 0; p < 10; p++) {
            eth_rx_poll();
            thread_sleep(100);
        }

        /* Try ICMPv6 ping to host's link-local */
        {
            uint8_t host_ip[16];
            kmemset(host_ip, 0, 16);
            host_ip[0] = 0xFE; host_ip[1] = 0x80;
            /* fe80::a01d:86ff:fea5:6caf */
            host_ip[8]  = 0xA0; host_ip[9]  = 0x1D;
            host_ip[10] = 0x86; host_ip[11] = 0xFF;
            host_ip[12] = 0xFE; host_ip[13] = 0xA5;
            host_ip[14] = 0x6C; host_ip[15] = 0xAF;

            kprintf("[NET] IPv6 ping host link-local...\n");
            int pr = icmpv6_ping(host_ip, 2000);
            kprintf("[NET] IPv6 ping result=%d\n", pr);
        }

        /* After a delay, try outgoing IPv6 TCP connect to default LL address */
        /* If our MAC is the default (52:54:00:12:34:56), skip self-connect */
        {
            uint8_t default_mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
            int is_default = (kmemcmp(nic.mac, default_mac, 6) == 0);
            if (is_default) {
                kprintf("[NET] Default MAC, acting as listener only\n");
                /* Pre-seed NDP cache for known connector MAC (52:54:00:12:34:57) */
                uint8_t connector_ip[16];
                kmemset(connector_ip, 0, 16);
                connector_ip[0] = 0xFE; connector_ip[1] = 0x80;
                connector_ip[8]  = 0x50; connector_ip[9]  = 0x54;
                connector_ip[10] = 0x00; connector_ip[11] = 0xFF;
                connector_ip[12] = 0xFE; connector_ip[13] = 0x12;
                connector_ip[14] = 0x34; connector_ip[15] = 0x57;
                uint8_t connector_mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x57};
                ndp_cache_update(connector_ip, connector_mac);
                kprintf("[NET] Pre-seeded NDP for connector at %x:%x:%x:%x:%x:%x:%x:%x\n",
                        (uint32_t)((connector_ip[0]<<8)|connector_ip[1]),
                        (uint32_t)((connector_ip[2]<<8)|connector_ip[3]),
                        (uint32_t)((connector_ip[4]<<8)|connector_ip[5]),
                        (uint32_t)((connector_ip[6]<<8)|connector_ip[7]),
                        (uint32_t)((connector_ip[8]<<8)|connector_ip[9]),
                        (uint32_t)((connector_ip[10]<<8)|connector_ip[11]),
                        (uint32_t)((connector_ip[12]<<8)|connector_ip[13]),
                        (uint32_t)((connector_ip[14]<<8)|connector_ip[15]));
            } else {
                uint8_t peer_ip[16];
                kmemset(peer_ip, 0, 16);
                peer_ip[0] = 0xFE; peer_ip[1] = 0x80;
                peer_ip[8]  = 0x50; peer_ip[9]  = 0x54;
                peer_ip[10] = 0x00; peer_ip[11] = 0xFF;
                peer_ip[12] = 0xFE; peer_ip[13] = 0x12;
                peer_ip[14] = 0x34; peer_ip[15] = 0x56;
                /* Pre-seed NDP cache for listener (no multicast NS forwarding in socket backend) */
                uint8_t peer_mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
                ndp_cache_update(peer_ip, peer_mac);
                kprintf("[NET] Pre-seeded NDP for listener at %x:%x:%x:%x:%x:%x:%x:%x\n",
                        (uint32_t)((peer_ip[0]<<8)|peer_ip[1]),
                        (uint32_t)((peer_ip[2]<<8)|peer_ip[3]),
                        (uint32_t)((peer_ip[4]<<8)|peer_ip[5]),
                        (uint32_t)((peer_ip[6]<<8)|peer_ip[7]),
                        (uint32_t)((peer_ip[8]<<8)|peer_ip[9]),
                        (uint32_t)((peer_ip[10]<<8)|peer_ip[11]),
                        (uint32_t)((peer_ip[12]<<8)|peer_ip[13]),
                        (uint32_t)((peer_ip[14]<<8)|peer_ip[15]));

                kprintf("[NET] Connector mode, will run userspace tests\n");
            }
        }
    } else {
        kprintf("[NET] SKIP: NIC not present\n");
    }

    /* Userspace TCP + UDP echo tests via /tcp_echo.elf and /udp_echo.elf */
    /* Retry both until they PASS (handles simultaneous boot without timeout races) */
    if (nic.present) {
        uint8_t default_mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
        int is_default = (kmemcmp(nic.mac, default_mac, 6) == 0);
        if (!is_default) {
            extern char _binary_build_tcp_echo_c_elf_start[];
            extern char _binary_build_tcp_echo_c_elf_end[];
            extern char _binary_build_udp_echo_c_elf_start[];
            extern char _binary_build_udp_echo_c_elf_end[];
            size_t tcp_elf_sz = (uint64_t)_binary_build_tcp_echo_c_elf_end
                              - (uint64_t)_binary_build_tcp_echo_c_elf_start;
            size_t udp_elf_sz = (uint64_t)_binary_build_udp_echo_c_elf_end
                              - (uint64_t)_binary_build_udp_echo_c_elf_start;

            int tcp_ok = 0, udp_ok = 0;
            int retries = 0;
            while ((!tcp_ok || !udp_ok) && retries < 3) {
                retries++;
                if (!tcp_ok) {
                    kprintf("[NET] Spawning /tcp_echo.elf...\n");
                    process_t* teproc = process_create("tcp_echo", 1);
                    if (teproc) {
                        err_t e = process_exec(teproc,
                            _binary_build_tcp_echo_c_elf_start, tcp_elf_sz);
                        if (e == ERR_OK) {
                            while (!teproc->exited && !(teproc->flags & PROC_FLAG_STOPPED))
                                sched_block(&teproc->exit_waiters);
                            if (teproc->exit_code == 0) {
                                kprintf("[NET] /tcp_echo.elf PASS\n");
                                tcp_ok = 1;
                            } else {
                                kprintf("[NET] /tcp_echo.elf failed (code=%d), retrying...\n",
                                        teproc->exit_code);
                            }
                        }
                    }
                }
                if (!udp_ok) {
                    kprintf("[NET] Spawning /udp_echo.elf...\n");
                    process_t* udproc = process_create("udp_echo", 1);
                    if (udproc) {
                        err_t e = process_exec(udproc,
                            _binary_build_udp_echo_c_elf_start, udp_elf_sz);
                        if (e == ERR_OK) {
                            while (!udproc->exited && !(udproc->flags & PROC_FLAG_STOPPED))
                                sched_block(&udproc->exit_waiters);
                            if (udproc->exit_code == 0) {
                                kprintf("[NET] /udp_echo.elf PASS\n");
                                udp_ok = 1;
                            } else {
                                kprintf("[NET] /udp_echo.elf failed (code=%d), retrying...\n",
                                        udproc->exit_code);
                            }
                        }
                    }
                }
                eth_rx_poll();
                thread_sleep(100);
            }
        }
    }



    kprintf("[BOOT] Starting shell...\n");
    shell_run();
    for (;;) asm volatile("cli; hlt");
}

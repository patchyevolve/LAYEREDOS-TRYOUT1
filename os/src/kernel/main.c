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
#include "pci.h"
#include "vfs.h"
#include "ramdisk.h"
#include "ramdisk_blk.h"
#include "sfs.h"
#include "block.h"
#include "tmpfs.h"
#include "devfs.h"
#include "elf.h"

static uint64_t mb_info_phys = 0;
#define BOOT_TOTAL_STEPS 6
static int boot_step = 0;
volatile int boot_complete = 0;

#define XEN_HVM_START_MAGIC 0x33687578

static void boot_report(const char* desc) {
    boot_step++;
    kprintf("[BOOT] Step %d/%d: %s\n", boot_step, BOOT_TOTAL_STEPS, desc);
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

    boot_report("Syscall Interface");
    syscall_init();

    boot_report("Layer 2 (SCHED) - Scheduler & Threading");
    sched_init();

    boot_report("Layer 3 (PROCESS) - Process Manager");
    process_init();

    boot_report("I/O Subsystem - Keyboard, ATA, PCI, VFS, Ramdisk");
    keyboard_init();
    ata_init();
    pci_init();
    vfs_init();
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

    /* Initialize block device layer and writable SFS filesystem */
    boot_report("Block Device Layer & SFS Filesystem");
    ramdisk_blk_init();
    sfs_format(block_find("ramdisk"));
    sfs_mount(block_find("ramdisk"));

    /* Copy boot files from ramdisk into SFS */
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

        extern char _binary_build_cat_program_elf_start[];
        extern char _binary_build_cat_program_elf_end[];
        size_t cat_sz = (uint64_t)_binary_build_cat_program_elf_end
                      - (uint64_t)_binary_build_cat_program_elf_start;
        vfs_create("/cat.elf", 0);
        fd = vfs_open("/cat.elf", O_WRONLY);
        if (fd >= 0) {
            vfs_write(fd, _binary_build_cat_program_elf_start, cat_sz);
            vfs_close(fd);
        }

        vfs_create("/version.txt", 0);
        fd = vfs_open("/version.txt", O_WRONLY);
        if (fd >= 0) {
            vfs_write(fd, "OPERtur/TRY1 OS v0.2.0\nLayered x86-64 Kernel\n", 48);
            vfs_close(fd);
        }

        vfs_create("/welcome.txt", 0);
        fd = vfs_open("/welcome.txt", O_WRONLY);
        if (fd >= 0) {
            vfs_write(fd, "echo Welcome to OPERtur/TRY1 OS!\nversion\nls\n", 47);
            vfs_close(fd);
        }
        kprintf("[BOOT] Copied boot files to SFS\n");
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
        process_t* uproc = process_create("init-user", 0);
        if (uproc) {
            size_t elf_len = (uint64_t)_binary_build_user_program_elf_end
                           - (uint64_t)_binary_build_user_program_elf_start;
            err_t e = process_exec(uproc, _binary_build_user_program_elf_start, elf_len);
            if (e == ERR_OK) {
                kprintf("  User process spawned, pid=%d\n", uproc->pid);
            }
        }
    }

    kprintf("[BOOT] Starting shell...\n");
    shell_run();
    for (;;) asm volatile("cli; hlt");
}

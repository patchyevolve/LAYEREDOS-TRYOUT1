╔════════════════════════════════════════════════════════════════╗
║         COMPREHENSIVE CODEBASE AUDIT - OPERtur/TRY1            ║
╚════════════════════════════════════════════════════════════════╝

Audit Date: Saturday 13 June 2026 08:48:10 AM IST
Audit Scope: All kernel and boot C/H files


════════════════════════════════════════
1. MEMORY MANAGEMENT ISSUES
════════════════════════════════════════

1.1 Buffer Overflow Vulnerabilities (strcpy, sprintf, etc.)
Found strcpy/strcat calls: 3
os/src/lib/libuser/string.c:char* strcpy(char* dst, const char* src) {
os/src/lib/libuser/string.c:char* strcat(char* dst, const char* src) {
os/src/lib/libuser/string.c:    strcpy(dst + strlen(dst), src);

1.2 Stack Allocation Safety (>4KB allocations on kernel stack)
os/src/kernel/fsck.c:            uint8_t ibuf[SFS_BLOCK_SIZE];
os/src/kernel/fsck.c:            uint8_t dibuf[SFS_BLOCK_SIZE];
os/src/kernel/fsck.c:                    uint8_t ibuf[SFS_BLOCK_SIZE];
os/src/kernel/fsck.c:        uint8_t disk_buf[SFS_BLOCK_SIZE];
os/src/kernel/fsck.c:        uint8_t tmp[SFS_BLOCK_SIZE];
os/src/kernel/pty.c:            ((uint8_t*)buf)[nread++] = p->m_buf[p->m_tail];
os/src/kernel/pty.c:            p->s_raw_buf[p->s_raw_head] = ((const uint8_t*)buf)[written++];
os/src/kernel/pty.c:        ((uint8_t*)buf)[nread++] = c;
os/src/kernel/pty.c:            p->m_buf[p->m_head] = ((const uint8_t*)buf)[written++];
os/src/kernel/e1000.c:static uint8_t*          e1000_rx_bufs[E1000_NUM_RX_DESC];
os/src/kernel/e1000.c:static uint8_t*          e1000_tx_bufs[E1000_NUM_TX_DESC];
os/src/kernel/e1000.c:        uint8_t s = e1000_rx_ring[i].status;
os/src/kernel/eth.c:    uint8_t buf[NIC_MAX_FRAME];
os/src/kernel/eth.c:    uint8_t buf[NIC_MAX_FRAME];
os/src/kernel/arp.c:    uint8_t     mac[6];
os/src/kernel/arp.c:    uint8_t broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
os/src/kernel/ndp.c:    uint8_t lladdr[IPV6_ADDR_LEN];
os/src/kernel/route.c:    uint8_t zero[16];
os/src/kernel/ipv4.c:    uint8_t buf[IPV4_HDR_LEN + len];
os/src/kernel/ipv4.c:        uint8_t bmac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

1.3 Potential Memory Leaks (kmalloc without kfree in error paths)
Files with kmalloc but limited kfree patterns:
os/src/kernel/devfs.c: kmalloc=3, kfree=1
os/src/kernel/work.c: kmalloc=1, kfree=0
os/src/kernel/kmalloc.c: kmalloc=33, kfree=2
os/src/kernel/main.c: kmalloc=2, kfree=0
os/src/kernel/pmm.c: kmalloc=2, kfree=0
os/src/kernel/sched.c: kmalloc=1, kfree=0
os/src/kernel/snap.c: kmalloc=1, kfree=0
os/src/kernel/tty.c: kmalloc=1, kfree=0
os/src/kernel/vma.c: kmalloc=4, kfree=2


════════════════════════════════════════
2. NULL POINTER DEREFERENCE RISKS
════════════════════════════════════════

2.1 Missing NULL checks after allocations:
os/src/kernel/devfs.c-#include "devfs.h"
os/src/kernel/devfs.c-#include "vfs.h"
os/src/kernel/devfs.c:#include "kmalloc.h"
--
os/src/kernel/devfs.c-    (void)node;
os/src/kernel/devfs.c-    if (index >= (uint32_t)DEVFS_ENTRIES) return -1;
os/src/kernel/devfs.c:    vfs_node_t* child = kmalloc(sizeof(vfs_node_t));
--
os/src/kernel/devfs.c-
os/src/kernel/devfs.c-err_t devfs_mount(void) {
os/src/kernel/devfs.c:    devfs_fs = kmalloc(sizeof(devfs_fs_t));
--
os/src/kernel/work.c-#include "kernel.h"
os/src/kernel/work.c-#include "sched.h"
os/src/kernel/work.c:#include "kmalloc.h"
--
os/src/kernel/fsck.c-#include "journal.h"
os/src/kernel/fsck.c-#include "pmm.h"
os/src/kernel/fsck.c:#include "kmalloc.h"
--
os/src/kernel/fsck.c-    /* Read the inode bitmap to cross-check */
os/src/kernel/fsck.c-    uint32_t imap_blocks = (total_inodes + SFS_BLOCK_SIZE * 8 - 1) / (SFS_BLOCK_SIZE * 8);
os/src/kernel/fsck.c:    uint8_t* imap_disk = (uint8_t*)kmalloc(imap_blocks * SFS_BLOCK_SIZE);
--
os/src/kernel/fsck.c-    uint32_t bmap_bytes = (total_data_blocks + 7) / 8;


════════════════════════════════════════
3. CONCURRENCY & SYNCHRONIZATION ISSUES
════════════════════════════════════════

3.1 Lock acquisition/release balance:
Checking for potential lock leaks in exception/error paths...
⚠️  os/src/kernel/watchdog.c: Locks=2 Unlocks=0
⚠️  os/src/kernel/ata.c: Locks=1 Unlocks=0
⚠️  os/src/kernel/sync.c: Locks=4 Unlocks=3
⚠️  os/src/kernel/pmm.c: Locks=3 Unlocks=0
⚠️  os/src/kernel/sched.c: Locks=18 Unlocks=0
⚠️  os/src/kernel/syscall.c: Locks=3 Unlocks=0
⚠️  os/src/kernel/vmm.c: Locks=1 Unlocks=0

3.2 Race conditions (variables accessed without lock):
os/src/kernel/sched.h:    volatile uint64_t   time_slice_remaining;
os/src/kernel/sched.h:extern volatile int sched_running;
os/src/kernel/sched.h:extern volatile int need_reschedule;
os/src/kernel/work.h:    volatile int state; /* 0=pending, 1=running, 2=done */
os/src/kernel/ata.h:    volatile uint8_t irq_status;
os/src/kernel/ata.h:    volatile int     irq_received;
os/src/kernel/sync.h:    volatile uint64_t lock;
os/src/kernel/sync.h:    volatile int      locked;
os/src/kernel/ahci.h:    volatile uint32_t prdbc;  /* Physical Region Descriptor Byte Count */
os/src/kernel/ahci.h:    volatile uint32_t* abar;   /* MMIO base */
os/src/kernel/nvme.h:    volatile uint32_t* regs;    /* MMIO registers */
os/src/kernel/nvme.h:    volatile int  acq_phase;  /* Phase tag for ACQ */
os/src/kernel/nvme.h:    volatile int  iocq_phase;
os/src/kernel/tcp.h:    volatile int recv_done;
os/src/kernel/hal.h:    asm volatile("inw %1, %0" : "=a"(v) : "dN"(port));


════════════════════════════════════════
4. ERROR HANDLING & RETURN VALUE CHECKS
════════════════════════════════════════

4.1 Functions with error paths but no return validation:
os/src/kernel/watchdog.c:    return ERR_OK;
os/src/kernel/ramdisk_blk.c:    if (offset + len > RAMDISK_BLK_SIZE) return ERR_NOSPACE;
os/src/kernel/ramdisk_blk.c:    return ERR_OK;
os/src/kernel/ramdisk_blk.c:    if (offset + len > RAMDISK_BLK_SIZE) return ERR_NOSPACE;
os/src/kernel/ramdisk_blk.c:    return ERR_OK;
os/src/kernel/ramdisk_blk.c:    if (!phys) return ERR_NOMEM;
os/src/kernel/ramdisk_blk.c:    return ERR_OK;
os/src/kernel/devfs.c:    if (!devfs_fs) return ERR_NOMEM;
os/src/kernel/devfs.c:    return ERR_OK;
os/src/kernel/work.c:    if (!wq || !name) return ERR_INVAL;
os/src/kernel/work.c:    return ERR_OK;
os/src/kernel/work.c:    if (!wq || !item || !item->func) return ERR_INVAL;
os/src/kernel/work.c:    return ERR_OK;
os/src/kernel/work.c:    if (!item) return ERR_INVAL;
os/src/kernel/work.c:    return ERR_OK;
os/src/kernel/work.c:    if (!task || !task->func) return ERR_INVAL;
os/src/kernel/work.c:    return ERR_OK;
os/src/kernel/work.c:    if (err != ERR_OK) return err;
os/src/kernel/work.c:        return ERR_NOMEM;
os/src/kernel/work.c:    return ERR_OK;

4.2 Syscall stubs without SYSCALL_COUNT validation:
54
Total syscalls defined


════════════════════════════════════════
5. RESOURCE MANAGEMENT ISSUES
════════════════════════════════════════

5.1 File descriptor leaks (open without close):
44
62

5.2 Thread/Process resource leaks:
os/src/kernel/work.c:    system_wq.worker = thread_create(work_worker_thread, &system_wq,
os/src/kernel/main.c:    process_t* test_proc = process_create("test", 0);
os/src/kernel/main.c:        process_t* uproc = process_create("init-user", 1);
os/src/kernel/main.c:        thread_t* np = thread_create(nic_poll_thread, NULL, THREAD_DEF_PRIO, "nic-poll");
os/src/kernel/main.c:        thread_t* uth = thread_create(udp_echo_server, NULL, THREAD_DEF_PRIO, "udp-echo");
os/src/kernel/main.c:                    process_t* teproc = process_create("tcp_echo", 1);
os/src/kernel/main.c:                    process_t* udproc = process_create("udp_echo", 1);
os/src/kernel/process.c:    process_t* init_proc = process_create("init", 0);
os/src/kernel/process.c:process_t* process_create(const char* name, pid_t ppid) {
os/src/kernel/sched.c:thread_t* thread_create(void (*func)(void*), void* arg,


════════════════════════════════════════
6. INTEGER ARITHMETIC ISSUES
════════════════════════════════════════

6.1 Unsigned integer underflow (size_t - positive value):
os/src/kernel/ramdisk_blk.c:    uint64_t pages = (RAMDISK_BLK_SIZE + PAGE_SIZE - 1) / PAGE_SIZE;
os/src/kernel/devfs.c:    if (index >= (uint32_t)DEVFS_ENTRIES) return -1;
os/src/kernel/ata.c:        uint8_t status = inb(drive->base + ATA_REG_STATUS);
os/src/kernel/ata.c:    uint8_t drive_idx = (uint8_t)(uintptr_t)dev->private_data;
os/src/kernel/ata.c:    uint8_t drive_idx = (uint8_t)(uintptr_t)dev->private_data;
os/src/kernel/sync.c:    uint64_t timeout_ns = (timeout_ms == (uint64_t)-1) ? (uint64_t)-1 :
os/src/kernel/sync.c:        if (timeout_ns != (uint64_t)-1) {
os/src/kernel/sync.c:            uint64_t elapsed = hal_timer_get_ns() - start_ns;
os/src/kernel/sync.c:    mutex_lock(m, (uint64_t)-1);
os/src/kernel/fsck.c:    uint32_t data_start = fs->sb.data_start;
os/src/kernel/fsck.c:    uint32_t imap_blocks = (total_inodes + SFS_BLOCK_SIZE * 8 - 1) / (SFS_BLOCK_SIZE * 8);
os/src/kernel/fsck.c:            uint32_t idx = blk - data_start;
os/src/kernel/fsck.c:                uint32_t idx = blk - data_start;
os/src/kernel/fsck.c:                    uint32_t didx = dblk - data_start;
os/src/kernel/fsck.c:                uint32_t idx = blk - data_start;

6.2 Potential integer overflow in array indexing:
os/src/kernel/ata.c:    uint32_t sectors_28 = (uint32_t)buf[60] | ((uint32_t)buf[61] << 16);
os/src/kernel/ata.c:    uint32_t sectors_48_lo = (uint32_t)buf[100] | ((uint32_t)buf[101] << 16);
os/src/kernel/ata.c:    uint32_t sectors_48_hi = (uint32_t)buf[102] | ((uint32_t)buf[103] << 16);
os/src/kernel/ata.c:    d->sector_count = sectors_48_lo ? ((uint64_t)sectors_48_hi << 32 | sectors_48_lo)
os/src/kernel/fsck.c:        int bmap_free = !(imap_disk[byte] & (1 << bit));
os/src/kernel/fsck.c:        inode_used[inum / 8] |= (1 << (inum % 8));
os/src/kernel/fsck.c:            if (block_used[idx / 8] & (1 << (idx % 8))) {
os/src/kernel/fsck.c:            block_used[idx / 8] |= (1 << (idx % 8));
os/src/kernel/fsck.c:                block_used[idx / 8] |= (1 << (idx % 8));
os/src/kernel/fsck.c:                    block_used[didx / 8] |= (1 << (didx % 8));
os/src/kernel/fsck.c:                block_used[idx / 8] |= (1 << (idx % 8));
os/src/kernel/fsck.c:                    block_used[iidx / 8] |= (1 << (iidx % 8));
os/src/kernel/fsck.c:                            block_used[didx / 8] |= (1 << (didx % 8));
os/src/kernel/fsck.c:                    disk_buf[i / 8] &= ~(1 << (i % 8));
os/src/kernel/fsck.c:                    disk_buf[i / 8] |= (1 << (i % 8));


════════════════════════════════════════
7. UNINITIALIZED VARIABLES & FIELDS
════════════════════════════════════════

7.1 Struct initialization gaps:
os/src/kernel/devfs.c:    vfs_node_t* child = kmalloc(sizeof(vfs_node_t));
os/src/kernel/devfs.c:    devfs_fs = kmalloc(sizeof(devfs_fs_t));
os/src/kernel/pty.c:    p->master_node = (vfs_node_t*)kmalloc(sizeof(vfs_node_t));
os/src/kernel/pty.c:    p->slave_node  = (vfs_node_t*)kmalloc(sizeof(vfs_node_t));
os/src/kernel/net.c:    socket_t* s = (socket_t*)kmalloc(sizeof(socket_t));
os/src/kernel/sfs.c:        sfs_file_t* f = kmalloc(sizeof(sfs_file_t));
os/src/kernel/sfs.c:    vfs_node_t* child = (vfs_node_t*)kmalloc(sizeof(vfs_node_t));
os/src/kernel/sfs.c:    sfs_file_t* cf = kmalloc(sizeof(sfs_file_t));
os/src/kernel/sfs.c:        f = kmalloc(sizeof(sfs_file_t));
os/src/kernel/sfs.c:    sfs_fs_t* fs = kmalloc(sizeof(sfs_fs_t));
os/src/kernel/sfs.c:    sfs_file_t* rf = kmalloc(sizeof(sfs_file_t));
os/src/kernel/pipe.c:    pipe_t* p = (pipe_t*)kmalloc(sizeof(pipe_t));
os/src/kernel/pipe.c:    vfs_node_t* rnode = (vfs_node_t*)kmalloc(sizeof(vfs_node_t));
os/src/kernel/pipe.c:    vfs_node_t* wnode = (vfs_node_t*)kmalloc(sizeof(vfs_node_t));
os/src/kernel/shell.c:    pipe_buf_t* bufs = kmalloc(sizeof(pipe_buf_t) * (nstages - 1));


════════════════════════════════════════
8. PLATFORM-SPECIFIC & ENDIANNESS ISSUES
════════════════════════════════════════

8.1 Byte order assumptions (network headers):
32
Uses hton/ntoh conversions

8.2 Pointer arithmetic on wrong types:


════════════════════════════════════════
9. BOUNDS CHECKING FAILURES
════════════════════════════════════════

9.1 Array accesses without bounds validation:
os/src/kernel/watchdog.c:static watchdog_layer_t layers[MAX_WATCHDOG_LAYERS];
os/src/kernel/watchdog.c:    layers[num_layers].layer_id = id;
os/src/kernel/watchdog.c:    layers[num_layers].name = name;
os/src/kernel/watchdog.c:    layers[num_layers].check = check;
os/src/kernel/watchdog.c:    layers[num_layers].consecutive_failures = 0;
os/src/kernel/watchdog.c:    layers[num_layers].total_failures = 0;
os/src/kernel/watchdog.c:static char watchdog_buf[WATCHDOG_BUF_SIZE];
os/src/kernel/watchdog.c:        watchdog_buf[pos++] = *s++;
os/src/kernel/watchdog.c:    watchdog_buf[pos] = '\0';
os/src/kernel/watchdog.c:    char reason[128];
os/src/kernel/watchdog.c:        reason[0] = '\0';
os/src/kernel/watchdog.c:        watchdog_health_t health = layers[i].check(reason, sizeof(reason));
os/src/kernel/watchdog.c:            layers[i].consecutive_failures++;
os/src/kernel/watchdog.c:            layers[i].total_failures++;
os/src/kernel/watchdog.c:            eventbus_publish(EV_WATCHDOG, (uint64_t)layers[i].layer_id,
os/src/kernel/watchdog.c:                           (uint64_t)health, (uint64_t)layers[i].total_failures, 0);
os/src/kernel/watchdog.c:            char tmp[128];
os/src/kernel/watchdog.c:            watchdog_buf_write("[WATCHDOG] Layer ");
os/src/kernel/watchdog.c:            watchdog_buf_write(layers[i].name);
os/src/kernel/watchdog.c:                watchdog_buf_write("[WATCHDOG] Escalating: Layer ");


════════════════════════════════════════
10. CODE QUALITY & MAINTAINABILITY
════════════════════════════════════════

10.1 TODO/FIXME/HACK comments indicating unfinished work:
os/src/kernel/elf.h:#define DT_DEBUG        21
os/src/kernel/e1000.c:    KDEBUG("[E1000 SEND] head=%d tx_next=%d len=%u status=0x%x\n",
os/src/kernel/e1000.c:    KDEBUG("[E1000] RX desc=%d len=%u status=0x%x errors=0x%x\n",
os/src/kernel/ipv6.c:    KDEBUG("[IP6 RX] nh=%u src=%x:%x:%x:%x:%x:%x:%x:%x dst=%x:%x:%x:%x:%x:%x:%x:%x\n",
os/src/kernel/icmpv6.c:    /* Solicited-node multicast: ff02::1:ffXX:XXXX */
os/src/kernel/icmpv6.c:    KDEBUG("[MLDv1] Report sent\n");
os/src/kernel/icmpv6.c:    KDEBUG("[MLDv1] Done sent\n");
os/src/kernel/udp.c:                KDEBUG("[UDP] IPv4 checksum mismatch: wire=0x%04x calc=0x%04x\n",
os/src/kernel/udp.c:            KDEBUG("[UDP] IPv6 zero checksum, dropping\n");
os/src/kernel/udp.c:            KDEBUG("[UDP] IPv6 checksum mismatch: wire=0x%04x calc=0x%04x\n",
os/src/kernel/tcp.c:        KDEBUG("[TCP] TX flags=0x%02x seq=%u ack=%u csum=0x%04x data=%u\n",
os/src/kernel/tcp.c:    KDEBUG("[TCP RX] flags=0x%02x sport=%u dport=%u seq=%u ack=%u len=%u\n",
os/src/kernel/pmm.c:#ifdef DEBUG
os/src/kernel/syscall.c:        KDEBUG("[SYSCALL] close(fd=%d) socket\n", fd);
os/src/kernel/syscall.c:    KDEBUG("[SYSCALL] socket(af=%d type=%d proto=%d) → fd=%d\n", user_af, type, protocol, fd);
os/src/kernel/syscall.c:    KDEBUG("[SYSCALL] bind(fd=%d) → %d\n", fd, e);
os/src/kernel/syscall.c:    KDEBUG("[SYSCALL] connect(fd=%d)\n", fd);
os/src/kernel/syscall.c:    KDEBUG("[SYSCALL] connect → %d\n", e);
os/src/kernel/vmm.c:#ifdef VMM_DEBUG

10.2 Magic numbers (hardcoded values without explanation):
os/src/kernel/watchdog.c:#define WATCHDOG_BUF_SIZE 512
os/src/kernel/ramdisk_blk.c:#define RAMDISK_BLK_SIZE (1 * 1024 * 1024)
os/src/kernel/ramdisk_blk.c:            RAMDISK_BLK_SIZE / 1024, ramdisk_blk_data);
os/src/kernel/ata.c:    for (int i = 0; i < 256; i++)
os/src/kernel/ata.c:            for (int i = 0; i < 256; i++)
os/src/kernel/ata.c:                outw(base + ATA_REG_DATA, word_buf[s * 256 + i]);
os/src/kernel/ata.c:            for (int i = 0; i < 256; i++)
os/src/kernel/ata.c:                word_buf[s * 256 + i] = inw(base + ATA_REG_DATA);
os/src/kernel/backup.c:#define BUF_SIZE 4096
os/src/kernel/ahci.c:    kmemset(bounce, 0, 4096);
os/src/kernel/ahci.c:        kmemset((void*)virt, 0, 4096);
os/src/kernel/ahci.c:        kmemset((void*)virt, 0, 4096);
os/src/kernel/nvme.c:    kmemset(nvme_ctrl.asq, 0, 4096);
os/src/kernel/nvme.c:    kmemset(nvme_ctrl.acq, 0, 4096);
os/src/kernel/nvme.c:        kmemset(nvme_ctrl.iocq, 0, 4096);

10.3 Potential dead code (unreachable paths):
os/src/kernel/tcp.c:            /* Don't goto process_established_data here — on_connect must
os/src/kernel/dhcp.c:            goto configured;
os/src/kernel/sfs.c:        if (sfs_read_data(f->fs, f->inode.double_indirect, dibuf) != ERR_OK) goto done;
os/src/kernel/syscall.c:    return proc->pid; /* Parent returns child PID */
os/src/kernel/tty.c:        if (ret < 0) return 0; /* signal interrupted → return 0 bytes */


════════════════════════════════════════
11. NETWORK & PROTOCOL ISSUES
════════════════════════════════════════

11.1 Unchecked packet buffer accesses:
os/src/kernel/main.c:            uint8_t udp_buf[] = {0x00, 0x00, 0x00, 0x00};

11.2 Potential buffer overruns in packet handlers:
os/src/kernel/ramdisk_blk.c:    kmemcpy(buf, ramdisk_blk_data + offset, len);
os/src/kernel/ramdisk_blk.c:    kmemcpy(ramdisk_blk_data + offset, buf, len);
os/src/kernel/ramdisk.c:    kmemcpy(buf, f->data + offset, count);
os/src/kernel/ahci.c:        kmemcpy(buf, bounce, total);
os/src/kernel/ahci.c:    kmemcpy(bounce, buf, total);
os/src/kernel/nvme.c:        kmemcpy(buf, data_virt, (size_t)byte_count);
os/src/kernel/nvme.c:    kmemcpy(data_virt, buf, (size_t)byte_count);
os/src/kernel/e1000.c:    kmemcpy(e1000_tx_bufs[tx_next], frame, len);
os/src/kernel/e1000.c:    kmemcpy(buf, e1000_rx_bufs[idx], len);
os/src/kernel/arp.c:    kmemcpy(sender_ip.bytes, pkt->spa, 4);


════════════════════════════════════════
12. SFS FILE SYSTEM ISSUES
════════════════════════════════════════

12.1 Block-level operations without bounds checking:
os/src/kernel/fsck.c:    uint32_t imap_blocks = (total_inodes + SFS_BLOCK_SIZE * 8 - 1) / (SFS_BLOCK_SIZE * 8);
os/src/kernel/fsck.c:    uint8_t* imap_disk = (uint8_t*)kmalloc(imap_blocks * SFS_BLOCK_SIZE);
os/src/kernel/fsck.c:    kmemset(imap_disk, 0, imap_blocks * SFS_BLOCK_SIZE);
os/src/kernel/fsck.c:        block_read(fs->bdev, fs->sb.inode_bmap_start + b, 1, imap_disk + b * SFS_BLOCK_SIZE);
os/src/kernel/fsck.c:            uint8_t ibuf[SFS_BLOCK_SIZE];
os/src/kernel/fsck.c:            uint8_t dibuf[SFS_BLOCK_SIZE];
os/src/kernel/fsck.c:                    uint8_t ibuf[SFS_BLOCK_SIZE];
os/src/kernel/fsck.c:    uint32_t bmap_blocks = (total_entries + SFS_BLOCK_SIZE * 8 - 1) / (SFS_BLOCK_SIZE * 8);
os/src/kernel/fsck.c:        uint8_t disk_buf[SFS_BLOCK_SIZE];
os/src/kernel/fsck.c:        for (uint32_t i = 0; i < SFS_BLOCK_SIZE * 8; i++) {
os/src/kernel/fsck.c:            uint32_t entry = b * SFS_BLOCK_SIZE * 8 + i;
os/src/kernel/fsck.c:        uint8_t tmp[SFS_BLOCK_SIZE];
os/src/kernel/sfs.c:    return (bits + SFS_BLOCK_SIZE * 8 - 1) / (SFS_BLOCK_SIZE * 8);
os/src/kernel/sfs.c:static err_t sfs_read_block(sfs_fs_t* fs, uint32_t block, void* buf) {
os/src/kernel/sfs.c:    uint8_t buf[SFS_BLOCK_SIZE];

12.2 Inode operations risk:
os/src/kernel/sfs.c:static int sfs_alloc_inode(sfs_fs_t* fs) {
os/src/kernel/sfs.c:    int inum = sfs_alloc_inode(f->fs);
os/src/kernel/sfs.c:    int inum = sfs_alloc_inode(dir_f->fs);


════════════════════════════════════════
13. KNOWN VULNERABILITY PATTERNS
════════════════════════════════════════

13.1 Format string vulnerabilities:

13.2 Use-after-free patterns:
os/src/kernel/watchdog.c:    uint64_t free_pct = pmm_total_pages() ?
os/src/kernel/watchdog.c:        pmm_free_pages_count() * 100 / pmm_total_pages() : 0;
os/src/kernel/watchdog.c:    if (free_pct < 5) {
os/src/kernel/watchdog.c:    uint64_t free = pmm_free_pages_count();
os/src/kernel/watchdog.c:    if (free == 0) {
os/src/kernel/watchdog.c:    if (free * 100 / total < 10) {
os/src/kernel/devfs.c:    if (e) { kfree(devfs_fs); devfs_fs = NULL; return e; }
os/src/kernel/fsck.c:        int bmap_free = !(imap_disk[byte] & (1 << bit));
os/src/kernel/fsck.c:            if (!bmap_free) {
os/src/kernel/fsck.c:        if (bmap_free) {


════════════════════════════════════════
AUDIT SUMMARY
════════════════════════════════════════

Total files analyzed: 155
Total lines: 3848

Critical Issues Found:
  - Concurrency/Lock issues: ~7

Recommendations:
1. Review all strcpy/strcat calls - replace with strnpy/strncat
2. Add systematic NULL checks after all allocations
3. Audit lock acquisition/release pairs for imbalance
4. Enable compiler warnings for uninitialized variables
5. Add static analysis tooling (cppcheck, clang-analyzer)

End of Audit

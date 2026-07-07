#include "kernel.h"
#include "e1000.h"
#include "nic.h"
#include "eth.h"
#include "pci.h"
#include "pmm.h"
#include "vmm.h"
#include "hal.h"
#include "apic.h"

/* Virtual address region for E1000 MMIO + DMA pages */
#define E1000_VADDR_BASE  0xFFFFFFA000000000ULL
#define E1000_VADDR_LIMIT (E1000_VADDR_BASE + 0x80000)

static uint64_t e1000_next_vaddr = E1000_VADDR_BASE;

/* E1000 per-controller state */
static volatile uint32_t* e1000_regs = NULL;
static e1000_tx_desc_t*  e1000_tx_ring = NULL;
static e1000_rx_desc_t*  e1000_rx_ring = NULL;
static uint64_t          e1000_tx_ring_phys = 0;
static uint64_t          e1000_rx_ring_phys = 0;
static uint8_t*          e1000_rx_bufs[E1000_NUM_RX_DESC];
static uint64_t          e1000_rx_bufs_phys[E1000_NUM_RX_DESC];
static uint8_t*          e1000_tx_bufs[E1000_NUM_TX_DESC];
static uint64_t          e1000_tx_bufs_phys[E1000_NUM_TX_DESC];
static volatile int      e1000_tx_head = 0;
static volatile int      e1000_tx_tail = 0;
static volatile int      e1000_rx_cur  = 0;
static int               e1000_irq    = 0;
static int               e1000_present = 0;
static volatile int      e1000_tx_lock = 0;

/* Forward declarations */
static err_t e1000_send(const struct nic* nic, const uint8_t* frame, uint32_t len);
static int   e1000_poll(const struct nic* nic, uint8_t* buf, uint32_t max_len);

/* ── MMIO helpers ───────────────────────────────────────────────────────── */
static inline uint32_t e1000_reg_read(uint32_t off) {
    return e1000_regs[off / 4];
}

static inline void e1000_reg_write(uint32_t off, uint32_t val) {
    e1000_regs[off / 4] = val;
}

/* ── DMA page allocator ─────────────────────────────────────────────────── */
static void* e1000_alloc_page(uint64_t* out_phys) {
    if (e1000_next_vaddr >= E1000_VADDR_LIMIT) return NULL;
    uint64_t phys = pmm_alloc_page();
    if (!phys) return NULL;
    uint64_t virt = e1000_next_vaddr;
    e1000_next_vaddr += 0x1000;
    err_t e = vmm_map_page(vmm_get_kernel_pml4(), virt, phys,
                           PAGE_PRESENT | PAGE_WRITE | PAGE_NX);
    if (e != ERR_OK) {
        pmm_free_page(phys);
        e1000_next_vaddr -= 0x1000;
        return NULL;
    }
    *out_phys = phys;
    return (void*)virt;
}

/* ── EEPROM read (via EERD register) ────────────────────────────────────── */
static uint16_t e1000_eeprom_read(uint32_t addr) {
    uint32_t val = (addr << E1000_EERD_ADDR_SH) | E1000_EERD_START;
    e1000_reg_write(E1000_EERD, val);
    for (int i = 0; i < 100; i++) {
        hal_udelay(1);
        val = e1000_reg_read(E1000_EERD);
        if (val & E1000_EERD_DONE) {
            return (uint16_t)(val >> E1000_EERD_DATA_SH);
        }
    }
    return 0;
}

/* ── Read MAC address ───────────────────────────────────────────────────── */
static void e1000_read_mac(uint8_t* mac) {
    uint32_t ral = e1000_reg_read(E1000_RAL0);
    uint32_t rah = e1000_reg_read(E1000_RAH0);
    if (ral == 0 && rah == 0) {
        ral = e1000_eeprom_read(0);
        rah = e1000_eeprom_read(1);
    }
    mac[0] =  ral        & 0xFF;
    mac[1] = (ral >> 8)  & 0xFF;
    mac[2] = (ral >> 16) & 0xFF;
    mac[3] = (ral >> 24) & 0xFF;
    mac[4] =  rah        & 0xFF;
    mac[5] = (rah >> 8)  & 0xFF;
}

/* ── E1000 soft reset ───────────────────────────────────────────────────── */
static err_t e1000_reset(void) {
    e1000_reg_write(E1000_CTRL, E1000_CTRL_RST);
    hal_udelay(1000);
    int timeout = 100;
    while (e1000_reg_read(E1000_CTRL) & E1000_CTRL_RST) {
        hal_udelay(1000);
        if (--timeout == 0) return ERR_TIMEOUT;
    }
    hal_udelay(10000);
    return ERR_OK;
}

/* ── Initialize descriptor rings ────────────────────────────────────────── */
static err_t e1000_init_rings(void) {
    /* Allocate TX descriptor ring (must be 16-byte aligned) */
    e1000_tx_ring = e1000_alloc_page(&e1000_tx_ring_phys);
    if (!e1000_tx_ring) return ERR_NOMEM;
    kmemset(e1000_tx_ring, 0, PAGE_SIZE);

    /* Allocate RX descriptor ring */
    e1000_rx_ring = e1000_alloc_page(&e1000_rx_ring_phys);
    if (!e1000_rx_ring) return ERR_NOMEM;
    kmemset(e1000_rx_ring, 0, PAGE_SIZE);

    /* Allocate RX bounce buffers */
    for (int i = 0; i < E1000_NUM_RX_DESC; i++) {
        e1000_rx_bufs[i] = e1000_alloc_page(&e1000_rx_bufs_phys[i]);
        if (!e1000_rx_bufs[i]) return ERR_NOMEM;
        e1000_rx_ring[i].addr = e1000_rx_bufs_phys[i];
        e1000_rx_ring[i].status = 0;
    }

    /* Allocate TX bounce buffers */
    for (int i = 0; i < E1000_NUM_TX_DESC; i++) {
        e1000_tx_bufs[i] = e1000_alloc_page(&e1000_tx_bufs_phys[i]);
        if (!e1000_tx_bufs[i]) return ERR_NOMEM;
        e1000_tx_ring[i].addr = e1000_tx_bufs_phys[i];
        e1000_tx_ring[i].status = E1000_TXD_STAT_DD;
    }

    return ERR_OK;
}

/* ── E1000 initialization ──────────────────────────────────────────────── */
static err_t e1000_init_nic(pci_device_t* dev) {
    /* Enable bus mastering */
    pci_enable_bus_mastering(dev);

    /* Decode BAR0 (MMIO) */
    uint64_t bar0_phys = (uint64_t)(dev->bar[0] & ~0xF);
    uint64_t bar0_virt = e1000_next_vaddr;
    e1000_next_vaddr += 0x20000; /* 128 KB for E1000 registers */

    for (uint64_t offset = 0; offset < 0x20000; offset += PAGE_SIZE) {
        err_t e = vmm_map_page(vmm_get_kernel_pml4(),
                                bar0_virt + offset,
                                bar0_phys + offset,
                                PAGE_PRESENT | PAGE_WRITE | PAGE_NX);
        if (e != ERR_OK) return e;
    }

    e1000_regs = (volatile uint32_t*)bar0_virt;

    /* Soft reset */
    err_t err = e1000_reset();
    if (err != ERR_OK) {
        kprintf("[E1000] Reset failed\n");
        return err;
    }

    /* Read MAC */
    e1000_read_mac(nic.mac);

    /* Initialize descriptor rings */
    err = e1000_init_rings();
    if (err != ERR_OK) {
        kprintf("[E1000] Ring init failed\n");
        return err;
    }

    /* Set RX ring base */
    e1000_reg_write(E1000_RDBAL, (uint32_t)(e1000_rx_ring_phys & 0xFFFFFFFF));
    e1000_reg_write(E1000_RDBAH, (uint32_t)((e1000_rx_ring_phys >> 32) & 0xFFFFFFFF));
    e1000_reg_write(E1000_RDLEN, E1000_NUM_RX_DESC * sizeof(e1000_rx_desc_t));
    e1000_reg_write(E1000_RDH, 0);
    e1000_reg_write(E1000_RDT, E1000_NUM_RX_DESC - 2); /* N-2 avoids "full" ring trap */

    /* Set TX ring base */
    e1000_reg_write(E1000_TDBAL, (uint32_t)(e1000_tx_ring_phys & 0xFFFFFFFF));
    e1000_reg_write(E1000_TDBAH, (uint32_t)((e1000_tx_ring_phys >> 32) & 0xFFFFFFFF));
    e1000_reg_write(E1000_TDLEN, E1000_NUM_TX_DESC * sizeof(e1000_tx_desc_t));
    e1000_reg_write(E1000_TDH, 0);
    e1000_reg_write(E1000_TDT, 0);

    /* Configure RX: enable, promiscuous, broadcast accept, 2048 byte buffers */
    uint32_t rctl = E1000_RCTL_EN | E1000_RCTL_UPE | E1000_RCTL_MPE |
                    E1000_RCTL_BAM | E1000_RCTL_BSIZE_2048 | E1000_RCTL_SECRC;
    /* Also set SBP to accept packets with errors */
    rctl |= E1000_RCTL_SBP;
    e1000_reg_write(E1000_RCTL, rctl);
    /* Read back to verify */
    kprintf("[E1000] RCTL=0x%08x\n", e1000_reg_read(E1000_RCTL));

    /* Re-write RDT to let QEMU know buffers are available */
    e1000_reg_write(E1000_RDT, E1000_NUM_RX_DESC - 2);

    /* Configure TX: enable, pad short packets, collision threshold */
    uint32_t tctl = E1000_TCTL_EN | E1000_TCTL_PSP |
                    (0x10 << E1000_TCTL_CT_SH) |
                    (0x40 << E1000_TCTL_COLD_SH);
    e1000_reg_write(E1000_TCTL, tctl);

    /* Set link-up in CTRL */
    uint32_t ctrl = e1000_reg_read(E1000_CTRL);
    ctrl |= E1000_CTRL_SLU;
    e1000_reg_write(E1000_CTRL, ctrl);

    uint32_t sreg = e1000_reg_read(E1000_STATUS);
    if (!(sreg & E1000_STATUS_LU)) {
        kprintf("[E1000] WARNING: Link is down!\n");
    }

    /* Clear all interrupts, then enable interesting ones */
    (void)e1000_reg_read(0x00C0); /* ICR */
    e1000_reg_write(E1000_IMS, E1000_ICR_TXDW | E1000_ICR_TXQE |
                                E1000_ICR_LSC | E1000_ICR_RXDMT0 |
                                E1000_ICR_RXT0);

    kprintf("[E1000] STATS: TPT=%u GPRC=%u TPR=%u\n",
            e1000_reg_read(E1000_TPT), e1000_reg_read(E1000_GPRC),
            e1000_reg_read(E1000_TPR));

    /* Store IRQ */
    e1000_irq = dev->irq_line;
    e1000_present = 1;

    kprintf("[E1000] Found Intel 82540EM at %02d:%02d.%d\n",
            dev->bus, dev->slot, dev->func);
    kprintf("[E1000] MAC = %02x:%02x:%02x:%02x:%02x:%02x\n",
            nic.mac[0], nic.mac[1], nic.mac[2],
            nic.mac[3], nic.mac[4], nic.mac[5]);
    kprintf("[E1000] RX=%d desc, TX=%d desc, IRQ=%d\n",
            E1000_NUM_RX_DESC, E1000_NUM_TX_DESC, e1000_irq);

    return ERR_OK;
}

/* ── NIC abstraction ────────────────────────────────────────────────────── */
nic_t nic;

static inline cpu_flags_t e1000_tx_lock_acquire(void) {
    cpu_flags_t _eflags;
    asm volatile("pushfq; popq %0; cli" : "=r"(_eflags));
    while (__sync_lock_test_and_set(&e1000_tx_lock, 1)) {
        asm volatile("sti; pause; cli");
    }
    return _eflags;
}

static inline void e1000_tx_lock_release(cpu_flags_t flags) {
    __sync_lock_release(&e1000_tx_lock);
    if (flags & 0x200) asm volatile("sti");
}

static err_t e1000_send(const struct nic* nic, const uint8_t* frame, uint32_t len) {
    (void)nic;
    if (!e1000_present) return ERR_IO;
    if (len > E1000_TX_BUF_SIZE) return ERR_INVAL;
    if (len < 64) len = 64;

    int tx_next;
    /* Longer timeout for virtual NIC where DD bit may be delayed */
    int timeout = 5000000;

    /* Wait for descriptor to be available (no lock held) */
    while (1) {
        tx_next = e1000_tx_head % E1000_NUM_TX_DESC;
        __asm__ volatile("clflush %0" : "+m" (e1000_tx_ring[tx_next].status));
        __asm__ volatile("" ::: "memory");
        if (e1000_tx_ring[tx_next].status & E1000_TXD_STAT_DD)
            break;
        hal_udelay(1);
        if (--timeout == 0) {
            kprintf("[E1000] TX timeout on desc %d (head=%d)\n", tx_next, e1000_tx_head);
            return ERR_TIMEOUT;
        }
    }

    /* Critical section: claim descriptor, copy data, advance head */
    cpu_flags_t _txfl = e1000_tx_lock_acquire();

    /* Re-check head in case another thread advanced it */
    tx_next = e1000_tx_head % E1000_NUM_TX_DESC;

    /* Uncomment for per-packet E1000 TX debugging:
    KDEBUG("[E1000 SEND] head=%d tx_next=%d len=%u status=0x%x\n",
            e1000_tx_head, tx_next, len,
            e1000_tx_ring[tx_next].status);
    */

    /* Copy frame to bounce buffer */
    kmemcpy(e1000_tx_bufs[tx_next], frame, len);
    e1000_tx_ring[tx_next].length = (uint16_t)len;
    e1000_tx_ring[tx_next].cmd = E1000_TXD_CMD_EOP | E1000_TXD_CMD_IFCS | E1000_TXD_CMD_RS;
    e1000_tx_ring[tx_next].status = 0;

    /* Flush descriptor from cache so that QEMU reads the latest data */
    __sync_synchronize();
    __asm__ volatile("clflush %0" : "+m" (e1000_tx_ring[tx_next]));
    __asm__ volatile("" ::: "memory");

    e1000_tx_head++;
    e1000_reg_write(E1000_TDT, e1000_tx_head % E1000_NUM_TX_DESC);

    e1000_tx_lock_release(_txfl);
    return ERR_OK;
}

static int e1000_poll(const struct nic* nic, uint8_t* buf, uint32_t max_len) {
    (void)nic;
    if (!e1000_present) return -1;

    int idx = e1000_rx_cur % E1000_NUM_RX_DESC;

    /* QEMU writes DD=1 to physical RAM during the VM exit above.
     * Our CPU cache may still have the old value (DD=0), so flush
     * the descriptor cache line before reading. */
    __asm__ volatile("clflush %0" : "+m" (e1000_rx_ring[idx].status));
    __asm__ volatile("" ::: "memory");

    if (!(e1000_rx_ring[idx].status & E1000_RXD_STAT_DD))
        return 0; /* No frame available */

    uint16_t len = e1000_rx_ring[idx].length;
    if (len > max_len) len = (uint16_t)max_len;

    /* Copy data before releasing descriptor (QEMU may reuse buffer after DD cleared) */
    kmemcpy(buf, e1000_rx_bufs[idx], len);

    /* Critical section: release descriptor back to hardware atomically.
     * No kprintf/context-switch between these ops — kprintf can trigger
     * UART interrupt → schedule(), and another thread entering e1000_poll
     * with stale e1000_rx_cur would double-process the same descriptor. */
    e1000_rx_ring[idx].status = 0;
    e1000_rx_cur++;
    __sync_synchronize();
    /* Refresh RDT to tell QEMU that descriptors up to `e1000_rx_cur - 1` are
     * free again.  We keep RDT = (e1000_rx_cur + E1000_NUM_RX_DESC - 2) % N
     * so QEMU always has at least N-2 available descriptors in front of
     * its internal RDH. */
    uint32_t new_rdt = (e1000_rx_cur + E1000_NUM_RX_DESC - 2) % E1000_NUM_RX_DESC;
    e1000_reg_write(E1000_RDT, new_rdt);

    /* Uncomment for per-packet E1000 RX debugging:
    KDEBUG("[E1000] RX desc=%d len=%u status=0x%x errors=0x%x\n",
            idx, len, e1000_rx_ring[idx].status, e1000_rx_ring[idx].errors);
    */

    return (int)len;
}

/* ── IRQ handler ────────────────────────────────────────────────────────── */
void nic_handle_irq(int_frame_t* frame, void* data) {
    (void)frame;
    (void)data;
    if (!e1000_present) return;

    uint32_t icr = e1000_reg_read(0x00C0); /* ICR */
    if (icr & E1000_ICR_LSC) {
        uint32_t status = e1000_reg_read(E1000_STATUS);
        kprintf("[E1000] Link %s, %s duplex\n",
                (status & E1000_STATUS_LU) ? "up" : "down",
                (status & E1000_STATUS_FD) ? "full" : "half");
    }
    if (icr & (E1000_ICR_RXT0 | E1000_ICR_RXDMT0)) {
        eth_rx_poll();
    }
}

/* ── Debug register access ──────────────────────────────────────────────── */
uint32_t nic_reg_read(uint32_t off) {
    if (!e1000_regs) return 0xFFFFFFFF;
    return e1000_reg_read(off);
}
void nic_reg_write(uint32_t off, uint32_t val) {
    if (!e1000_regs) return;
    e1000_reg_write(off, val);
}
void nic_dump_rx_ring(void) {
    if (!e1000_present) return;
    int rdh = e1000_reg_read(E1000_RDH);
    int rdt = e1000_reg_read(E1000_RDT);
    (void)rdh; (void)rdt;
    KDEBUG("[E1000] RX ring: RDH=%d RDT=%d cur=%d\n", rdh, rdt, e1000_rx_cur);
}
void nic_dump_tx_ring(void) {
    if (!e1000_present) return;
    KDEBUG("[E1000] TX ring: TDH=%d TDT=%d head=%d tail=%d\n",
            e1000_reg_read(E1000_TDH), e1000_reg_read(E1000_TDT),
            e1000_tx_head, e1000_tx_tail);
}

/* ── Multicast Table Array programming ──────────────────────────────────── */
void e1000_mta_set(const uint8_t* mac) {
    uint32_t crc = 0xFFFFFFFF;
    for (int i = 0; i < 6; i++) {
        crc ^= mac[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 1)
                crc = (crc >> 1) ^ 0xEDB88320;
            else
                crc >>= 1;
        }
    }
    crc = ~crc;
    uint32_t hash_idx = (crc >> 20) & 0xFFF;
    uint32_t reg_idx = hash_idx / 32;
    uint32_t bit_pos = hash_idx % 32;
    uint32_t reg = e1000_reg_read(E1000_MTA + reg_idx * 4);
    reg |= (1 << bit_pos);
    e1000_reg_write(E1000_MTA + reg_idx * 4, reg);
}

/* ── Public API ─────────────────────────────────────────────────────────── */
err_t nic_init(void) {
    kmemset(&nic, 0, sizeof(nic));

    /* Find E1000 PCI device */
    pci_device_t* e1000_dev = NULL;
    int n = pci_device_count();
    for (int i = 0; i < n; i++) {
        pci_device_t* dev = pci_get_device(i);
        if (!dev) continue;
        if (dev->class_code == PCI_CLASS_NETWORK && dev->subclass == 0x00) {
            if (dev->vendor_id == E1000_VENDOR_INTEL &&
                (dev->device_id == E1000_DEV_82540EM ||
                 dev->device_id == E1000_DEV_82545EM ||
                 dev->device_id == E1000_DEV_82573L ||
                 dev->device_id == E1000_DEV_82574L)) {
                e1000_dev = dev;
                break;
            }
        }
    }

    if (!e1000_dev) {
        kprintf("[NIC] No supported Ethernet controller found\n");
        nic.present = 0;
        return ERR_NOENT;
    }

    err_t err = e1000_init_nic(e1000_dev);
    if (err != ERR_OK) {
        kprintf("[NIC] E1000 init failed: %d\n", err);
        nic.present = 0;
        return err;
    }

    /* Register IRQ handler */
    err = hal_irq_register((uint8_t)e1000_irq, nic_handle_irq, NULL);
    if (err != ERR_OK) {
        kprintf("[NIC] IRQ registration failed: %d\n", err);
    }

    /* Unmask IRQ on PIC if APIC not available */
    if (!apic_present) {
        uint16_t mask = inb(0xA1);
        mask = (uint16_t)(mask << 8);
        mask |= inb(0x21);
        mask &= (uint16_t)~(1 << e1000_irq);
        outb(0x21, mask & 0xFF);
        outb(0xA1, (mask >> 8) & 0xFF);
    }

    /* Wire up NIC abstraction */
    nic.present = 1;
    nic.irq     = e1000_irq;
    nic.send    = e1000_send;
    nic.poll    = e1000_poll;

    return ERR_OK;
}

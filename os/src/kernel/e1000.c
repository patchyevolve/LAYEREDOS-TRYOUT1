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
static void*              e1000_tx_ring = NULL;
static void*              e1000_rx_ring = NULL;
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
static e1000_model_t     e1000_model  = E1000_MODEL_LEGACY;
static int               e1000_msix_on = 0;
static volatile uint32_t e1000_msix_irq_count = 0;

/* Forward declarations */
static err_t e1000_send(const struct nic* nic, const uint8_t* frame, uint32_t len);
static int   e1000_poll(const struct nic* nic, uint8_t* buf, uint32_t max_len);

/* ── Model identification ───────────────────────────────────────────────── */
const char* e1000_model_name(e1000_model_t m) {
    switch (m) {
    case E1000_MODEL_E1000E: return "e1000e";
    case E1000_MODEL_IGB:    return "igb";
    default:                 return "legacy";
    }
}

e1000_model_t e1000_model_for_devid(uint16_t devid) {
    switch (devid) {
    case E1000_DEV_82571EB:
    case E1000_DEV_82572EI:
    case E1000_DEV_82573L:
    case E1000_DEV_82574L:
    case E1000_DEV_82579V:
    case E1000_DEV_I217V:
    case E1000_DEV_I218V:
    case E1000_DEV_I219V:
        return E1000_MODEL_E1000E;
    case E1000_DEV_82576:
    case E1000_DEV_I350:
    case E1000_DEV_I210:
    case E1000_DEV_I211:
        return E1000_MODEL_IGB;
    default:
        return E1000_MODEL_LEGACY;
    }
}

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

/* ── PHY auto-negotiation restart (e1000e/igb) ─────────────────────────── */
static void e1000_phy_autoneg_restart(void) {
    /* QEMU's e1000e/igb models use the MDIC OP bits SWAPPED vs. real Intel
     * silicon (QEMU: WRITE=0x04000000, READ=0x08000000; Intel: READ=0x04000000,
     * WRITE=0x08000000). Detect QEMU via CPUID hypervisor bit and pick the
     * matching convention. */
    uint32_t op_write = hal_is_qemu() ? 0x04000000u : E1000_MDIC_OP_WRITE;
    uint32_t mdic = (1 << E1000_MDIC_PHY_SHIFT) |
                    (0 << E1000_MDIC_REG_SHIFT) |   /* MII BMCR = reg 0 */
                    (E1000_BMCR_SPEED1000 | E1000_BMCR_FD |
                     E1000_BMCR_AUTOEN | E1000_BMCR_ANRESTART) |
                    op_write;
    e1000_reg_write(E1000_MDIC, mdic);
    /* Poll for transaction completion (~20-30 us on real hardware) */
    for (int i = 0; i < 1000; i++) {
        hal_udelay(10);
        if (e1000_reg_read(E1000_MDIC) & E1000_MDIC_READY) break;
    }
}

/* ── Wait for link up, returns 1 on success ────────────────────────────── */
static int e1000_wait_link(int timeout_ms) {
    for (int i = 0; i < timeout_ms; i += 50) {
        if (e1000_reg_read(E1000_STATUS) & E1000_STATUS_LU) return 1;
        hal_udelay(50000);
    }
    return e1000_reg_read(E1000_STATUS) & E1000_STATUS_LU ? 1 : 0;
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
        if (e1000_model == E1000_MODEL_IGB) {
            /* Advanced single-buffer Rx descriptor: pkt_addr at offset 0 */
            ((e1000_adv_rx_desc_t*)e1000_rx_ring)[i].pkt_addr = e1000_rx_bufs_phys[i];
        } else {
            ((e1000_rx_desc_t*)e1000_rx_ring)[i].addr = e1000_rx_bufs_phys[i];
            ((e1000_rx_desc_t*)e1000_rx_ring)[i].status = 0;
        }
    }

    /* Allocate TX bounce buffers */
    for (int i = 0; i < E1000_NUM_TX_DESC; i++) {
        e1000_tx_bufs[i] = e1000_alloc_page(&e1000_tx_bufs_phys[i]);
        if (!e1000_tx_bufs[i]) return ERR_NOMEM;
        if (e1000_model == E1000_MODEL_IGB) {
            /* Advanced Tx descriptor: buffer_addr@0, cmd_type_len@8,
             * olinfo_status@12; HW writeback zeroes the whole descriptor
             * and sets DD in the write-back word (offset 12). Pre-mark
             * every descriptor free with DD=1 at that offset. */
            e1000_adv_tx_desc_t* d = &((e1000_adv_tx_desc_t*)e1000_tx_ring)[i];
            d->buffer_addr = e1000_tx_bufs_phys[i];
            d->cmd_type_len = 0;
            d->olinfo_status = E1000_TXD_STAT_DD;
        } else {
            ((e1000_tx_desc_t*)e1000_tx_ring)[i].addr = e1000_tx_bufs_phys[i];
            ((e1000_tx_desc_t*)e1000_tx_ring)[i].status = E1000_TXD_STAT_DD;
        }
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

    /* e1000e/igb family setup */
    if (e1000_model == E1000_MODEL_IGB) {
        /* Advanced (extended) descriptors: default type must match the
         * descriptor formats used by e1000_send()/e1000_poll() below.
         * QEMU's igb model only implements the advanced formats. */
        uint32_t ctrl_ext = e1000_reg_read(E1000_CTRL_EXT);
        ctrl_ext |= E1000_CTRL_EXT_EXTDE;
        e1000_reg_write(E1000_CTRL_EXT, ctrl_ext);
    } else if (e1000_model == E1000_MODEL_E1000E) {
        /* e1000e (I217/I218/I219/82579/82573/82574) has no advanced
         * descriptor support; keep the legacy 16-byte formats explicit */
        uint32_t ctrl_ext = e1000_reg_read(E1000_CTRL_EXT);
        ctrl_ext &= ~E1000_CTRL_EXT_EXTDE;
        e1000_reg_write(E1000_CTRL_EXT, ctrl_ext);
    }
    if (e1000_model != E1000_MODEL_LEGACY) {
        /* 1 Gb/s link: program IPG (IPGT=8, IPGR1=8, IPGR2=2) */
        e1000_reg_write(E1000_TIPG, E1000_TIPG_1000);
        /* Restart PHY auto-negotiation. Required after a soft reset: real
         * silicon needs it to bring the link up, and QEMU's igb model
         * cancels its autoneg timer on guest software reset and only
         * re-arms it on a MDIC write of BMCR.ANRESTART. */
        e1000_phy_autoneg_restart();
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
    /* igb: select advanced single-buffer Rx descriptors (SRRCTL0 shares the
     * RCTL register offset on 82576/I210; QEMU igb ignores this) */
    if (e1000_model == E1000_MODEL_IGB)
        rctl |= E1000_SRRCTL_DESCTYPE_ADV_ONEBUF;
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
        if (e1000_model != E1000_MODEL_LEGACY) {
            /* PHY auto-negotiation: ~5-10 ms on QEMU models, up to a few
             * seconds on real hardware */
            if (e1000_wait_link(2000))
                kprintf("[E1000] Link up (PHY auto-negotiation)\n");
        }
        if (!(e1000_reg_read(E1000_STATUS) & E1000_STATUS_LU)) {
            kprintf("[E1000] WARNING: Link is down!\n");
        }
    }

    /* MSI-X delivery for the e1000e/igb family (classic 82540/82545 has no
     * MSI-X/MSI capability — legacy INTx IRQ + PIC path stays for those).
     * QEMU e1000e/igb: MSI-X table in BAR3 at offset 0; igb falls back to
     * MSI (capability at 0x50) if MSI-X is absent. */
    {
        pci_msix_info_t msix;
        if (pci_msix_probe(dev, &msix) == ERR_OK) {
            err_t me = pci_msix_enable(dev, &msix, PCI_MSIX_VECTOR);
            if (me == ERR_OK) {
                e1000_msix_on = 1;
                kprintf("[E1000] MSI-X enabled: %u entries, BAR%d+0x%x, "
                        "vector 0x%02x (irq %d)\n",
                        msix.table_size, msix.table_bar, msix.table_offset,
                        PCI_MSIX_VECTOR, PCI_MSIX_IRQ);
            } else {
                kprintf("[E1000] MSI-X enable failed: %d\n", me);
            }
        } else if (pci_msi_enable(dev, PCI_MSIX_VECTOR) == ERR_OK) {
            e1000_msix_on = 1;
            kprintf("[E1000] MSI enabled: vector 0x%02x (irq %d)\n",
                    PCI_MSIX_VECTOR, PCI_MSIX_IRQ);
        }

        /* e1000e: per-cause MSI-X routing requires valid IVAR entries.
         * Real 82574: IVAR@0x1700, entry valid bit 0x80 → 0x00888880.
         * QEMU model: IVAR@0xE4, entry valid bit 0x8 → 0x00088888.
         * Each write is a no-op on the other platform. */
        if (e1000_msix_on && e1000_model == E1000_MODEL_E1000E) {
            e1000_reg_write(E1000_IVAR, 0x00888880);
            if (hal_is_qemu())
                e1000_reg_write(E1000_IVAR_QEMU, 0x00088888);
        }
    }

    /* Clear all interrupts, then enable interesting ones */
    (void)e1000_reg_read(0x00C0); /* ICR */
    uint32_t ims = E1000_ICR_TXDW | E1000_ICR_TXQE |
                   E1000_ICR_LSC | E1000_ICR_RXDMT0 |
                   E1000_ICR_RXT0;
    if (e1000_model == E1000_MODEL_E1000E)
        ims |= E1000_ICR_RXQ0;  /* e1000e raises RXQ0, not RXT0 */
    e1000_reg_write(E1000_IMS, ims);

    kprintf("[E1000] STATS: TPT=%u GPRC=%u TPR=%u\n",
            e1000_reg_read(E1000_TPT), e1000_reg_read(E1000_GPRC),
            e1000_reg_read(E1000_TPR));

    /* Store IRQ */
    e1000_irq = dev->irq_line;
    e1000_present = 1;

    kprintf("[E1000] Found Intel %s (%x:%x) at %02d:%02d.%d\n",
            e1000_model_name(e1000_model),
            dev->vendor_id, dev->device_id, dev->bus, dev->slot, dev->func);
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

    int igb_mode = (e1000_model == E1000_MODEL_IGB);
    e1000_tx_desc_t*     ltx = (e1000_tx_desc_t*)e1000_tx_ring;
    e1000_adv_tx_desc_t* atx = (e1000_adv_tx_desc_t*)e1000_tx_ring;

    int tx_next;
    /* Longer timeout for virtual NIC where DD bit may be delayed */
    int timeout = 5000000;

    /* Wait for descriptor to be available (no lock held) */
    while (1) {
        tx_next = e1000_tx_head % E1000_NUM_TX_DESC;
        if (igb_mode) {
            volatile uint32_t* st =
                (volatile uint32_t*)((uint8_t*)&atx[tx_next] + 12);
            __asm__ volatile("clflush %0" : "+m" (*st));
            __asm__ volatile("" ::: "memory");
            if (*st & E1000_TXD_STAT_DD) break;
        } else {
            __asm__ volatile("clflush %0" : "+m" (ltx[tx_next].status));
            __asm__ volatile("" ::: "memory");
            if (ltx[tx_next].status & E1000_TXD_STAT_DD)
                break;
        }
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
            ltx[tx_next].status);
    */

    /* Copy frame to bounce buffer */
    kmemcpy(e1000_tx_bufs[tx_next], frame, len);
    if (igb_mode) {
        atx[tx_next].cmd_type_len = (len & 0xFFFF) |
                                    E1000_ADVTXD_DCMD_DEXT |
                                    E1000_ADVTXD_DTYP_DATA |
                                    E1000_ADVTXD_DCMD_EOP |
                                    E1000_ADVTXD_DCMD_IFCS |
                                    E1000_ADVTXD_DCMD_RS;
        atx[tx_next].buffer_addr = e1000_tx_bufs_phys[tx_next];
        atx[tx_next].olinfo_status = 0;
    } else {
        ltx[tx_next].length = (uint16_t)len;
        ltx[tx_next].cmd = E1000_TXD_CMD_EOP | E1000_TXD_CMD_IFCS | E1000_TXD_CMD_RS;
        ltx[tx_next].status = 0;
    }

    /* Flush descriptor from cache so that the NIC reads the latest data */
    __sync_synchronize();
    if (igb_mode)
        __asm__ volatile("clflush %0" : "+m" (atx[tx_next].cmd_type_len));
    else
        __asm__ volatile("clflush %0" : "+m" (ltx[tx_next]));
    __asm__ volatile("" ::: "memory");

    e1000_tx_head++;
    e1000_reg_write(E1000_TDT, e1000_tx_head % E1000_NUM_TX_DESC);

    e1000_tx_lock_release(_txfl);
    return ERR_OK;
}

static int e1000_poll(const struct nic* nic, uint8_t* buf, uint32_t max_len) {
    (void)nic;
    if (!e1000_present) return -1;

    int igb_mode = (e1000_model == E1000_MODEL_IGB);
    e1000_rx_desc_t*     lrx = (e1000_rx_desc_t*)e1000_rx_ring;
    e1000_adv_rx_desc_t* arx = (e1000_adv_rx_desc_t*)e1000_rx_ring;

    int idx = e1000_rx_cur % E1000_NUM_RX_DESC;

    /* QEMU writes DD=1 to physical RAM during the VM exit above.
     * Our CPU cache may still have the old value (DD=0), so flush
     * the descriptor cache line before reading. */
    uint16_t len;
    if (igb_mode) {
        __asm__ volatile("clflush %0" : "+m" (arx[idx].status_error));
        __asm__ volatile("" ::: "memory");
        if (!(arx[idx].status_error & E1000_RXD_STAT_DD))
            return 0; /* No frame available */
        len = arx[idx].length;
        if (len > max_len) len = (uint16_t)max_len;
        /* Copy data before releasing descriptor (NIC may reuse buffer after DD cleared) */
        kmemcpy(buf, e1000_rx_bufs[idx], len);
        /* Release descriptor back to hardware. No kprintf/context-switch
         * between these ops — kprintf can trigger UART interrupt →
         * schedule(), and another thread entering e1000_poll with stale
         * e1000_rx_cur would double-process the same descriptor. */
        arx[idx].length = 0;
        arx[idx].status_error = 0;
        arx[idx].vlan = 0;
    } else {
        __asm__ volatile("clflush %0" : "+m" (lrx[idx].status));
        __asm__ volatile("" ::: "memory");
        if (!(lrx[idx].status & E1000_RXD_STAT_DD))
            return 0; /* No frame available */
        len = lrx[idx].length;
        if (len > max_len) len = (uint16_t)max_len;
        /* Copy data before releasing descriptor (QEMU may reuse buffer after DD cleared) */
        kmemcpy(buf, e1000_rx_bufs[idx], len);
        /* Critical section: release descriptor back to hardware atomically.
         * No kprintf/context-switch between these ops — kprintf can trigger
         * UART interrupt → schedule(), and another thread entering e1000_poll
         * with stale e1000_rx_cur would double-process the same descriptor. */
        lrx[idx].status = 0;
    }
    e1000_rx_cur++;
    __sync_synchronize();
    /* Refresh RDT to tell the NIC that descriptors up to `e1000_rx_cur - 1`
     * are free again.  We keep RDT = (e1000_rx_cur + E1000_NUM_RX_DESC - 2) % N
     * so the NIC always has at least N-2 available descriptors in front of
     * its internal RDH. */
    uint32_t new_rdt = (e1000_rx_cur + E1000_NUM_RX_DESC - 2) % E1000_NUM_RX_DESC;
    e1000_reg_write(E1000_RDT, new_rdt);

    /* Uncomment for per-packet E1000 RX debugging:
    KDEBUG("[E1000] RX desc=%d len=%u status=0x%x errors=0x%x\n",
            idx, len, lrx[idx].status, lrx[idx].errors);
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

/* MSI-X/MSI delivery handler. Deliberately minimal: ack the device (ICR
 * read clears pending causes) and count — the NIC poll thread drains the
 * RX ring as usual. Calling eth_rx_poll() here would risk spinning on
 * eth_lock while the poll thread holds it (ISR context, IF=0). */
static void e1000_msix_isr(int_frame_t* frame, void* data) {
    (void)frame;
    (void)data;
    if (!e1000_present || !e1000_msix_on) return;
    e1000_msix_irq_count++;
    if (e1000_msix_irq_count == 1)
        kprintf("[E1000] MSI-X interrupt received (vector 0x%02x)\n",
                PCI_MSIX_VECTOR);
    (void)e1000_reg_read(0x00C0); /* ICR — ack/clear pending causes */
}

int e1000_msix_active(void) { return e1000_msix_on; }
uint32_t e1000_msix_count(void) { return e1000_msix_irq_count; }

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
        if (dev->class_code == PCI_CLASS_NETWORK && dev->subclass == 0x00 &&
            dev->vendor_id == E1000_VENDOR_INTEL) {
            e1000_model_t m = e1000_model_for_devid(dev->device_id);
            if (m != E1000_MODEL_LEGACY ||
                dev->device_id == E1000_DEV_82540EM ||
                dev->device_id == E1000_DEV_82545EM) {
                e1000_dev = dev;
                e1000_model = m;
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

    if (e1000_msix_on) {
        /* MSI-X/MSI vector: no PIC involvement, no legacy INTx line */
        err = hal_irq_register(PCI_MSIX_IRQ, e1000_msix_isr, NULL);
        if (err != ERR_OK)
            kprintf("[NIC] MSI-X IRQ registration failed: %d\n", err);
    } else {
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
    }

    /* Wire up NIC abstraction */
    nic.present = 1;
    nic.irq     = e1000_msix_on ? PCI_MSIX_IRQ : e1000_irq;
    nic.send    = e1000_send;
    nic.poll    = e1000_poll;

    return ERR_OK;
}

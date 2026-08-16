#include "kernel.h"
#include "pci.h"
#include "hal.h"
#include "sync.h"
#include "acpi.h"
#include "vmm.h"
#include "apic.h"

static pci_device_t pci_devices[MAX_PCI_DEVICES];
static int pci_count = 0;
static spinlock_t pci_lock;

/* PCIe ECAM state (from ACPI MCFG); inactive (0) on legacy-only platforms */
static uint64_t pci_ecam_base = 0;
static int pci_ecam_start_bus = 0;
static int pci_ecam_end_bus = -1;

static void pci_out_config_addr(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t addr = (uint32_t)1 << 31
                  | (uint32_t)bus << 16
                  | (uint32_t)slot << 11
                  | (uint32_t)func << 8
                  | (offset & 0xFC);
    outl(PCI_CONFIG_ADDR, addr);
}

uint32_t pci_config_read_legacy(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    cpu_flags_t _sf;
    spinlock_acquire(&pci_lock, &_sf);
    pci_out_config_addr(bus, slot, func, offset);
    uint32_t v = inl(PCI_CONFIG_DATA);
    spinlock_release(&pci_lock, _sf);
    return v;
}

void pci_config_write_legacy(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value) {
    cpu_flags_t _sf;
    spinlock_acquire(&pci_lock, &_sf);
    pci_out_config_addr(bus, slot, func, offset);
    outl(PCI_CONFIG_DATA, value);
    spinlock_release(&pci_lock, _sf);
}

/* Map the 4 KB page containing phys (MMIO); returns its virtual address. */
static uint64_t pci_map_mmio_page(uint64_t phys) {
    uint64_t virt = PHYS_TO_VIRT(phys & ~0xFFFULL);
    page_entry_t* pte = vmm_walk_pagetable(vmm_get_kernel_pml4(), virt);
    if (!pte) {
        vmm_map_page(vmm_get_kernel_pml4(), virt, phys & ~0xFFFULL,
                     PAGE_PRESENT | PAGE_WRITE | PAGE_NX);
    }
    return virt;
}

int pci_ecam_active(void) {
    return pci_ecam_base != 0;
}

static int pci_ecam_in_range(uint8_t bus) {
    return pci_ecam_active() && bus >= pci_ecam_start_bus && bus <= pci_ecam_end_bus;
}

static uint32_t pci_ecam_read(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint64_t phys = pci_ecam_base
                  + ((uint64_t)bus << 20)
                  + ((uint64_t)slot << 15)
                  + ((uint64_t)func << 12);
    uint64_t virt = pci_map_mmio_page(phys) + (phys & 0xFFF) + (offset & 0xFC);
    return *(volatile uint32_t*)virt;
}

static void pci_ecam_write(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value) {
    uint64_t phys = pci_ecam_base
                  + ((uint64_t)bus << 20)
                  + ((uint64_t)slot << 15)
                  + ((uint64_t)func << 12);
    uint64_t virt = pci_map_mmio_page(phys) + (phys & 0xFFF) + (offset & 0xFC);
    *(volatile uint32_t*)virt = value;
}

uint32_t pci_config_read(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    if (pci_ecam_in_range(bus))
        return pci_ecam_read(bus, slot, func, offset);
    return pci_config_read_legacy(bus, slot, func, offset);
}

void pci_config_write(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value) {
    if (pci_ecam_in_range(bus)) {
        pci_ecam_write(bus, slot, func, offset, value);
        return;
    }
    pci_config_write_legacy(bus, slot, func, offset, value);
}

static err_t pci_ecam_init(void) {
    if (acpi_parse_mcfg() != 0) return ERR_NOENT;
    pci_ecam_base = mcfg_base_addr;
    pci_ecam_start_bus = mcfg_start_bus;
    pci_ecam_end_bus = mcfg_end_bus;
    kprintf("[PCI] ECAM enabled: base=0x%llx bus=%d-%d\n",
            (unsigned long long)pci_ecam_base, pci_ecam_start_bus, pci_ecam_end_bus);
    return ERR_OK;
}

static void pci_read_device(uint8_t bus, uint8_t slot, uint8_t func, pci_device_t* dev) {
    uint32_t id = pci_config_read(bus, slot, func, PCI_VENDOR_ID);
    dev->vendor_id = id & 0xFFFF;
    dev->device_id = (id >> 16) & 0xFFFF;

    uint32_t class_rev = pci_config_read(bus, slot, func, PCI_REVISION);
    dev->rev_id = class_rev & 0xFF;
    dev->prog_if = (class_rev >> 8) & 0xFF;
    dev->subclass = (class_rev >> 16) & 0xFF;
    dev->class_code = (class_rev >> 24) & 0xFF;

    uint32_t hdr = pci_config_read(bus, slot, func, PCI_HEADER_TYPE);
    dev->header_type = hdr & 0xFF;

    dev->bus = bus;
    dev->slot = slot;
    dev->func = func;

    for (int i = 0; i < 6; i++) {
        dev->bar[i] = pci_config_read(bus, slot, func, PCI_BAR0 + i * 4);
    }

    uint32_t irq_reg = pci_config_read(bus, slot, func, PCI_INTERRUPT_LINE);
    dev->irq_line = irq_reg & 0xFF;
}

void pci_enable_bus_mastering(pci_device_t* dev) {
    uint32_t cmd = pci_config_read(dev->bus, dev->slot, dev->func, PCI_COMMAND);
    cmd |= (1 << 2) | (1 << 0);
    pci_config_write(dev->bus, dev->slot, dev->func, PCI_COMMAND, cmd);
}

/* ── MSI / MSI-X ────────────────────────────────────────────────────────── */

err_t pci_find_cap(uint8_t bus, uint8_t slot, uint8_t func, uint8_t cap_id, uint8_t* out_offset) {
    /* NOTE: pci_config_read() reads the 4-byte-aligned word below the
     * requested offset (offset & 0xFC). Reading PCI_STATUS (0x06) returns
     * the word at 0x04: COMMAND in bits 15:0, STATUS in bits 31:16. The
     * capabilities-list bit is STATUS bit 4 = value bit 20. */
    uint32_t sts = pci_config_read(bus, slot, func, PCI_STATUS);
    if (!(sts & (1 << 20))) return ERR_NOENT;   /* capabilities list absent */
    /* Header type is byte 0x0D → bits 15:8 of the word at 0x0C */
    if (((pci_config_read(bus, slot, func, PCI_HEADER_TYPE) >> 8) & 0x7F) != 0x00)
        return ERR_NOENT;                       /* type-0 header required */

    uint8_t off = (uint8_t)pci_config_read(bus, slot, func, PCI_CAP_PTR);
    for (int i = 0; i < 32 && off != 0; i++) {
        uint32_t reg = pci_config_read(bus, slot, func, off & 0xFC);
        uint8_t id = (uint8_t)(reg & 0xFF);
        uint8_t next = (uint8_t)((reg >> 8) & 0xFF);
        if (id == cap_id) {
            if (out_offset) *out_offset = off;
            return ERR_OK;
        }
        if (next == 0 || next == off) break;
        off = next;
    }
    return ERR_NOENT;
}

/* MSI-X fixed-delivery address: xAPIC MMIO base + BSP APIC ID in bits 19:12,
 * physical addressing (RH=0), no redirection (DM=0). */
static uint32_t pci_msix_addr(void) {
    return 0xFEE00000u | ((uint32_t)apic_id << 12);
}

err_t pci_msix_probe(pci_device_t* dev, pci_msix_info_t* out) {
    uint8_t off;
    if (pci_find_cap(dev->bus, dev->slot, dev->func, PCI_CAP_ID_MSIX, &off) != ERR_OK)
        return ERR_NOENT;

    /* msgctl sits at cap+2 → bits 31:16 of the aligned word at cap */
    uint32_t msgctl = pci_config_read(dev->bus, dev->slot, dev->func,
                                      (uint8_t)(off + 2)) >> 16;
    uint32_t table = pci_config_read(dev->bus, dev->slot, dev->func, (uint8_t)(off + 4));

    out->cap_offset = off;
    out->msgctl = (uint16_t)(msgctl & 0xFFFF);
    out->table_size = (uint16_t)((msgctl & 0x7FF) + 1);
    out->table_bar = (uint8_t)(table & 0x7);
    out->table_offset = table & ~0x7u;

    if (out->table_size == 0 || out->table_size > 32 ||
        out->table_bar > 5 || (out->table_offset & 0xFFF) != 0)
        return ERR_IO;

    return ERR_OK;
}

err_t pci_msix_enable(pci_device_t* dev, pci_msix_info_t* info, uint8_t vector) {
    /* Map the table page and program every entry with the same vector.
     * Programming all entries makes delivery independent of the device's
     * internal cause→vector routing (IVAR etc.). */
    uint64_t tbl_phys = (uint64_t)(dev->bar[info->table_bar] & ~0xF) + info->table_offset;
    uint64_t tbl_virt = pci_map_mmio_page(tbl_phys) + (tbl_phys & 0xFFF);
    volatile uint32_t* tbl = (volatile uint32_t*)tbl_virt;

    uint32_t addr = pci_msix_addr();
    for (int i = 0; i < info->table_size; i++) {
        volatile uint32_t* e = &tbl[i * 4];   /* 16-byte entry */
        e[0] = addr;    /* addr lo: 0xFEE00000 | dest<<12 */
        e[1] = 0;       /* addr hi (64-bit) */
        e[2] = vector;  /* data: fixed delivery mode, edge, no level */
        e[3] = 0;       /* vector control: unmasked */
    }
    __sync_synchronize();

    /* Enable MSI-X: set bit 15, clear function mask (bit 14); preserve the
     * read-only table-size field in bits 10:0. msgctl is the upper half of
     * the aligned word at cap — recombine without clobbering cap id/next. */
    uint32_t word = pci_config_read(dev->bus, dev->slot, dev->func, info->cap_offset);
    uint32_t msgctl = (word >> 16) & 0xFFFF;
    msgctl |= (1 << 15);
    msgctl &= ~(1 << 14);
    word = (word & 0xFFFF) | (msgctl << 16);
    pci_config_write(dev->bus, dev->slot, dev->func, info->cap_offset, word);

    /* INTx disable in the command register — MSI-X and legacy INTx must not
     * be active simultaneously. */
    uint32_t cmd = pci_config_read(dev->bus, dev->slot, dev->func, PCI_COMMAND);
    cmd |= (1 << 10);
    pci_config_write(dev->bus, dev->slot, dev->func, PCI_COMMAND, cmd);

    return ERR_OK;
}

err_t pci_msi_enable(pci_device_t* dev, uint8_t vector) {
    uint8_t off;
    if (pci_find_cap(dev->bus, dev->slot, dev->func, PCI_CAP_ID_MSI, &off) != ERR_OK)
        return ERR_NOENT;

    /* msgctl sits at cap+2 → bits 31:16 of the aligned word at cap */
    uint32_t word = pci_config_read(dev->bus, dev->slot, dev->func, off);
    uint32_t msgctl = (word >> 16) & 0xFFFF;
    int is64 = (msgctl >> 7) & 1;
    uint32_t addr = pci_msix_addr();

    if (is64) {
        pci_config_write(dev->bus, dev->slot, dev->func, (uint8_t)(off + 4), addr);
        pci_config_write(dev->bus, dev->slot, dev->func, (uint8_t)(off + 8), 0);
        pci_config_write(dev->bus, dev->slot, dev->func, (uint8_t)(off + 0x0C), vector);
    } else {
        pci_config_write(dev->bus, dev->slot, dev->func, (uint8_t)(off + 4), addr);
        pci_config_write(dev->bus, dev->slot, dev->func, (uint8_t)(off + 8), vector);
    }

    /* Enable MSI (bit 0), single vector (clear multiple-message enable 6:4);
     * recombine with cap id/next in the low half. */
    msgctl |= 1;
    msgctl &= ~(0x7 << 4);
    word = (word & 0xFFFF) | (msgctl << 16);
    pci_config_write(dev->bus, dev->slot, dev->func, off, word);

    uint32_t cmd = pci_config_read(dev->bus, dev->slot, dev->func, PCI_COMMAND);
    cmd |= (1 << 10);
    pci_config_write(dev->bus, dev->slot, dev->func, PCI_COMMAND, cmd);

    return ERR_OK;
}

static void pci_scan_bus(int bus);

static void pci_scan_function(uint8_t bus, uint8_t slot, uint8_t func) {
    if (pci_count >= MAX_PCI_DEVICES) return;

    pci_device_t* dev = &pci_devices[pci_count];
    pci_read_device(bus, slot, func, dev);

    if (dev->vendor_id == 0xFFFF || dev->vendor_id == 0x0000)
        return;

    pci_count++;

    if (dev->class_code == PCI_CLASS_BRIDGE && dev->subclass == 0x04) {
        uint32_t bus_reg = pci_config_read(bus, slot, func, 0x18);
        uint8_t secondary_bus = (bus_reg >> 8) & 0xFF;
        if (secondary_bus != bus)
            pci_scan_bus(secondary_bus);
    }
}

static void pci_scan_slot(uint8_t bus, uint8_t slot) {
    uint32_t id = pci_config_read(bus, slot, 0, PCI_VENDOR_ID);
    if (id == 0xFFFF || id == 0x0000) return;

    pci_scan_function(bus, slot, 0);

    uint32_t hdr = pci_config_read(bus, slot, 0, PCI_HEADER_TYPE);
    if (hdr & (1 << 7)) {
        for (int func = 1; func < 8; func++) {
            uint32_t fid = pci_config_read(bus, slot, func, PCI_VENDOR_ID);
            if (fid != 0xFFFF && fid != 0x0000)
                pci_scan_function(bus, slot, func);
        }
    }
}

static void pci_scan_bus(int bus) {
    for (int slot = 0; slot < 32; slot++)
        pci_scan_slot(bus, slot);
}

err_t pci_init(void) {
    kmemset(pci_devices, 0, sizeof(pci_devices));
    pci_count = 0;

    pci_ecam_init();

    uint32_t hdr = pci_config_read(0, 0, 0, PCI_HEADER_TYPE);
    if (hdr & (1 << 7)) {
        for (int bus = 0; bus < 256; bus++)
            pci_scan_bus(bus);
    } else {
        pci_scan_bus(0);
    }

    kprintf("[PCI] Enumerated %d devices\n", pci_count);
    for (int i = 0; i < pci_count; i++) {
        pci_device_t* d = &pci_devices[i];
        kprintf("  %02x:%02x.%d  %04x:%04x  class=%02x subclass=%02x irq=%d\n",
                d->bus, d->slot, d->func,
                d->vendor_id, d->device_id,
                d->class_code, d->subclass, d->irq_line);
    }
    return ERR_OK;
}

int pci_device_count(void) { return pci_count; }

pci_device_t* pci_get_device(int index) {
    if (index < 0 || index >= pci_count) return NULL;
    return &pci_devices[index];
}
